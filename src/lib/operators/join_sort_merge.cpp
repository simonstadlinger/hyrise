#include "join_sort_merge.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "constant_mappings.hpp"
#include "join_sort_merge/radix_cluster_sort.hpp"
#include "resolve_type.hpp"
#include "scheduler/abstract_task.hpp"
#include "scheduler/current_scheduler.hpp"
#include "scheduler/job_task.hpp"
#include "storage/abstract_segment_visitor.hpp"
#include "storage/reference_segment.hpp"
#include "storage/segment_accessor.hpp"

namespace opossum {

/**
* TODO(anyone): Outer not-equal join (outer !=)
**/

/**
* The sort merge join performs a join on two input tables on specific join columns. For usage notes, see the
* join_sort_merge.hpp. This is how the join works:
* -> The input tables are materialized and clustered to a specified number of clusters.
*    /utils/radix_cluster_sort.hpp for more info on the clustering phase.
* -> The join is performed per cluster. For the joining phase, runs of entries with the same value are identified
*    and handled at once. If a join-match is identified, the corresponding row_ids are noted for the output.
* -> Using the join result, the output table is built using pos lists referencing the original tables.
**/
JoinSortMerge::JoinSortMerge(const std::shared_ptr<const AbstractOperator>& left,
                             const std::shared_ptr<const AbstractOperator>& right, const JoinMode mode,
                             const ColumnIDPair& column_ids, const PredicateCondition op)
    : AbstractJoinOperator(OperatorType::JoinSortMerge, left, right, mode, column_ids, op) {
  // Validate the parameters
  DebugAssert(mode != JoinMode::Cross, "Sort merge join does not support cross joins.");
  DebugAssert((mode != JoinMode::Semi && mode != JoinMode::Anti) || op == PredicateCondition::Equals,
              "Sort merge join only supports Semi and Anti joins with an equality predicate.");
  DebugAssert(left != nullptr, "The left input operator is null.");
  DebugAssert(right != nullptr, "The right input operator is null.");
  DebugAssert(
      op == PredicateCondition::Equals || op == PredicateCondition::LessThan || op == PredicateCondition::GreaterThan ||
          op == PredicateCondition::LessThanEquals || op == PredicateCondition::GreaterThanEquals ||
          op == PredicateCondition::NotEquals,
      "Sort merge join does not support predicate condition '" + predicate_condition_to_string.left.at(op) + "'.");
  DebugAssert(op != PredicateCondition::NotEquals || mode == JoinMode::Inner,
              "Sort merge join does not support outer joins with inequality predicates.");
}

std::shared_ptr<AbstractOperator> JoinSortMerge::_on_deep_copy(
    const std::shared_ptr<AbstractOperator>& copied_input_left,
    const std::shared_ptr<AbstractOperator>& copied_input_right) const {
  return std::make_shared<JoinSortMerge>(copied_input_left, copied_input_right, _mode, _column_ids,
                                         _predicate_condition);
}

void JoinSortMerge::_on_set_parameters(const std::unordered_map<ParameterID, AllTypeVariant>& parameters) {}

std::shared_ptr<const Table> JoinSortMerge::_on_execute() {
  // Check column types
  const auto& left_column_type = input_table_left()->column_data_type(_column_ids.first);
  DebugAssert(left_column_type == input_table_right()->column_data_type(_column_ids.second),
              "Left and right column types do not match. The sort merge join requires matching column types");

  // Create implementation to compute the join result
  _impl = make_unique_by_data_type<AbstractJoinOperatorImpl, JoinSortMergeImpl>(
      left_column_type, *this, _column_ids.first, _column_ids.second, _predicate_condition, _mode);

  return _impl->_on_execute();
}

void JoinSortMerge::_on_cleanup() { _impl.reset(); }

const std::string JoinSortMerge::name() const { return "JoinSortMerge"; }

/**
** Start of implementation.
**/
template <typename T>
class JoinSortMerge::JoinSortMergeImpl : public AbstractJoinOperatorImpl {
 public:
  JoinSortMergeImpl<T>(JoinSortMerge& sort_merge_join, ColumnID left_column_id, ColumnID right_column_id,
                       const PredicateCondition op, JoinMode mode)
      : _sort_merge_join{sort_merge_join},
        _left_column_id{left_column_id},
        _right_column_id{right_column_id},
        _op{op},
        _mode{mode} {
    _cluster_count = _determine_number_of_clusters(_sort_merge_join.input_table_left()->row_count(),
                                                   _sort_merge_join.input_table_right()->row_count());
    _output_pos_lists_left.resize(_cluster_count);
    _output_pos_lists_right.resize(_cluster_count);
  }

 protected:
  JoinSortMerge& _sort_merge_join;

  // Contains the materialized sorted input tables
  std::unique_ptr<MaterializedSegmentList<T>> _sorted_left_table;
  std::unique_ptr<MaterializedSegmentList<T>> _sorted_right_table;

  // Contains the null value row ids if a join column is an outer join column
  std::unique_ptr<PosList> _null_rows_left;
  std::unique_ptr<PosList> _null_rows_right;

  const ColumnID _left_column_id;
  const ColumnID _right_column_id;

  const PredicateCondition _op;
  const JoinMode _mode;

  // the cluster count must be a power of two, i.e. 1, 2, 4, 8, 16, ...
  size_t _cluster_count;

  // Contains the output row ids for each cluster
  std::vector<std::shared_ptr<PosList>> _output_pos_lists_left;
  std::vector<std::shared_ptr<PosList>> _output_pos_lists_right;

  /**
   * The TablePosition is a utility struct that is used to define a specific position in a sorted input table.
  **/
  struct TableRange;
  struct TablePosition {
    TablePosition() = default;
    TablePosition(const size_t cluster, const size_t index) : cluster{cluster}, index{index} {}

    size_t cluster;
    size_t index;

    const TableRange to(const TablePosition position) const { return TableRange(*this, position); }
  };

  TablePosition _end_of_left_table;
  TablePosition _end_of_right_table;

  /**
    * The TableRange is a utility struct that is used to define ranges of rows in a sorted input table spanning from
    * a start position to an end position.
  **/
  struct TableRange {
    TableRange(const TablePosition start_position, const TablePosition end_position)
        : start(start_position), end(end_position) {}
    TableRange(const size_t cluster, const size_t start_index, const size_t end_index)
        : start{TablePosition(cluster, start_index)}, end{TablePosition(cluster, end_index)} {}

    const TablePosition start;
    const TablePosition end;

    // Executes the given action for every row id of the table in this range.
    template <typename F>
    void for_every_row_id(const std::unique_ptr<MaterializedSegmentList<T>>& table, F action) const {
      for (size_t cluster = start.cluster; cluster <= end.cluster; ++cluster) {
        const size_t start_index = (cluster == start.cluster) ? start.index : 0;
        const size_t end_index = (cluster == end.cluster) ? end.index : (*table)[cluster]->size();
        for (size_t index = start_index; index < end_index; ++index) {
          action((*(*table)[cluster])[index].row_id);
        }
      }
    }
  };

  /**
  * Determines the number of clusters to be used for the join.
  * This task is not trivial as multiple aspects have to be considered: (i) the system's cache
  * size, (ii) potential partitioning overhead, and (iii) the impact on successive operators.
  * As of now, the cache can only be estimated. A size of 256k is used as this should be close 
  * to the working machine of the students. For servers, however, this number might be vastly off.
  * Aspects (i) and (ii) determine the performance of the join alone. Many partitions
  * usually work well for sequential as well as parallel execution as the actual join phas is
  * faster, setting off the partitioning overhead.
  * However, each cluster results in an output chunk. As such, to limit the potential
  * negative impact of too many small chunks for the following operators, the cluster
  * count is limited (to avoid expensive merges in the end).
  * This is achieved by allowing the cluster count to grow linear up to 16 in every case, but
  * adding only squareroot(clusters beyong 16) after that.
  **/
  static size_t _determine_number_of_clusters(const size_t row_count_left, const size_t row_count_right) {
    constexpr size_t LINEAR_GROWTH_UPPER_BOUND = 16;
    const auto row_count_max = std::max(row_count_left, row_count_right);

    // Determine size in order to enable L2 cache-local sorts of the clusters.
    const auto materialized_value_size_per_cluster = 256'000 / sizeof(MaterializedValue<T>);
    const auto cluster_count_goal = row_count_max / materialized_value_size_per_cluster;

    const auto cluster_count_capped = std::min(LINEAR_GROWTH_UPPER_BOUND, cluster_count_goal) +
                                      static_cast<size_t>(std::sqrt(std::max(
                                          int{0}, static_cast<int>(cluster_count_goal - LINEAR_GROWTH_UPPER_BOUND))));

    const auto final_cluster_count = static_cast<size_t>(std::pow(2, std::lround(std::log2(cluster_count_capped))));
    return std::max(size_t{1}, final_cluster_count);
  }

  /**
  * Gets the table position corresponding to the end of the table, i.e. the last entry of the last cluster.
  **/
  static TablePosition _end_of_table(std::unique_ptr<MaterializedSegmentList<T>>& table) {
    DebugAssert(!table->empty(), "table has no chunks");
    auto last_cluster = table->size() - 1;
    return TablePosition(last_cluster, (*table)[last_cluster]->size());
  }

  /**
  * Represents the result of a value comparison.
  **/
  enum class CompareResult { Less, Greater, Equal };

  /**
  * Performs the join for two runs of a specified cluster.
  * A run is a series of rows in a cluster with the same value.
  **/
  void _join_runs(const TableRange left_run, const TableRange right_run, const CompareResult compare_result) {
    size_t cluster_number = left_run.start.cluster;
    switch (_op) {
      case PredicateCondition::Equals:
        if (compare_result == CompareResult::Equal) {
          _emit_all_combinations(cluster_number, left_run, right_run);
        } else if (compare_result == CompareResult::Less) {
          if (_mode == JoinMode::Left || _mode == JoinMode::Outer) {
            _emit_right_null_combinations(cluster_number, left_run);
          }
        } else if (compare_result == CompareResult::Greater) {
          if (_mode == JoinMode::Right || _mode == JoinMode::Outer) {
            _emit_left_null_combinations(cluster_number, right_run);
          }
        }
        break;
      case PredicateCondition::NotEquals:
        if (compare_result == CompareResult::Greater) {
          _emit_all_combinations(cluster_number, left_run.start.to(_end_of_left_table), right_run);
        } else if (compare_result == CompareResult::Equal) {
          _emit_all_combinations(cluster_number, left_run.end.to(_end_of_left_table), right_run);
          _emit_all_combinations(cluster_number, left_run, right_run.end.to(_end_of_right_table));
        } else if (compare_result == CompareResult::Less) {
          _emit_all_combinations(cluster_number, left_run, right_run.start.to(_end_of_right_table));
        }
        break;
      case PredicateCondition::GreaterThan:
        if (compare_result == CompareResult::Greater) {
          _emit_all_combinations(cluster_number, left_run.start.to(_end_of_left_table), right_run);
        } else if (compare_result == CompareResult::Equal) {
          _emit_all_combinations(cluster_number, left_run.end.to(_end_of_left_table), right_run);
        }
        break;
      case PredicateCondition::GreaterThanEquals:
        if (compare_result == CompareResult::Greater || compare_result == CompareResult::Equal) {
          _emit_all_combinations(cluster_number, left_run.start.to(_end_of_left_table), right_run);
        }
        break;
      case PredicateCondition::LessThan:
        if (compare_result == CompareResult::Less) {
          _emit_all_combinations(cluster_number, left_run, right_run.start.to(_end_of_right_table));
        } else if (compare_result == CompareResult::Equal) {
          _emit_all_combinations(cluster_number, left_run, right_run.end.to(_end_of_right_table));
        }
        break;
      case PredicateCondition::LessThanEquals:
        if (compare_result == CompareResult::Less || compare_result == CompareResult::Equal) {
          _emit_all_combinations(cluster_number, left_run, right_run.start.to(_end_of_right_table));
        }
        break;
      default:
        throw std::logic_error("Unknown PredicateCondition");
    }
  }

  /**
  * Emits a combination of a left row id and a right row id to the join output.
  **/
  void _emit_combination(const size_t output_cluster, const RowID left, const RowID right) {
    _output_pos_lists_left[output_cluster]->push_back(left);
    _output_pos_lists_right[output_cluster]->push_back(right);
  }

  /**
  * Emits all the combinations of row ids from the left table range and the right table range to the join output.
  * I.e. the cross product of the ranges is emitted.
  **/
  void _emit_all_combinations(const size_t output_cluster, TableRange left_range, TableRange right_range) {
    if (_mode != JoinMode::Semi && _mode != JoinMode::Anti) {
      left_range.for_every_row_id(_sorted_left_table, [&](RowID left_row_id) {
        right_range.for_every_row_id(_sorted_right_table, [&](RowID right_row_id) {
          _emit_combination(output_cluster, left_row_id, right_row_id);
        });
      });
    } else {
      left_range.for_every_row_id(_sorted_left_table, [&](RowID left_row_id) {
        _output_pos_lists_left[output_cluster]->push_back(left_row_id);
      });
    }
  }

  /**
  * Emits all combinations of row ids from the left table range and a NULL value on the right side to the join output.
  **/
  void _emit_right_null_combinations(const size_t output_cluster, TableRange left_range) {
    left_range.for_every_row_id(
        _sorted_left_table, [&](RowID left_row_id) { _emit_combination(output_cluster, left_row_id, NULL_ROW_ID); });
  }

  /**
  * Emits all combinations of row ids from the right table range and a NULL value on the left side to the join output.
  **/
  void _emit_left_null_combinations(const size_t output_cluster, TableRange right_range) {
    right_range.for_every_row_id(
        _sorted_right_table, [&](RowID right_row_id) { _emit_combination(output_cluster, NULL_ROW_ID, right_row_id); });
  }

  /**
  * Determines the length of the run starting at start_index in the values vector.
  * A run is a series of the same value.
  **/
  size_t _run_length(const size_t start_index, const std::shared_ptr<MaterializedSegment<T>> values) const {
    if (start_index >= values->size()) {
      return 0;
    }

    auto start_position = values->begin() + start_index;
    auto result = std::upper_bound(start_position, values->end(), *start_position,
                                   [](const auto& a, const auto& b) { return a.value < b.value; });

    return result - start_position;
  }

  /**
  * Compares two values and creates a comparison result.
  **/
  CompareResult _compare(const T left, const T right) const {
    if (left < right) {
      return CompareResult::Less;
    } else if (left == right) {
      return CompareResult::Equal;
    } else {
      return CompareResult::Greater;
    }
  }

  /**
  * Performs the join on a single cluster. Runs of entries with the same value are identified and handled together.
  * This constitutes the merge phase of the join. The output combinations of row ids are determined by _join_runs.
  **/
  void _join_cluster(const size_t cluster_number) {
    const auto& left_cluster = (*_sorted_left_table)[cluster_number];
    const auto& right_cluster = (*_sorted_right_table)[cluster_number];

    auto left_run_start = size_t{0};
    auto right_run_start = size_t{0};

    auto left_run_end = left_run_start + _run_length(left_run_start, left_cluster);
    auto right_run_end = right_run_start + _run_length(right_run_start, right_cluster);

    const size_t left_size = left_cluster->size();
    const size_t right_size = right_cluster->size();

    while (left_run_start < left_size && right_run_start < right_size) {
      const auto& left_value = (*left_cluster)[left_run_start].value;
      const auto& right_value = (*right_cluster)[right_run_start].value;

      const auto compare_result = _compare(left_value, right_value);

      TableRange left_run(cluster_number, left_run_start, left_run_end);
      TableRange right_run(cluster_number, right_run_start, right_run_end);
      _join_runs(left_run, right_run, compare_result);

      // Advance to the next run on the smaller side or both if equal
      if (compare_result == CompareResult::Equal) {
        // Advance both runs
        left_run_start = left_run_end;
        right_run_start = right_run_end;
        left_run_end = left_run_start + _run_length(left_run_start, left_cluster);
        right_run_end = right_run_start + _run_length(right_run_start, right_cluster);
      } else if (compare_result == CompareResult::Less) {
        // Advance the left run
        left_run_start = left_run_end;
        left_run_end = left_run_start + _run_length(left_run_start, left_cluster);
      } else {
        // Advance the right run
        right_run_start = right_run_end;
        right_run_end = right_run_start + _run_length(right_run_start, right_cluster);
      }
    }

    // Join the rest of the unfinished side, which is relevant for outer joins and non-equi joins
    const auto right_rest = TableRange(cluster_number, right_run_start, right_size);
    const auto left_rest = TableRange(cluster_number, left_run_start, left_size);
    if (left_run_start < left_size) {
      _join_runs(left_rest, right_rest, CompareResult::Less);
    } else if (right_run_start < right_size) {
      _join_runs(left_rest, right_rest, CompareResult::Greater);
    }

    // Short cut implementation for Anti joins. Implementing anti joins within the current sort-merge join
    // is not trivial. But since the anti join implementation of the hash joins can be slow in certain
    // cases (large build relation and small probe relations), this short cut still provides value.
    if (_mode == JoinMode::Anti) {
      // Overwrite semi join result with anti join result
      _output_pos_lists_left[cluster_number] =
          _remove_row_ids_from_materialized_segment(_output_pos_lists_left[cluster_number], left_cluster);

      // TODO(multi-predicate joins): in case of semi and anti joins, additional
      // predicates have to executed hereafter and not within _join_runs.
    }
  }

  /**
  * "Anti-merges" the left input and the matches of the executed semi join result.
  * As both lists are sorted by value, this process is rather efficient even though a full
  * anti join implementation within the actual sort merge join would be faster.
  */
  std::shared_ptr<PosList> _remove_row_ids_from_materialized_segment(
      const std::shared_ptr<PosList>& matches, const std::shared_ptr<MaterializedSegment<T>> input_segment) {
    auto pos_list = PosList{};
    pos_list.reserve(input_segment->size() - matches->size());

    auto matches_iter = matches->begin();
    auto input_segment_iter = input_segment->begin();

    auto append_remaining_positions = [&]() {
      while (input_segment_iter != input_segment->end()) {
        pos_list.push_back((*input_segment_iter).row_id);
        ++input_segment_iter;
      }
    };

    // Short cut for empty result of semi join
    if (matches->empty()) {
      append_remaining_positions();
      return std::make_shared<PosList>(std::move(pos_list));
    }

    // Accessor cache
    std::vector<std::unique_ptr<BaseSegmentAccessor<T>>> accessors(_sort_merge_join.input_table_left()->chunk_count());
    while (input_segment_iter != input_segment->end()) {
      const auto input_value = (*input_segment_iter).value;
      const auto input_row_id = (*input_segment_iter).row_id;

      const auto matches_chunk_id = (*matches_iter).chunk_id;

      auto& accessor = accessors[matches_chunk_id];
      if (!accessor) {
        accessor = create_segment_accessor<T>(
            _sort_merge_join.input_table_left()->get_chunk(matches_chunk_id)->get_segment(_left_column_id));
      }

      // Optional directly accessed, as matches cannot contain NULLs.
      const auto& semi_join_value = accessor->access((*matches_iter).chunk_offset).value();

      if (input_value == semi_join_value) {
        // If the value matches, the input tuple cannot be part of the anti join result
        ++input_segment_iter;
        ++matches_iter;
        if (matches_iter == matches->end()) {
          // If end of matches has been reached, all remaining tuples of input are part of the anti join result
          append_remaining_positions();
          break;
        }
      } else if (input_value < semi_join_value) {
        // If input value is smaller than the next semi result value, the value qualifies
        // for the anti join result and the iterator is moved to the next input value
        pos_list.push_back(input_row_id);
        ++input_segment_iter;
      } else if (input_value > semi_join_value) {
        // When the input value is larger than the semi result value (both lists are sorted and equal
        // values increases both iterators), all remaining input values are part of the anti join result
        append_remaining_positions();
        break;
      }
    }
    return std::make_shared<PosList>(std::move(pos_list));
  }

  /**
  * Determines the smallest value in a sorted materialized table.
  **/
  T& _table_min_value(std::unique_ptr<MaterializedSegmentList<T>>& sorted_table) {
    DebugAssert(_op != PredicateCondition::Equals,
                "Complete table order is required for _table_min_value() which is only available in the non-equi case");
    DebugAssert(!sorted_table->empty(), "Sorted table has no partitions");

    for (const auto& partition : *sorted_table) {
      if (!partition->empty()) {
        return (*partition)[0].value;
      }
    }

    throw std::logic_error("Every partition is empty");
  }

  /**
  * Determines the largest value in a sorted materialized table.
  **/
  T& _table_max_value(std::unique_ptr<MaterializedSegmentList<T>>& sorted_table) {
    DebugAssert(_op != PredicateCondition::Equals,
                "The table needs to be sorted for _table_max_value() which is only the case in the non-equi case");
    DebugAssert(!sorted_table->empty(), "Sorted table is empty");

    for (size_t partition_id = sorted_table->size() - 1; partition_id < sorted_table->size(); --partition_id) {
      if (!(*sorted_table)[partition_id]->empty()) {
        return (*sorted_table)[partition_id]->back().value;
      }
    }

    throw std::logic_error("Every partition is empty");
  }

  /**
  * Looks for the first value in a sorted materialized table that fulfills the specified condition.
  * Returns the TablePosition of this element and whether a satisfying element has been found.
  **/
  template <typename Function>
  std::optional<TablePosition> _first_value_that_satisfies(std::unique_ptr<MaterializedSegmentList<T>>& sorted_table,
                                                           Function condition) {
    for (size_t partition_id = 0; partition_id < sorted_table->size(); ++partition_id) {
      auto partition = (*sorted_table)[partition_id];
      if (!partition->empty() && condition(partition->back().value)) {
        for (size_t index = 0; index < partition->size(); ++index) {
          if (condition((*partition)[index].value)) {
            return TablePosition(partition_id, index);
          }
        }
      }
    }

    return {};
  }

  /**
  * Looks for the first value in a sorted materialized table that fulfills the specified condition, but searches
  * the table in reverse order. Returns the TablePosition of this element, and a satisfying element has been found.
  **/
  template <typename Function>
  std::optional<TablePosition> _first_value_that_satisfies_reverse(
      std::unique_ptr<MaterializedSegmentList<T>>& sorted_table, Function condition) {
    for (size_t partition_id = sorted_table->size() - 1; partition_id < sorted_table->size(); --partition_id) {
      auto partition = (*sorted_table)[partition_id];
      if (!partition->empty() && condition((*partition)[0].value)) {
        for (size_t index = partition->size() - 1; index < partition->size(); --index) {
          if (condition((*partition)[index].value)) {
            return TablePosition(partition_id, index + 1);
          }
        }
      }
    }

    return {};
  }

  /**
  * Adds the rows without matches for left outer joins for non-equi operators (<, <=, >, >=).
  * This method adds those rows from the left table to the output that do not find a join partner.
  * The outer join for the equality operator is handled in _join_runs instead.
  **/
  void _left_outer_non_equi_join() {
    auto& left_min_value = _table_min_value(_sorted_left_table);
    auto& left_max_value = _table_max_value(_sorted_left_table);
    auto end_of_right_table = _end_of_table(_sorted_right_table);

    if (_op == PredicateCondition::LessThan) {
      // Look for the first right value that is bigger than the smallest left value.
      auto result =
          _first_value_that_satisfies(_sorted_right_table, [&](const T& value) { return value > left_min_value; });
      if (result.has_value()) {
        _emit_left_null_combinations(0, TablePosition(0, 0).to(*result));
      }
    } else if (_op == PredicateCondition::LessThanEquals) {
      // Look for the first right value that is bigger or equal to the smallest left value.
      auto result =
          _first_value_that_satisfies(_sorted_right_table, [&](const T& value) { return value >= left_min_value; });
      if (result.has_value()) {
        _emit_left_null_combinations(0, TablePosition(0, 0).to(*result));
      }
    } else if (_op == PredicateCondition::GreaterThan) {
      // Look for the first right value that is smaller than the biggest left value.
      auto result = _first_value_that_satisfies_reverse(_sorted_right_table,
                                                        [&](const T& value) { return value < left_max_value; });
      if (result.has_value()) {
        _emit_left_null_combinations(0, (*result).to(end_of_right_table));
      }
    } else if (_op == PredicateCondition::GreaterThanEquals) {
      // Look for the first right value that is smaller or equal to the biggest left value.
      auto result = _first_value_that_satisfies_reverse(_sorted_right_table,
                                                        [&](const T& value) { return value <= left_max_value; });
      if (result.has_value()) {
        _emit_left_null_combinations(0, (*result).to(end_of_right_table));
      }
    }
  }

  /**
    * Adds the rows without matches for right outer joins for non-equi operators (<, <=, >, >=).
    * This method adds those rows from the right table to the output that do not find a join partner.
    * The outer join for the equality operator is handled in _join_runs instead.
    **/
  void _right_outer_non_equi_join() {
    auto& right_min_value = _table_min_value(_sorted_right_table);
    auto& right_max_value = _table_max_value(_sorted_right_table);
    auto end_of_left_table = _end_of_table(_sorted_left_table);

    if (_op == PredicateCondition::LessThan) {
      // Look for the last left value that is smaller than the biggest right value.
      auto result = _first_value_that_satisfies_reverse(_sorted_left_table,
                                                        [&](const T& value) { return value < right_max_value; });
      if (result.has_value()) {
        _emit_right_null_combinations(0, (*result).to(end_of_left_table));
      }
    } else if (_op == PredicateCondition::LessThanEquals) {
      // Look for the last left value that is smaller or equal than the biggest right value.
      auto result = _first_value_that_satisfies_reverse(_sorted_left_table,
                                                        [&](const T& value) { return value <= right_max_value; });
      if (result.has_value()) {
        _emit_right_null_combinations(0, (*result).to(end_of_left_table));
      }
    } else if (_op == PredicateCondition::GreaterThan) {
      // Look for the first left value that is bigger than the smallest right value.
      auto result =
          _first_value_that_satisfies(_sorted_left_table, [&](const T& value) { return value > right_min_value; });
      if (result.has_value()) {
        _emit_right_null_combinations(0, TablePosition(0, 0).to(*result));
      }
    } else if (_op == PredicateCondition::GreaterThanEquals) {
      // Look for the first left value that is bigger or equal to the smallest right value.
      auto result =
          _first_value_that_satisfies(_sorted_left_table, [&](const T& value) { return value >= right_min_value; });
      if (result.has_value()) {
        _emit_right_null_combinations(0, TablePosition(0, 0).to(*result));
      }
    }
  }

  /**
  * Performs the join on all clusters in parallel.
  **/
  void _perform_join() {
    std::vector<std::shared_ptr<AbstractTask>> jobs;
    jobs.reserve(_cluster_count);
    // Parallel join for each cluster
    for (size_t cluster_number = 0; cluster_number < _cluster_count; ++cluster_number) {
      // Create output position lists
      _output_pos_lists_left[cluster_number] = std::make_shared<PosList>();
      _output_pos_lists_right[cluster_number] = std::make_shared<PosList>();

      // Avoid empty jobs for inner equi joins
      // TODO(anyone): we can take the sort cut for semi, but not for anti ...
      if ((_mode == JoinMode::Inner || _mode == JoinMode::Semi) && _op == PredicateCondition::Equals) {
        if ((*_sorted_left_table)[cluster_number]->empty() || (*_sorted_right_table)[cluster_number]->empty()) {
          continue;
        }
      }
      jobs.push_back(std::make_shared<JobTask>([this, cluster_number] { this->_join_cluster(cluster_number); }));
      jobs.back()->schedule();
    }

    CurrentScheduler::wait_for_tasks(jobs);

    // The outer joins for the non-equi cases
    // Note: Equi outer joins can be integrated into the main algorithm, while these can not.
    if ((_mode == JoinMode::Left || _mode == JoinMode::Outer) && _op != PredicateCondition::Equals) {
      _left_outer_non_equi_join();
    }
    if ((_mode == JoinMode::Right || _mode == JoinMode::Outer) && _op != PredicateCondition::Equals) {
      _right_outer_non_equi_join();
    }
  }

  /**
  * Concatenates a vector of pos lists into a single new pos list.
  **/
  std::shared_ptr<PosList> _concatenate_pos_lists(std::vector<std::shared_ptr<PosList>>& pos_lists) {
    auto output = std::make_shared<PosList>();

    // Determine the required space
    size_t total_size = 0;
    for (auto& pos_list : pos_lists) {
      total_size += pos_list->size();
    }

    // Move the entries over the output pos list
    output->reserve(total_size);
    for (auto& pos_list : pos_lists) {
      output->insert(output->end(), pos_list->begin(), pos_list->end());
    }

    return output;
  }

  /**
  * Adds the segments from an input table to the output table
  **/
  void _add_output_segments(Segments& output_segments, std::shared_ptr<const Table> input_table,
                            std::shared_ptr<const PosList> pos_list) {
    auto column_count = input_table->column_count();
    for (ColumnID column_id{0}; column_id < column_count; ++column_id) {
      // Add the segment data (in the form of a poslist)
      if (input_table->type() == TableType::References) {
        // Create a pos_list referencing the original segment instead of the reference segment
        auto new_pos_list = _dereference_pos_list(input_table, column_id, pos_list);

        if (input_table->chunk_count() > 0) {
          const auto base_segment = input_table->get_chunk(ChunkID{0})->get_segment(column_id);
          const auto ref_segment = std::dynamic_pointer_cast<const ReferenceSegment>(base_segment);

          auto new_ref_segment = std::make_shared<ReferenceSegment>(ref_segment->referenced_table(),
                                                                    ref_segment->referenced_column_id(), new_pos_list);
          output_segments.push_back(new_ref_segment);
        } else {
          // If there are no Chunks in the input_table, we can't deduce the Table that input_table is referencing to.
          // pos_list will contain only NULL_ROW_IDs anyway, so it doesn't matter which Table the ReferenceSegment that
          // we output is referencing. HACK, but works fine: we create a dummy table and let the ReferenceSegment ref
          // it.
          const auto dummy_table = Table::create_dummy_table(input_table->column_definitions());
          output_segments.push_back(std::make_shared<ReferenceSegment>(dummy_table, column_id, pos_list));
        }
      } else {
        auto new_ref_segment = std::make_shared<ReferenceSegment>(input_table, column_id, pos_list);
        output_segments.push_back(new_ref_segment);
      }
    }
  }

  /**
  * Turns a pos list that is pointing to reference segment entries into a pos list pointing to the original table.
  * This is done because there should not be any reference segments referencing reference segments.
  **/
  std::shared_ptr<PosList> _dereference_pos_list(const std::shared_ptr<const Table>& input_table, ColumnID column_id,
                                                 const std::shared_ptr<const PosList>& pos_list) {
    // Get all the input pos lists so that we only have to pointer cast the segments once
    auto input_pos_lists = std::vector<std::shared_ptr<const PosList>>();
    for (ChunkID chunk_id{0}; chunk_id < input_table->chunk_count(); ++chunk_id) {
      auto base_segment = input_table->get_chunk(chunk_id)->get_segment(column_id);
      auto reference_segment = std::dynamic_pointer_cast<const ReferenceSegment>(base_segment);
      input_pos_lists.push_back(reference_segment->pos_list());
    }

    // Get the row ids that are referenced
    auto new_pos_list = std::make_shared<PosList>();
    for (const auto& row : *pos_list) {
      if (row.is_null()) {
        new_pos_list->push_back(NULL_ROW_ID);
      } else {
        new_pos_list->push_back((*input_pos_lists[row.chunk_id])[row.chunk_offset]);
      }
    }

    return new_pos_list;
  }

 public:
  /**
  * Executes the SortMergeJoin operator.
  **/
  std::shared_ptr<const Table> _on_execute() override {
    bool include_null_left = (_mode == JoinMode::Left || _mode == JoinMode::Outer);
    bool include_null_right = (_mode == JoinMode::Right || _mode == JoinMode::Outer);
    auto radix_clusterer = RadixClusterSort<T>(
        _sort_merge_join.input_table_left(), _sort_merge_join.input_table_right(), _sort_merge_join._column_ids,
        _op == PredicateCondition::Equals, include_null_left, include_null_right, _cluster_count);
    // Sort and cluster the input tables
    auto sort_output = radix_clusterer.execute();
    _sorted_left_table = std::move(sort_output.clusters_left);
    _sorted_right_table = std::move(sort_output.clusters_right);
    _null_rows_left = std::move(sort_output.null_rows_left);
    _null_rows_right = std::move(sort_output.null_rows_right);
    _end_of_left_table = _end_of_table(_sorted_left_table);
    _end_of_right_table = _end_of_table(_sorted_right_table);

    _perform_join();

    if (include_null_left || include_null_right) {
      auto null_output_left = std::make_shared<PosList>();
      auto null_output_right = std::make_shared<PosList>();
      null_output_left->reserve(_null_rows_left->size());
      null_output_right->reserve(_null_rows_right->size());

      // Add the outer join rows which had a null value in their join column
      if (include_null_left) {
        for (auto row_id_left : *_null_rows_left) {
          null_output_left->push_back(row_id_left);
          null_output_right->push_back(NULL_ROW_ID);
        }
      }
      if (include_null_right) {
        for (auto row_id_right : *_null_rows_right) {
          null_output_left->push_back(NULL_ROW_ID);
          null_output_right->push_back(row_id_right);
        }
      }

      _output_pos_lists_left.push_back(null_output_left);
      _output_pos_lists_right.push_back(null_output_right);
    }

    // Intermediate structure for output chunks (to avoid concurrent appending to table)
    std::vector<std::shared_ptr<Segments>> result_chunks(_output_pos_lists_left.size());

    // Determine if writing output in parallel is necessary.
    // As partitions ought to be roughly equally sized, looking at the first should be sufficient.
    const auto write_output_concurrently = _cluster_count > 1 && _output_pos_lists_left[0]->size() > 10'000;

    std::vector<std::shared_ptr<AbstractTask>> output_jobs;
    output_jobs.reserve(_output_pos_lists_left.size());
    for (auto pos_list_id = size_t{0}; pos_list_id < _output_pos_lists_left.size(); ++pos_list_id) {
      auto write_output_fun = [this, pos_list_id, &result_chunks] {
        Segments output_segments;
        _add_output_segments(output_segments, _sort_merge_join.input_table_left(), _output_pos_lists_left[pos_list_id]);
        if (_mode != JoinMode::Semi && _mode != JoinMode::Anti) {
          // In case of semi or anti join, we discard the right join relation.
          _add_output_segments(output_segments, _sort_merge_join.input_table_right(),
                               _output_pos_lists_right[pos_list_id]);
        }

        result_chunks[pos_list_id] = std::make_shared<Segments>(std::move(output_segments));
      };

      if (write_output_concurrently) {
        auto job = std::make_shared<JobTask>(write_output_fun);
        output_jobs.push_back(job);
        output_jobs.back()->schedule();
      } else {
        write_output_fun();
      }
    }

    if (write_output_concurrently) CurrentScheduler::wait_for_tasks(output_jobs);

    auto output_table = _sort_merge_join._initialize_output_table();
    for (auto& chunk : result_chunks) {
      output_table->append_chunk(*chunk);
    }

    // TODO(Bouncner): mark chunks as sorted in case of equality predicate.

    return output_table;
  }
};

}  // namespace opossum
