#include <memory>
#include <string>
#include <utility>

#include "base_test.hpp"
#include "storage/table.hpp"
#include "tasks/chunk_compression_task.hpp"
#include "testing_assert.hpp"

#include "SQLParser.h"
#include "SQLParserResult.h"

#include "hyrise.hpp"
#include "sql/sql_pipeline.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "sql/sql_plan_cache.hpp"
#include "types.hpp"

namespace opossum {

class DDLStatementTest : public BaseTest {
 protected:
  void SetUp() override {
    Hyrise::reset();

    // We reload table_a every time since it is modified during the test case.
    _table_a = load_table("resources/test_data/tbl/int_float_create_index_test.tbl", 2);
    ChunkEncoder::encode_all_chunks(_table_a);
    Hyrise::get().storage_manager.add_table("table_a", _table_a);
  }

  // Tables modified during test case
  std::shared_ptr<Table> _table_a;

  const std::string _create_index_single_column = "CREATE INDEX myindex ON table_a (a)";
  const std::string _create_index_multi_column = "CREATE INDEX myindex ON table_a (a, b)";
  const std::string _create_index = "CREATE INDEX myindex ON table_a (a)";
  const std::string _alter_table = "ALTER TABLE table_a DROP COLUMN a";
};

void create_index(const std::string statement) {
  auto sql_pipeline = SQLPipelineBuilder{statement}.create_pipeline();

  const auto& [pipeline_status, table] = sql_pipeline.get_result_table();
  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);
}

void check_if_index_exists_correctly(std::shared_ptr<std::vector<ColumnID>> column_ids, std::shared_ptr<Table> table,
                                     int index_count = 1) {
  auto chunk_count = table->chunk_count();
  for (ChunkID id = ChunkID{0}; id < chunk_count; id += 1) {
    auto current_chunk = table->get_chunk(id);
    auto actual_indices = current_chunk->get_indexes(*column_ids);
    EXPECT_EQ(actual_indices.size(), static_cast<size_t>(index_count));
  }
}

TEST_F(DDLStatementTest, CreateIndexSingleColumn) {
  create_index(_create_index_single_column);

  auto column_ids = std::make_shared<std::vector<ColumnID>>();
  column_ids->emplace_back(ColumnID{0});

  auto actual_index = _table_a->indexes_statistics().at(0);

  EXPECT_EQ(actual_index.name, "myindex");
  EXPECT_EQ(actual_index.column_ids, *column_ids);

  check_if_index_exists_correctly(column_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexMultiColumn) {
  create_index(_create_index_multi_column);

  auto column_ids = std::make_shared<std::vector<ColumnID>>();
  column_ids->emplace_back(ColumnID{0});
  column_ids->emplace_back(ColumnID{1});

  auto actual_index = _table_a->indexes_statistics().at(0);

  EXPECT_EQ(actual_index.name, "myindex");
  EXPECT_EQ(actual_index.column_ids, *column_ids);

  check_if_index_exists_correctly(column_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexWithoutName) {
  create_index("CREATE INDEX ON table_a (a)");

  auto column_ids = std::make_shared<std::vector<ColumnID>>();
  column_ids->emplace_back(ColumnID{0});

  auto actual_index = _table_a->indexes_statistics().at(0);

  EXPECT_EQ(actual_index.name, "table_a_a");
  EXPECT_EQ(actual_index.column_ids, *column_ids);

  check_if_index_exists_correctly(column_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexIfNotExistsFirstTime) {
  create_index("CREATE INDEX IF NOT EXISTS myindex ON table_a (a)");

  auto column_ids = std::make_shared<std::vector<ColumnID>>();
  column_ids->emplace_back(ColumnID{0});

  auto actual_index = _table_a->indexes_statistics().at(0);

  EXPECT_EQ(actual_index.name, "myindex");
  EXPECT_EQ(actual_index.column_ids, *column_ids);

  check_if_index_exists_correctly(column_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexExistsFlagSet) {
  create_index(_create_index_single_column);

  auto second_sql_pipeline =
      SQLPipelineBuilder{"CREATE INDEX IF NOT EXISTS myindex ON table_a (a, b)"}.create_pipeline();

  const auto& [second_pipeline_status, second_table] = second_sql_pipeline.get_result_table();
  EXPECT_EQ(second_pipeline_status, SQLPipelineStatus::Success);

  auto single_column_col_ids = std::make_shared<std::vector<ColumnID>>();
  single_column_col_ids->emplace_back(ColumnID{0});

  check_if_index_exists_correctly(single_column_col_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexExistsFlagNotSet) {
  create_index(_create_index_single_column);

  auto second_sql_pipeline = SQLPipelineBuilder{"CREATE INDEX myindex ON table_a (a, b)"}.create_pipeline();

  // TODO(anyone): come up with way to test this without aborting test execution
  // EXPECT_THROW(second_sql_pipeline.get_result_table(), std::exception);

  auto single_column_col_ids = std::make_shared<std::vector<ColumnID>>();
  single_column_col_ids->emplace_back(ColumnID{0});

  check_if_index_exists_correctly(single_column_col_ids, _table_a);
}
TEST_F(DDLStatementTest, CreateIndexIfNotExistsWithoutName) {
  auto sql_pipeline = SQLPipelineBuilder{"CREATE INDEX IF NOT EXISTS ON table_a (a, b)"}.create_pipeline();

  EXPECT_THROW(sql_pipeline.get_result_table(), std::exception);
}

TEST_F(DDLStatementTest, DropIndex) {
  create_index(_create_index_single_column);

  auto sql_pipeline = SQLPipelineBuilder{"DROP INDEX myindex"}.create_pipeline();

  const auto& [pipeline_status, table] = sql_pipeline.get_result_table();
  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  auto single_column_col_ids = std::make_shared<std::vector<ColumnID>>();
  single_column_col_ids->emplace_back(ColumnID{0});

  check_if_index_exists_correctly(single_column_col_ids, _table_a, 0);
}

TEST_F(DDLStatementTest, DropIndexNotExistsNoFlag) {
  auto sql_pipeline = SQLPipelineBuilder{"DROP INDEX myindex"}.create_pipeline();

  // TODO(anyone): come up with way to test this without aborting test execution
  // EXPECT_THROW(sql_pipeline.get_result_table(), std::logic_error);
}

TEST_F(DDLStatementTest, DropIndexNotExistsWithFlag) {
  auto sql_pipeline = SQLPipelineBuilder{"DROP INDEX IF EXISTS myindex"}.create_pipeline();

  EXPECT_NO_THROW(sql_pipeline.get_result_table());
}

TEST_F(DDLStatementTest, AlterTableDropColumn) {
  auto sql_pipeline = SQLPipelineBuilder{_alter_table}.create_pipeline();

  const auto& [pipeline_status, table] = sql_pipeline.get_result_table();
  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  auto targeted_table = Hyrise::get().storage_manager.get_table("table_a");

  EXPECT_EQ(targeted_table->column_count(), 1u);
  EXPECT_EQ(targeted_table->column_name(ColumnID{0}), "b");
}

TEST_F(DDLStatementTest, CreateTableWithTableKeyConstraints) {
  auto sql_pipeline =
      SQLPipelineBuilder{
          "CREATE TABLE a_table (a_int INTEGER, a_long LONG, a_float FLOAT, a_double DOUBLE NULL, a_string VARCHAR(10) "
          "NOT NULL  , PRIMARY KEY ( a_int, a_float ), UNIQUE (a_double))"}
          .create_pipeline();

  const TableColumnDefinitions& column_definitions = TableColumnDefinitions{{"a_int", DataType::Int, false},
                                                                            {"a_long", DataType::Long, false},
                                                                            {"a_float", DataType::Float, false},
                                                                            {"a_double", DataType::Double, true},
                                                                            {"a_string", DataType::String, false}};

  std::shared_ptr<Table> table = Table::create_dummy_table(column_definitions);

  std::unordered_set<ColumnID> column_ids{table->column_id_by_name("a_int"), table->column_id_by_name("a_float")};

  table->add_soft_key_constraint({column_ids, KeyConstraintType::PRIMARY_KEY});
  table->add_soft_key_constraint({{table->column_id_by_name("a_double")}, KeyConstraintType::UNIQUE});

  const auto& [pipeline_status, pipeline_table] = sql_pipeline.get_result_table();

  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  std::shared_ptr<const Table> result_table = Hyrise::get().storage_manager.get_table("a_table");
  EXPECT_TABLE_EQ(result_table, table, OrderSensitivity::No, TypeCmpMode::Strict,
                  FloatComparisonMode::AbsoluteDifference);
}

TEST_F(DDLStatementTest, CreateTableWithColumnConstraints) {
  auto sql_pipeline =
      SQLPipelineBuilder{
          "CREATE TABLE a_table (a_int INTEGER, a_long LONG, a_float FLOAT UNIQUE, a_double DOUBLE NULL PRIMARY KEY, "
          "a_string VARCHAR(10) NOT "
          "NULL)"}
          .create_pipeline();

  const TableColumnDefinitions& column_definitions = TableColumnDefinitions{
      {"a_int", DataType::Int, false},
      {"a_long", DataType::Long, false},
      {"a_float", DataType::Float, false, new std::vector<hsql::ConstraintType>({hsql::ConstraintType::UNIQUE})},
      {"a_double", DataType::Double, true, new std::vector<hsql::ConstraintType>({hsql::ConstraintType::PRIMARY_KEY})},
      {"a_string", DataType::String, false}};

  std::shared_ptr<const Table> table = Table::create_dummy_table(column_definitions);

  const auto& [pipeline_status, pipeline_table] = sql_pipeline.get_result_table();

  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  std::shared_ptr<const Table> result_table = Hyrise::get().storage_manager.get_table("a_table");
  EXPECT_TABLE_EQ(result_table, table, OrderSensitivity::No, TypeCmpMode::Strict,
                  FloatComparisonMode::AbsoluteDifference);
}

}  // namespace opossum
