
#include <memory>

#include "base_test.hpp"
#include "utils/assert.hpp"

#include "concurrency/transaction_context.hpp"
#include "expression/expression_functional.hpp"
#include "expression/pqp_column_expression.hpp"
#include "hyrise.hpp"
#include "operators/get_table.hpp"
#include "operators/maintenance/create_index.hpp"
#include "operators/maintenance/drop_index.hpp"
#include "operators/projection.hpp"
#include "operators/table_wrapper.hpp"
#include "operators/validate.hpp"
#include "storage/table.hpp"
#include "tasks/chunk_compression_task.hpp"

namespace opossum {

using namespace opossum::expression_functional;  // NOLINT

class DropIndexTest : public BaseTest {
 public:
  void SetUp() override {
    test_table = load_table("resources/test_data/tbl/string_int_index.tbl", 3);
    ChunkEncoder::encode_all_chunks(test_table);
    Hyrise::get().storage_manager.add_table(table_name, test_table);

    column_ids->emplace_back(ColumnID{static_cast<ColumnID>(test_table->column_id_by_name("b"))});

    create_index = std::make_shared<CreateIndex>(index_name, true, table_name, column_ids);

    const auto context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);
    create_index->set_transaction_context(context);

    create_index->execute();
    context->commit();
  }

  std::shared_ptr<Table> test_table;
  std::shared_ptr<CreateIndex> create_index;
  std::string index_name = "TestIndex";
  std::shared_ptr<std::vector<ColumnID>> column_ids = std::make_shared<std::vector<ColumnID>>();
  SegmentIndexType index_type;
  std::string table_name = "TestTable";
};

TEST_F(DropIndexTest, NameAndDescription) {
  auto drop_index = std::make_shared<DropIndex>(index_name, true);
  EXPECT_EQ(drop_index->name(), "DropIndex");
  EXPECT_EQ(drop_index->description(DescriptionMode::SingleLine), "DropIndex 'IF EXISTS' 'TestIndex'");
}

TEST_F(DropIndexTest, IndexStatisticsEmpty) {
  EXPECT_EQ(test_table->indexes_statistics().size(), 1);
  auto drop_index = std::make_shared<DropIndex>(index_name, false);

  const auto context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);
  drop_index->set_transaction_context(context);

  drop_index->execute();
  context->commit();
  EXPECT_EQ(test_table->indexes_statistics().size(), 0);
}

TEST_F(DropIndexTest, FailOnWrongIndexName) {
  EXPECT_EQ(test_table->indexes_statistics().size(), 1);
  auto table_wrapper = std::make_shared<TableWrapper>(test_table);
  table_wrapper->execute();
  auto drop_index = std::make_shared<DropIndex>("WrongIndexName", false);

  const auto context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);
  drop_index->set_transaction_context(context);

  EXPECT_THROW(drop_index->execute(), std::logic_error);
  context->rollback(RollbackReason::Conflict);
}

TEST_F(DropIndexTest, NoFailOnWrongIndexNameWithExistsFlag) {
  EXPECT_EQ(test_table->indexes_statistics().size(), 1);
  auto table_wrapper = std::make_shared<TableWrapper>(test_table);
  table_wrapper->execute();
  auto drop_index = std::make_shared<DropIndex>("WrongIndexName", true);

  const auto context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);
  drop_index->set_transaction_context(context);

  EXPECT_NO_THROW(drop_index->execute());
  context->commit();
  EXPECT_EQ(test_table->indexes_statistics().size(), 1);
}

}  // namespace opossum
