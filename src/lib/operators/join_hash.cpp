#include "join_hash.hpp"

#include <cmath>
#include <memory>
#include <numeric>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bytell_hash_map.hpp"
#include "join_hash/join_hash_steps.hpp"
#include "join_hash/join_hash_traits.hpp"
#include "resolve_type.hpp"
#include "scheduler/abstract_task.hpp"
#include "scheduler/current_scheduler.hpp"
#include "scheduler/job_task.hpp"
#include "type_cast.hpp"
#include "type_comparison.hpp"
#include "utils/assert.hpp"
#include "utils/timer.hpp"

namespace opossum {

JoinHash::JoinHash(const std::shared_ptr<const AbstractOperator>& left,
                   const std::shared_ptr<const AbstractOperator>& right, const JoinMode mode,
                   const ColumnIDPair& column_ids, const PredicateCondition predicate_condition,
                   const std::optional<size_t>& radix_bits,
                   std::vector<JoinPredicate> additional_join_predicates)
    : AbstractJoinOperator(OperatorType::JoinHash, left, right, mode, column_ids, predicate_condition),
      _radix_bits(radix_bits),
      _additional_join_predicates(std::move(additional_join_predicates)) {
  DebugAssert(predicate_condition == PredicateCondition::Equals, "Operator not supported by Hash Join.");
}

const std::string JoinHash::name() const { return "JoinHash"; }

std::shared_ptr<AbstractOperator> JoinHash::_on_deep_copy(
    const std::shared_ptr<AbstractOperator>& copied_input_left,
    const std::shared_ptr<AbstractOperator>& copied_input_right) const {
  return std::make_shared<JoinHash>(copied_input_left, copied_input_right, _mode, _column_ids, _predicate_condition,
      _radix_bits, _additional_join_predicates);
}

void JoinHash::_on_set_parameters(const std::unordered_map<ParameterID, AllTypeVariant>& parameters) {}

std::shared_ptr<const Table> JoinHash::_on_execute() {
  std::shared_ptr<const AbstractOperator> build_operator;
  std::shared_ptr<const AbstractOperator> probe_operator;
  ColumnID build_column_id;
  ColumnID probe_column_id;

  // This is the expected implementation for swapping tables:
  // (1) if left or right outer join, outer relation becomes probe relation (we have to swap only for left outer)
  // (2) for a semi and anti join the inputs are always swapped
  bool inputs_swapped = (_mode == JoinMode::Left || _mode == JoinMode::Anti || _mode == JoinMode::Semi);

  // (3) else the smaller relation will become build relation, the larger probe relation
  if (!inputs_swapped && _input_left->get_output()->row_count() > _input_right->get_output()->row_count()) {
    inputs_swapped = true;
  }


  if (inputs_swapped) {
    // luckily we don't have to swap the operation itself here, because we only support the commutative Equi Join.
    build_operator = _input_right;
    probe_operator = _input_left;
    build_column_id = _column_ids.second;
    probe_column_id = _column_ids.first;
  } else {
    build_operator = _input_left;
    probe_operator = _input_right;
    build_column_id = _column_ids.first;
    probe_column_id = _column_ids.second;
  }

  std::vector<JoinPredicate> additional_join_predicates;

  if (inputs_swapped) {
    for (const auto& pred : _additional_join_predicates) {
      additional_join_predicates.emplace_back(
          JoinPredicate{ColumnIDPair{pred.column_id_pair.second, pred.column_id_pair.first},
                        flip_predicate_condition(pred.predicate_condition)});
    }
  } else {
    additional_join_predicates = _additional_join_predicates;
  }

  auto adjusted_column_ids = std::make_pair(build_column_id, probe_column_id);

  auto build_input = build_operator->get_output();
  auto probe_input = probe_operator->get_output();

  _impl = make_unique_by_data_types<AbstractReadOnlyOperatorImpl, JoinHashImpl>(
      build_input->column_data_type(build_column_id), probe_input->column_data_type(probe_column_id), *this,
      build_operator, probe_operator, _mode, adjusted_column_ids, _predicate_condition, inputs_swapped, _radix_bits,
      std::move(additional_join_predicates));
  return _impl->_on_execute();
}

void JoinHash::_on_cleanup() { _impl.reset(); }

template <typename LeftType, typename RightType>
class JoinHash::JoinHashImpl : public AbstractJoinOperatorImpl {
 public:
  JoinHashImpl(const JoinHash& join_hash, const std::shared_ptr<const AbstractOperator>& left,
               const std::shared_ptr<const AbstractOperator>& right, const JoinMode mode,
               const ColumnIDPair& column_ids, const PredicateCondition predicate_condition, const bool inputs_swapped,
               const std::optional<size_t>& radix_bits = std::nullopt,
               std::vector<JoinPredicate> additional_join_predicates = {})
      : _join_hash(join_hash),
        _left(left),
        _right(right),
        _mode(mode),
        _column_ids(column_ids),
        _predicate_condition(predicate_condition),
        _inputs_swapped(inputs_swapped),
        _additional_join_predicates(std::move(additional_join_predicates)) {
    if (radix_bits.has_value()) {
      _radix_bits = radix_bits.value();
    } else {
      _radix_bits = _calculate_radix_bits();
    }
  }

 protected:
  const JoinHash& _join_hash;
  const std::shared_ptr<const AbstractOperator> _left, _right;
  const JoinMode _mode;
  const ColumnIDPair _column_ids;
  const PredicateCondition _predicate_condition;
  const bool _inputs_swapped;
  const std::vector<JoinPredicate> _additional_join_predicates;

  std::shared_ptr<Table> _output_table;

  size_t _radix_bits;

  // Determine correct type for hashing
  using HashedType = typename JoinHashTraits<LeftType, RightType>::HashType;

  size_t _calculate_radix_bits() const {
    /*
      Setting number of bits for radix clustering:
      The number of bits is used to create probe partitions with a size that can
      be expected to fit into the L2 cache.
      This should incorporate hardware knowledge, once available in Hyrise.
      As of now, we assume a L2 cache size of 256 KB.
      We estimate the size the following way:
        - we assume each key appears once (that is an overestimation space-wise, but we
        aim rather for a hash map that is slightly smaller than L2 than slightly larger)
        - each entry in the hash map is a data structure holding the actual value
        and the RowID
    */
    const auto build_relation_size = _left->get_output()->row_count();
    const auto probe_relation_size = _right->get_output()->row_count();

    if (build_relation_size > probe_relation_size) {
      /*
        Hash joins perform best for join relations with a small left join partner. In case the
        optimizer selects the hash join due to such a situation, but neglects that the
        input will be switched (e.g., due to the join type), the user will be warned.
      */
      std::string warning{"Left relation larger than right relation hash join"};
      warning += _inputs_swapped ? " (input relations have been swapped)." : ".";
      PerformanceWarning(warning);
    }

    const auto l2_cache_size = 256'000;  // bytes

    // To get a pessimistic estimation (ensure that the hash table fits within the cache), we assume
    // that each value maps to a PosList with a single RowID. For the used small_vector's, we assume a
    // size of 2*RowID per PosList. For sizing, see comments:
    // https://probablydance.com/2018/05/28/a-new-fast-hash-table-in-response-to-googles-new-fast-hash-table/
    const auto complete_hash_map_size =
        // number of items in map
        (build_relation_size *
         // key + value (and one byte overhead, see link above)
         (sizeof(LeftType) + 2 * sizeof(RowID) + 1))
        // fill factor
        / 0.8;

    const auto adaption_factor = 2.0f;  // don't occupy the whole L2 cache
    const auto cluster_count = std::max(1.0, (adaption_factor * complete_hash_map_size) / l2_cache_size);

    return static_cast<size_t>(std::ceil(std::log2(cluster_count)));
  }

  std::shared_ptr<const Table> _on_execute() override {
    auto right_in_table = _right->get_output();
    auto left_in_table = _left->get_output();

    /**
     * Creates output table. This means a table is created by populating the column
     * definitions.
     */
    _output_table = _join_hash._initialize_output_table();

    /*
     * This flag is used in the materialization and probing phases.
     * When dealing with an OUTER join, we need to make sure that we keep the NULL values for the outer relation.
     * In the current implementation, the relation on the right is always the outer relation.
     */
    const auto keep_nulls = (_mode == JoinMode::Left || _mode == JoinMode::Right);

    // Pre-partitioning:
    // Save chunk offsets into the input relation.
    const auto left_chunk_offsets = determine_chunk_offsets(left_in_table);
    const auto right_chunk_offsets = determine_chunk_offsets(right_in_table);

    Timer performance_timer;

    // Containers used to store histograms for (potentially subsequent) radix
    // partitioning phase (in cases _radix_bits > 0). Created during materialization phase.
    std::vector<std::vector<size_t>> histograms_left;
    std::vector<std::vector<size_t>> histograms_right;

    // Output containers of materialization phase. Type similar to the output
    // of radix partitioning phase to allow short cut for _radix_bits == 0
    // (in this case, we can skip the partitioning alltogether).
    RadixContainer<LeftType> materialized_left;
    RadixContainer<RightType> materialized_right;

    // Containers for potential (skipped when left side small) radix partitioning phase
    RadixContainer<LeftType> radix_left;
    RadixContainer<RightType> radix_right;
    std::vector<std::optional<HashTable<HashedType>>> hashtables;

    // Depiction of the hash join parallelization (radix partitioning can be skipped when radix_bits = 0)
    // ===============================================================================================
    // We have two data paths, one for left side and one for right input side. We can prepare (i.e.,
    // materialize(), build(), etc.) both sides in parallel until the actual join takes place.
    // All tasks might spawn concurrent tasks themselves. For example, materialize parallelizes over
    // the input chunks and the following steps over the radix clusters.
    //
    //           Relation Left                       Relation Right
    //                 |                                    |
    //        materialize_input()                  materialize_input()
    //                 |                                    |
    //  ( partition_radix_parallel() )       ( partition_radix_parallel() )
    //                 |                                    |
    //               build()                                |
    //                   \_                               _/
    //                     \_                           _/
    //                       \_                       _/
    //                         \_                   _/
    //                           \                 /
    //                          Probing (actual Join)

    std::vector<std::shared_ptr<AbstractTask>> jobs;

    // Pre-Probing path of left relation
    jobs.emplace_back(std::make_shared<JobTask>([&]() {
      // materialize left table (NULLs are always discarded for the build side)
      materialized_left = materialize_input<LeftType, HashedType, false>(
          left_in_table, _column_ids.first, left_chunk_offsets, histograms_left, _radix_bits);

      if (_radix_bits > 0) {
        // radix partition the left table
        radix_left = partition_radix_parallel<LeftType, HashedType, false>(materialized_left, left_chunk_offsets,
                                                                           histograms_left, _radix_bits);
      } else {
        // short cut: skip radix partitioning and use materialized data directly
        radix_left = std::move(materialized_left);
      }

      // build hash tables
      hashtables = build<LeftType, HashedType>(radix_left);
    }));
    jobs.back()->schedule();

    jobs.emplace_back(std::make_shared<JobTask>([&]() {
      // Materialize right table. The third template parameter signals if the relation on the right (probe
      // relation) materializes NULL values when executing OUTER joins (default is to discard NULL values).
      if (keep_nulls) {
        materialized_right = materialize_input<RightType, HashedType, true>(
            right_in_table, _column_ids.second, right_chunk_offsets, histograms_right, _radix_bits);
      } else {
        materialized_right = materialize_input<RightType, HashedType, false>(
            right_in_table, _column_ids.second, right_chunk_offsets, histograms_right, _radix_bits);
      }

      if (_radix_bits > 0) {
        // radix partition the right table. 'keep_nulls' makes sure that the
        // relation on the right keeps NULL values when executing an OUTER join.
        if (keep_nulls) {
          radix_right = partition_radix_parallel<RightType, HashedType, true>(materialized_right, right_chunk_offsets,
                                                                              histograms_right, _radix_bits);
        } else {
          radix_right = partition_radix_parallel<RightType, HashedType, false>(materialized_right, right_chunk_offsets,
                                                                               histograms_right, _radix_bits);
        }
      } else {
        // short cut: skip radix partitioning and use materialized data directly
        radix_right = std::move(materialized_right);
      }
    }));
    jobs.back()->schedule();

    CurrentScheduler::wait_for_tasks(jobs);

    // Probe phase
    std::vector<PosList> left_pos_lists;
    std::vector<PosList> right_pos_lists;
    const size_t partition_count = radix_right.partition_offsets.size();
    left_pos_lists.resize(partition_count);
    right_pos_lists.resize(partition_count);

    // simple heuristic: half of the rows of the right relation will match
    const size_t result_rows_per_partition = _right->get_output()->row_count() / partition_count / 2;
    for (size_t i = 0; i < partition_count; i++) {
      left_pos_lists[i].reserve(result_rows_per_partition);
      right_pos_lists[i].reserve(result_rows_per_partition);
    }

    /*
    NUMA notes:
    The workers for each radix partition P should be scheduled on the same node as the input data:
    leftP, rightP and hashtableP.
    */
    if (_mode == JoinMode::Semi || _mode == JoinMode::Anti) {
      probe_semi_anti<RightType, HashedType>(radix_right, hashtables, right_pos_lists, _mode, *left_in_table,
                                             *right_in_table, _additional_join_predicates);
    } else {
      if (_mode == JoinMode::Left || _mode == JoinMode::Right) {
        probe<RightType, HashedType, true>(radix_right, hashtables, left_pos_lists, right_pos_lists, _mode,
                                           *left_in_table, *right_in_table, _additional_join_predicates);
      } else {
        probe<RightType, HashedType, false>(radix_right, hashtables, left_pos_lists, right_pos_lists, _mode,
                                            *left_in_table, *right_in_table, _additional_join_predicates);
      }
    }

    auto only_output_right_input = _inputs_swapped && (_mode == JoinMode::Semi || _mode == JoinMode::Anti);

    /**
     * After the probe phase left_pos_lists and right_pos_lists contain all pairs of joined rows grouped by
     * partition. Let p be a partition index and r a row index. The value of left_pos_lists[p][r] will match right_pos_lists[p][r].
     */

    /**
     * Two Caches to avoid redundant reference materialization for Reference input tables. As there might be
     *  quite a lot Partitions (>500 seen), input Chunks (>500 seen), and columns (>50 seen), this speeds up
     *  write_output_chunks a lot.
     *
     * They do two things:
     *      - Make it possible to re-use output pos lists if two segments in the input table have exactly the same
     *          PosLists Chunk by Chunk
     *      - Avoid creating the std::vector<const PosList*> for each Partition over and over again.
     *
     * They hold one entry per column in the table, not per BaseSegment in a single chunk
     */
    // std::vector<std::shared_ptr<PosLists>>;
    PosListsBySegment left_pos_lists_by_segment;
    PosListsBySegment right_pos_lists_by_segment;

    // left_pos_lists_by_segment will only be needed if left is a reference table and being output
    if (left_in_table->type() == TableType::References && !only_output_right_input) {
      left_pos_lists_by_segment = setup_pos_lists_by_segment(left_in_table);
    }

    // right_pos_lists_by_segment will only be needed if right is a reference table
    if (right_in_table->type() == TableType::References) {
      right_pos_lists_by_segment = setup_pos_lists_by_segment(right_in_table);
    }

    // for every partition create a reference segment
    for (size_t partition_id = 0; partition_id < left_pos_lists.size(); ++partition_id) {
      // moving the values into a shared pos list saves us some work in write_output_segments. We know that
      // left_pos_lists and right_pos_lists will not be used again.
      auto left = std::make_shared<PosList>(std::move(left_pos_lists[partition_id]));
      auto right = std::make_shared<PosList>(std::move(right_pos_lists[partition_id]));

      if (left->empty() && right->empty()) {
        continue;
      }

      /**
       * Apply additional predicates
       * Table 1, Table 2, left, right
       * Iterates through left and right and checks if predicate applies, if not, row will be deleted from left & right.
       * left and right contain the joined row ids of the left and right table.
       */

      /*if (_additional_join_predicates.has_value()) {
        _apply_additional_join_predicates(*left_in_table, *left, *right_in_table, *right,
                                          _additional_join_predicates.value());
      }*/

      // using Segments = pmr_vector<std::shared_ptr<BaseSegment>>
      Segments output_segments;

      // write_output_segments iterates through right and left and creates one segment for every column which is written
      // to output_segments.
      // output_segments is then appended as a chunk to _output_table

      // we need to swap back the inputs, so that the order of the output columns is not harmed
      if (_inputs_swapped) {
        write_output_segments(output_segments, right_in_table, right_pos_lists_by_segment, right);

        // Semi/Anti joins are always swapped but do not need the outer relation
        if (!only_output_right_input) {
          write_output_segments(output_segments, left_in_table, left_pos_lists_by_segment, left);
        }
      } else {
        write_output_segments(output_segments, left_in_table, left_pos_lists_by_segment, left);
        write_output_segments(output_segments, right_in_table, right_pos_lists_by_segment, right);
      }

      _output_table->append_chunk(output_segments);
    }

    return _output_table;
  }

  /**
   * We only accept join predicates where predicate_condition is Equals. Also, predicates must be linked by AND.
   * @param left
   * @param left_rows_to_verify
   * @param right
   * @param right_rows_to_verify
   * @param join_predicates
   */
  static void _apply_additional_join_predicates(const Table& left, PosList& left_rows_to_verify, const Table& right,
                                                PosList& right_rows_to_verify,
                                                const std::vector<JoinPredicate>& join_predicates) {
    DebugAssert(left_rows_to_verify.size() == right_rows_to_verify.size(),
                "left_rows_to_verify should have the same"
                " amount of rows as right_rows_to_verify");

    if (join_predicates.empty()) {
      return;
    }

    PosList left_selection;
    left_selection.reserve(left_rows_to_verify.size());
    PosList right_selection;
    right_selection.reserve(right_rows_to_verify.size());

    for (size_t row_idx{0}; row_idx < left_rows_to_verify.size(); ++row_idx) {
      const auto& left_row_id = left_rows_to_verify[row_idx];
      const auto& right_row_id = right_rows_to_verify[row_idx];

      bool row_pair_satisfies_predicates = true;
      for (const auto& pred : join_predicates) {
        DebugAssert(pred.predicate_condition == PredicateCondition::Equals,
                    "Only PredicateCondition::Equals is"
                    " supported.");
        const auto& left_segment = *left.get_chunk(left_row_id.chunk_id)->segments()[pred.column_id_pair.first];
        const auto& right_segment = *right.get_chunk(right_row_id.chunk_id)->segments()[pred.column_id_pair.second];

        if (!(left_segment[left_row_id.chunk_offset] == right_segment[right_row_id.chunk_offset])) {
          row_pair_satisfies_predicates = false;
          break;
        }
      }

      if (row_pair_satisfies_predicates) {
        left_selection.push_back(left_row_id);
        right_selection.push_back(right_row_id);
      }
    }

    left_rows_to_verify = std::move(left_selection);
    right_rows_to_verify = std::move(right_selection);
  }
};

}  // namespace opossum
