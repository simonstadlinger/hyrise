#include "disjoint_clusters_algo.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

#include "abstract_clustering_algo.hpp"
#include "hyrise.hpp"
#include "operators/sort.hpp"
#include "operators/table_wrapper.hpp"
#include "storage/chunk.hpp"
#include "storage/chunk_encoder.hpp"
#include "storage/segment_encoding_utils.hpp"
#include "storage/segment_iterate.hpp"
#include "storage/storage_manager.hpp"
#include "storage/table.hpp"
#include "storage/dictionary_segment.hpp"
#include "storage/value_segment.hpp"
#include "utils/format_duration.hpp"
#include "utils/timer.hpp"

#include "statistics/attribute_statistics.hpp"
#include "statistics/base_attribute_statistics.hpp"
#include "statistics/statistics_objects/abstract_histogram.hpp"
#include "statistics/table_statistics.hpp"


namespace opossum {

DisjointClustersAlgo::DisjointClustersAlgo(StorageManager& storage_manager, ClusteringByTable clustering) : AbstractClusteringAlgo(storage_manager, clustering) {}

const std::string DisjointClustersAlgo::description() const {
  return "DisjointClustersAlgo";
}

template <typename ColumnDataType>
std::shared_ptr<const AbstractHistogram<ColumnDataType>> DisjointClustersAlgo::_get_histogram(const std::shared_ptr<const Table>& table, const std::string& column_name) const {
  const auto table_statistics = table->table_statistics();        
  const auto column_id = table->column_id_by_name(column_name);  
  const auto base_attribute_statistics = table_statistics->column_statistics[column_id];
    
  const auto attribute_statistics = std::dynamic_pointer_cast<AttributeStatistics<ColumnDataType>>(base_attribute_statistics);
  Assert(attribute_statistics, "could not cast to AttributeStatistics");
  const auto histogram = attribute_statistics->histogram;  
  Assert(histogram, "no histogram available for column "  + column_name);
  return histogram;
}

// NOTE: num_clusters is just an estimate.
// The greedy logic that computes the boundaries currently sacrifices exact cluster count rather than balanced clusters
template <typename ColumnDataType>
std::vector<std::pair<AllTypeVariant, AllTypeVariant>> DisjointClustersAlgo::_get_boundaries(const std::shared_ptr<const AbstractHistogram<ColumnDataType>>& histogram, const size_t row_count, const size_t num_clusters) const {
  Assert(histogram, "histogram was null");
  Assert(histogram->total_count() == row_count, "NULL values are not yet supported");
  Assert(num_clusters > 1, "having less than 2 clusters does not make sense (" + std::to_string(num_clusters) + " cluster(s) requested)");
  Assert(num_clusters < histogram->bin_count(), "more clusters (" + std::to_string(num_clusters) + ") than histogram bins (" + std::to_string(histogram->bin_count()) + ")");

  // TODO handle NULL values  
  std::vector<std::pair<AllTypeVariant, AllTypeVariant>> boundaries(num_clusters);

  const auto ideal_rows_per_cluster = std::max(size_t{1}, row_count / num_clusters);
  size_t boundary_index = 0;
  AllTypeVariant lower_bound;
  AllTypeVariant upper_bound;
  size_t rows_in_cluster = 0;
  bool lower_bound_set = false;
  bool cluster_full = false;
  size_t bin_id{0};
  for (; bin_id < histogram->bin_count(); bin_id++) {
    if (!lower_bound_set) {
      lower_bound = histogram->bin_minimum(bin_id);
      lower_bound_set = true;
    }

    const auto bin_size = histogram->bin_height(bin_id);
    Assert(bin_size < 2 * ideal_rows_per_cluster, "bin is too large: " + std::to_string(bin_size) + ", but a cluster should have about " + std::to_string(ideal_rows_per_cluster) + " rows");
    if (rows_in_cluster + bin_size < ideal_rows_per_cluster) {
      // cluster has not yet reached its target size
      rows_in_cluster += bin_size;
      upper_bound = histogram->bin_maximum(bin_id);
    } else if (rows_in_cluster + bin_size - ideal_rows_per_cluster < ideal_rows_per_cluster - rows_in_cluster) {
      // cluster gets larger than the target size with this bin, but it is still closer to the target size than without the bin
      upper_bound = histogram->bin_maximum(bin_id);
      cluster_full = true;
    } else {
      // cluster would get larger than intended - process the bin again in the next cluster
      bin_id--;
      cluster_full = true;
    }

    if (boundary_index == boundaries.size()) {
      boundaries.push_back(std::make_pair(AllTypeVariant{}, AllTypeVariant{}));
    }
    boundaries[boundary_index] = std::make_pair(lower_bound, upper_bound);

    if (cluster_full) {
      lower_bound_set = false;
      rows_in_cluster = 0;
      boundary_index++;
      cluster_full = false;   
    }
  }

  Assert(bin_id == histogram->bin_count(), "histogram has " + std::to_string(histogram->bin_count()) + " bins, but processed only " + std::to_string(bin_id));

  return boundaries;
}


template <typename ColumnDataType>
size_t _get_cluster_index(const std::vector<std::pair<AllTypeVariant, AllTypeVariant>>& cluster_boundaries, const std::optional<ColumnDataType>& optional_value) {
  size_t cluster_index = 0;
  
  if (!optional_value) {
    Fail("null values not yet supported");
  } else {
    const ColumnDataType& value = *optional_value;
    
    for (const std::pair<AllTypeVariant, AllTypeVariant>& boundary : cluster_boundaries) {
      const auto low = boost::lexical_cast<ColumnDataType>(boundary.first);
      const auto high = boost::lexical_cast<ColumnDataType>(boundary.second);
      if (low <= value && value <= high) {
        break;                
      }
      cluster_index++;
    }

    if (cluster_index == cluster_boundaries.size()) {
      std::cout << "no matching cluster found for " << value                 
                << " with boundaries [" << cluster_boundaries[1].first << ", " << cluster_boundaries[1].second << "]" << std::endl;
      Fail("no matching cluster");
    }
  }

  return cluster_index;
}


// TODO: maybe get rid of AllTypeVariant
std::vector<std::shared_ptr<Chunk>> DisjointClustersAlgo::_distribute_chunk(const std::shared_ptr<Chunk>& chunk, const std::shared_ptr<Table>& table, const std::vector<std::vector<std::pair<AllTypeVariant, AllTypeVariant>>>& boundaries, std::vector<std::shared_ptr<Chunk>>& partially_filled_chunks, const std::vector<std::shared_ptr<Chunk>>& previously_partially_filled_chunks, const std::vector<ColumnID>& clustering_column_ids) {
  Assert(partially_filled_chunks.empty(), "'partially_filled_chunks' should be empty");
  Assert(boundaries.size() == clustering_column_ids.size(), "we need one boundary per clustering column");

  std::vector<std::vector<size_t>> cluster_indices(chunk->size());

  size_t previously_partially_filled_chunk_row_count = 0;
  for (const auto& c: previously_partially_filled_chunks) {
    previously_partially_filled_chunk_row_count += c->size();
  }

  for (size_t index{0}; index < boundaries.size(); index++) {
    const auto clustering_column_id = clustering_column_ids[index];
    const auto column_data_type = table->column_data_type(clustering_column_id);
    const auto& cluster_boundaries = boundaries[index];

    resolve_data_type(column_data_type, [&](const auto data_type_t) {
      using ColumnDataType = typename decltype(data_type_t)::type;
      const auto segment = chunk->get_segment(clustering_column_id);
      Assert(segment, "segment was null");

      ChunkOffset chunk_offset{0};      
      segment_iterate<ColumnDataType>(*segment, [&](const auto& position) {
          std::optional<ColumnDataType> value;
          if (!position.is_null()) {
            value = position.value();
          }
          cluster_indices[chunk_offset].push_back(_get_cluster_index<ColumnDataType>(cluster_boundaries, value));
          ++chunk_offset;
        });
    });
  }

  


  // über clustering_columns iterieren, über segmente iterieren, für jede Zeile linear die Boundaries durchlaufen und treffer-index in cluster_indices schreiben

  // previously_... und cluster_indices in clusters einpflegen
  std::map<std::vector<size_t>, std::vector<std::shared_ptr<Chunk>>> clusters;
  for (const auto& previously_partially_filled_chunk : previously_partially_filled_chunks) {
    std::vector<size_t> indices;
    for (size_t index{0}; index < boundaries.size(); index++) {
      const auto clustering_column_id = clustering_column_ids[index];
      const auto column_data_type = table->column_data_type(clustering_column_id);
      const auto& cluster_boundaries = boundaries[index];

      resolve_data_type(column_data_type, [&](const auto data_type_t) {
        using ColumnDataType = typename decltype(data_type_t)::type;
        const auto segment = previously_partially_filled_chunk->get_segment(clustering_column_id);
        Assert(segment, "segment was null");
        
        const auto& value = boost::lexical_cast<ColumnDataType>((*segment)[0]);
        indices.push_back(_get_cluster_index<ColumnDataType>(cluster_boundaries, value));
      });      
    }
    Assert(indices.size() == 1, "index calculation broken");
    const auto segments = _get_segments(previously_partially_filled_chunk);
    auto copied_chunk = std::make_shared<Chunk>(segments, previously_partially_filled_chunk->mvcc_data());
    std::cout << "starting with a partially filled chunk (id: " << indices[0] << "), size is: " << copied_chunk->size() << std::endl;
    clusters[indices] = { copied_chunk };
  }
  Assert(clusters.size() == previously_partially_filled_chunks.size(), "did not copy all chunks into the cluster");

    size_t rows_loaded = 0;
    for (const auto& [key, chunk_vector] : clusters) {
      Assert(chunk_vector.size() == 1, "expected just one chunk");
      rows_loaded += chunk_vector[0]->size();
    }
    Assert(rows_loaded == previously_partially_filled_chunk_row_count, "should have " + std::to_string(previously_partially_filled_chunk_row_count)
      + " rows, but got " + std::to_string(rows_loaded));

  for (ChunkOffset chunk_offset{0}; chunk_offset < chunk->size(); chunk_offset++) {
    const auto cluster_index = cluster_indices[chunk_offset];

    std::vector<AllTypeVariant> insertion_values;
    for (ColumnID column_id{0}; column_id < chunk->column_count(); column_id++) {
      const auto segment = chunk->get_segment(column_id);
      Assert(segment, "segment was null");
      insertion_values.push_back((*segment)[chunk_offset]);
    }


    if (clusters.find(cluster_index) == clusters.end()) {
      clusters[cluster_index] = { _create_empty_chunk(table, table->target_chunk_size()) };
      std::cout << "creating new empty chunk with id ";
      for (const auto ci : cluster_index) {std::cout << ci << " ";}
      std::cout << std::endl;
    } else {
      //std::cout << "chunk with id ";
      //for (const auto ci : cluster_index) {std::cout << ci << " ";}
      //std::cout << std::endl;


      const auto& insertion_chunk_vector = clusters[cluster_index];
      Assert(table->target_chunk_size() == 25000, "wrong target chunk size");
      Assert(table->target_chunk_size() >= insertion_chunk_vector.back()->size(), "chunk is larger than allowed");
      if (insertion_chunk_vector.back()->size() == table->target_chunk_size()) {
        clusters[cluster_index].push_back(_create_empty_chunk(table, table->target_chunk_size()));
        std::cout << "reached a full chunk" << std::endl;
      }
    }
    auto rows = clusters[cluster_index].back()->size();
    clusters[cluster_index].back()->append(insertion_values);
    Assert(rows + 1 == clusters[cluster_index].back()->size(), "append did not work");
  }

    size_t total_rows = 0;
    for (const auto& [key, chunk_vector] : clusters) {
      for (const auto& c : chunk_vector) {
        total_rows += c->size();
      }
    
    }
    Assert(total_rows == previously_partially_filled_chunk_row_count + chunk->size(), "wrong number of rows");

  // über clusters drübergehen, aufteilen in volle Chunks und partially_...
  std::vector<std::shared_ptr<Chunk>> full_chunks;
  for (const auto& [clustering_key, chunks] : clusters) {
    for (const auto & clustered_chunk : chunks) {
      if (clustered_chunk->size() == table->target_chunk_size()) {
        full_chunks.push_back(clustered_chunk);
        std::cout << "found a full chunk" << std::endl;
      } else {
        partially_filled_chunks.push_back(clustered_chunk);
      }
    }
  }

  return full_chunks;
}

std::vector<std::shared_ptr<Chunk>> DisjointClustersAlgo::_sort_and_encode_chunks(const std::vector<std::shared_ptr<Chunk>>& chunks, const ColumnID sort_column_id, const std::shared_ptr<Table>& table) const {
  std::vector<std::shared_ptr<Chunk>> sorted_chunks;
  for (const auto& chunk : chunks) {
    Assert(chunk->mvcc_data(), "no mvcc");
    auto sorted_chunk = _sort_chunk(chunk, sort_column_id, table->column_definitions());
    Assert(sorted_chunk->mvcc_data(), "no mvcc");
    sorted_chunk->finalize();
    ChunkEncoder::encode_chunk(sorted_chunk, table->column_data_types(), EncodingType::Dictionary);
    sorted_chunks.push_back(sorted_chunk);
    Assert(sorted_chunk->mvcc_data(), "no mvcc");
  }      

  return sorted_chunks;  
}

ChunkID _get_chunk_id_in_table(const std::shared_ptr<Chunk> chunk, const std::shared_ptr<Table> table) {
  for (ChunkID chunk_id{0}; chunk_id < table->chunk_count(); chunk_id++) {
    const auto& table_chunk = table->get_chunk(chunk_id);
    if (table_chunk == chunk) {
      std::cout << "found a chunk with id " << chunk_id << std::endl;
      return chunk_id;
    }
  }
  Fail("chunk not found");
  return ChunkID{0};
}

void DisjointClustersAlgo::_perform_clustering() {

  for (const auto& [table_name, clustering_config] : clustering_by_table) {
    const auto& table = Hyrise::get().storage_manager.get_table(table_name);
    Assert(table, "table " + table_name + " does not exist");

    std::vector<ColumnID> clustering_column_ids;
    std::vector<size_t> num_clusters_per_dimension;
    for (const auto& clustering_dimension : clustering_config) {
      clustering_column_ids.push_back(table->column_id_by_name(clustering_dimension.first));
      num_clusters_per_dimension.push_back(clustering_dimension.second);
    }

    const auto& clustering_dimension = clustering_config[0];  // TODO multiple dimensions
    const auto& clustering_column = clustering_dimension.first;
    const auto  num_clusters = clustering_dimension.second;
    const auto row_count = table->row_count();

    const auto& sort_column_name = clustering_config.back().first;
    const auto sort_column_id = table->column_id_by_name(sort_column_name);

    const auto column_data_type = table->column_data_type(table->column_id_by_name(clustering_column));
    resolve_data_type(column_data_type, [&](const auto data_type_t) {
      using ColumnDataType = typename decltype(data_type_t)::type;
      const auto histogram = _get_histogram<ColumnDataType>(table, clustering_column);

            
      std::cout << clustering_column << " (" << table_name << ") has " << row_count - (histogram->total_count()) << " NULL values" << std::endl;
      // TODO: proper NULL handling
      const auto boundaries = _get_boundaries<ColumnDataType>(histogram, row_count, num_clusters);
      std::cout << "computed boundaries" << std::endl;

      auto tmp = 0;
      for (const auto& boundary : boundaries) {
        std::cout << "boundary " << tmp << ": [" << boundary.first << ", " << boundary.second << "]" << std::endl;
        tmp++;
      }

      std::cout << "requested " << num_clusters << " boundaries, got " << boundaries.size() << " (" << 100.0 * boundaries.size() / num_clusters << "%)" << std::endl;

      std::vector<std::shared_ptr<Chunk>> partially_filled_chunks;
      std::vector<std::shared_ptr<Chunk>> previously_partially_filled_chunks;
      std::vector<ChunkID> temporary_chunk_ids;

      const auto chunk_count_before_clustering = table->chunk_count();
      for (ChunkID chunk_id{0}; chunk_id < chunk_count_before_clustering; chunk_id++) {
        const auto initial_chunk = table->get_chunk(chunk_id);
        bool last_chunk_to_cluster = chunk_id + 1 == chunk_count_before_clustering;
        if (initial_chunk) {
          auto filled_chunks = _distribute_chunk(initial_chunk, table, std::vector<std::vector<std::pair<AllTypeVariant, AllTypeVariant>>>{ boundaries }, partially_filled_chunks, previously_partially_filled_chunks, clustering_column_ids);

          // since we do just one pass over the table, we can sort and finalize the chunks immediately
          const auto& post_processed_chunks = _sort_and_encode_chunks(filled_chunks, sort_column_id, table);

          //TODO MVCC check and transaction-like move, repeat on failure
          constexpr bool CHUNK_UNCHANGED = true;        
          if (CHUNK_UNCHANGED) {
            table->remove_chunk(chunk_id);
            for (const auto temporary_chunk_id : temporary_chunk_ids) {
              table->remove_chunk(temporary_chunk_id);
            }

            _append_sorted_chunks_to_table(post_processed_chunks, table, false);
            std::cout << "added full chunks" << std::endl;
            

            // TODO do transactions guarantee that no new chunks are added? Probably not -> assert
            const auto first_inserted_chunk_id = table->chunk_count();
            if (last_chunk_to_cluster) {
              const auto post_processed_last_chunks = _sort_and_encode_chunks(partially_filled_chunks, sort_column_id, table);
              for (const auto& c : post_processed_last_chunks) {
                Assert(!c->is_mutable(), "mutable chunk");
              }
              _append_sorted_chunks_to_table(post_processed_last_chunks, table, false);

              size_t rows_in_unfull_chunks = 0;
              for (const auto& c : post_processed_last_chunks) {
                rows_in_unfull_chunks += c->size();
              }

              const auto num_unfull_chunks = post_processed_last_chunks.size();
              const auto avg_rows_in_unfull_chunks = rows_in_unfull_chunks / num_unfull_chunks;
              std::cout << "There are "  << num_unfull_chunks << " chunks that are not full. On average, they have "
                        << avg_rows_in_unfull_chunks << " rows (" << 100 * avg_rows_in_unfull_chunks / table->target_chunk_size()
                        << "% of the target chunk size " << table->target_chunk_size() << ")" << std::endl;
            } else {
              for (const auto& c : partially_filled_chunks) {
                Assert(c->size() < 25000, "partially filled != full");
              }

              _append_chunks_to_table(partially_filled_chunks, table, true);
              std::cout << "added partially filled chunks" << std::endl;
              temporary_chunk_ids.clear();
              for (auto inserted_chunk_id = first_inserted_chunk_id; inserted_chunk_id < table->chunk_count(); inserted_chunk_id++) {
                temporary_chunk_ids.push_back(inserted_chunk_id);
              }
            }
            
            Assert(temporary_chunk_ids.size() == partially_filled_chunks.size(), "incorrect number of chunks");
            Assert(first_inserted_chunk_id + partially_filled_chunks.size() == size_t{table->chunk_count()}, "some additional chunk appeared");

            previously_partially_filled_chunks = partially_filled_chunks;
            partially_filled_chunks = {};
          }
          // TODO last chunk handling
        }        
      }

    });
  }
}

} // namespace opossum

