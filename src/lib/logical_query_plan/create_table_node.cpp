#include "create_table_node.hpp"

#include <sstream>

#include "constant_mappings.hpp"
#include "static_table_node.hpp"

namespace opossum {

CreateTableNode::CreateTableNode(const std::string& init_table_name, const bool init_if_not_exists,
                                 const std::shared_ptr<TableKeyConstraints> init_key_constraints)
    : AbstractNonQueryNode(LQPNodeType::CreateTable),
      table_name(init_table_name),
      if_not_exists(init_if_not_exists),
      key_constraints(init_key_constraints) {}

std::string CreateTableNode::description(const DescriptionMode mode) const {
  std::ostringstream stream;

  stream << "[CreateTable] " << (if_not_exists ? "IfNotExists " : "");
  stream << "Name: '" << table_name << "'";

  return stream.str();
}

size_t CreateTableNode::_on_shallow_hash() const {
  auto hash = boost::hash_value(table_name);
  boost::hash_combine(hash, if_not_exists);
  boost::hash_combine(hash, key_constraints);
  return hash;
}

std::shared_ptr<AbstractLQPNode> CreateTableNode::_on_shallow_copy(LQPNodeMapping& node_mapping) const {
  return CreateTableNode::make(table_name, if_not_exists, key_constraints, left_input());
}

bool CreateTableNode::_on_shallow_equals(const AbstractLQPNode& rhs, const LQPNodeMapping& node_mapping) const {
  const auto& create_table_node = static_cast<const CreateTableNode&>(rhs);
  return table_name == create_table_node.table_name && if_not_exists == create_table_node.if_not_exists &&
         *key_constraints == *(create_table_node.key_constraints);
}

}  // namespace opossum
