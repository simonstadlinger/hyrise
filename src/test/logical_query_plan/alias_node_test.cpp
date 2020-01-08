#include <utils/constraint_test_utils.hpp>
#include "gtest/gtest.h"

#include "expression/expression_functional.hpp"
#include "expression/lqp_column_expression.hpp"
#include "logical_query_plan/alias_node.hpp"
#include "logical_query_plan/lqp_utils.hpp"
#include "logical_query_plan/mock_node.hpp"
#include "operators/table_wrapper.hpp"
#include "testing_assert.hpp"
#include "utils/load_table.hpp"
#include "utils/constraint_test_utils.hpp"

using namespace std::string_literals;            // NOLINT
using namespace opossum::expression_functional;  // NOLINT

namespace opossum {

class AliasNodeTest : public ::testing::Test {
 public:
  void SetUp() override {
    mock_node = MockNode::make(MockNode::ColumnDefinitions{{DataType::Int, "a"}, {DataType::Float, "b"}});

    a = lqp_column_(mock_node->get_column("a"));
    b = lqp_column_(mock_node->get_column("b"));

    aliases = {"x", "y"};
    expressions = {b, a};

    alias_node = AliasNode::make(expressions, aliases, mock_node);
  }

  std::vector<std::string> aliases;
  std::vector<std::shared_ptr<AbstractExpression>> expressions;
  std::shared_ptr<MockNode> mock_node;

  std::shared_ptr<AbstractExpression> a, b;
  std::shared_ptr<AliasNode> alias_node;
};

TEST_F(AliasNodeTest, NodeExpressions) {
  ASSERT_EQ(alias_node->node_expressions.size(), 2u);
  EXPECT_EQ(alias_node->node_expressions.at(0), b);
  EXPECT_EQ(alias_node->node_expressions.at(1), a);
}

TEST_F(AliasNodeTest, ShallowEqualsAndCopy) {
  const auto alias_node_copy = alias_node->deep_copy();
  const auto node_mapping = lqp_create_node_mapping(alias_node, alias_node_copy);

  EXPECT_TRUE(alias_node->shallow_equals(*alias_node_copy, node_mapping));
}

TEST_F(AliasNodeTest, HashingAndEqualityCheck) {
  const auto alias_node_copy = alias_node->deep_copy();
  EXPECT_EQ(*alias_node, *alias_node_copy);

  const auto alias_node_other_aliases = AliasNode::make(expressions, std::vector<std::string>{"a", "b"}, mock_node);
  EXPECT_NE(*alias_node, *alias_node_other_aliases);

  const auto other_mock_node =
      MockNode::make(MockNode::ColumnDefinitions{{DataType::Int, "a"}, {DataType::Float, "b"}}, "named");
  const auto expr_a = lqp_column_(other_mock_node->get_column("a"));
  const auto expr_b = lqp_column_(other_mock_node->get_column("b"));
  const auto other_expressions = std::vector<std::shared_ptr<AbstractExpression>>{expr_a, expr_b};
  const auto alias_node_other_expressions = AliasNode::make(other_expressions, aliases, mock_node);
  EXPECT_NE(*alias_node, *alias_node_other_expressions);
  const auto alias_node_other_left_input = AliasNode::make(expressions, aliases, other_mock_node);
  EXPECT_NE(*alias_node, *alias_node_other_left_input);

  EXPECT_NE(alias_node->hash(), alias_node_other_expressions->hash());
  EXPECT_EQ(alias_node->hash(), alias_node_other_left_input->hash());
  // alias_node == alias_node_other_left_input is false
  // but the hash codes of these nodes are equal.
  // The reason for this are the LQPColumnExpressions:
  // Semantically equal LQPColumnExpressions which use semantically equal LQPColumnReferences are evaluated
  // as not equal if the original node of the LQPColumnReferences are semantically equal but not identical
  // (= different StoredTableNode pointers).
  // The hash function does not take the actual pointer into account, so the hashes of
  // semantically equal LQPColumnReferences are equal.
  // The following lines show this fact in detail:
  EXPECT_NE(*a, *expr_a);
  EXPECT_NE(*b, *expr_b);
  EXPECT_EQ(a->hash(), expr_a->hash());
  EXPECT_EQ(b->hash(), expr_b->hash());
  // The expressions under test are not equal since for the AbstractExpression's `operator==`, `_shallow_equals` of
  // the derived class is called. The equal check of two LQPColumnExpression checks the equality of the included
  // LQPColumnReferences, i.e., calls `LQPColumnReference::operator==`.
  // For the equality check of two LQPColumnReferences, the included original nodes (StoredTableNode) have to be
  // identical (equal pointer) and the column ids have to be equal.
  // Since the original nodes of the LQPColumnReferences of the expressions under test are not identical,
  // the equality check fails.
  // The hash function on the other hand uses shallow_hash of the LQPReference, where the pointer is not used for the
  // hash code calculation. Therefore, the hash codes of `a` and `expr_a` are equal.
}

TEST_F(AliasNodeTest, ConstraintsEmpty) {
  EXPECT_TRUE(mock_node->constraints()->empty());
  EXPECT_TRUE(alias_node->constraints()->empty());
}

TEST_F(AliasNodeTest, ConstraintsForwarding) {
  // Recreate MockNode to incorporate two constraints
  //  Primary Key: a, b
  const auto table_constraint_1 =
      TableConstraintDefinition{std::vector<ColumnID>{ColumnID{0}, ColumnID{1}}, IsPrimaryKey::Yes};
  //  Unique: b
  const auto table_constraint_2 = TableConstraintDefinition{std::vector<ColumnID>{ColumnID{1}}, IsPrimaryKey::No};
  const auto table_constraints = TableConstraintDefinitions{table_constraint_1, table_constraint_2};

  mock_node = MockNode::make(MockNode::ColumnDefinitions{{DataType::Int, "a"}, {DataType::Float, "b"}}, "mock_node", table_constraints);
  a = lqp_column_(mock_node->get_column("a"));
  b = lqp_column_(mock_node->get_column("b"));

  // Recreate AliasNode
  aliases = {"x", "y"};
  expressions = {b, a};
  alias_node = AliasNode::make(expressions, aliases, mock_node);

  // Basic check
  const auto lqp_constraints = alias_node->constraints();
  EXPECT_EQ(lqp_constraints->size(), 2);
  // In-depth check
  check_table_constraint_representation(table_constraints, lqp_constraints);
}

}  // namespace opossum
