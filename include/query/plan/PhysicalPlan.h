//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/IQueryable.h>
#include <query/plan/LogicalPlan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query::plan {

struct PhysicalNode;
using PhysicalNodePtr = std::shared_ptr<PhysicalNode>;

struct PhysicalInSubquery {
    Predicate                        predicate;
    ColumnType                       column_type{ColumnType::STRING};
    bool                             pushable{false};
    std::shared_ptr<SelectStatement> child;
};

struct PhysicalTableScan {
    std::string            table_name;
    std::string            table_alias;
    bool                   qualify_output{false};
    std::vector<Predicate> pushable_predicates;
    std::set<std::string>  projection_hint;
    std::string            ipc_path;
    bool                   arrow_ipc{false};
    std::shared_ptr<SelectStatement> derived_query;
    std::vector<PhysicalInSubquery>  in_subqueries;
    std::shared_ptr<SelectStatement> window_subquery;
    std::optional<std::array<int64_t, 2>> window_literal;
    WindowShardSpec           window_shards{};
};

struct PhysicalFilter {
    PhysicalNodePtr        input;
    std::vector<Predicate> predicates;
};

struct PhysicalProject {
    PhysicalNodePtr          input;
    std::vector<std::string> columns;
    std::vector<ExpressionPtr> expressions;
    std::vector<std::string> names;
};

struct PhysicalLimit {
    PhysicalNodePtr input;
    uint64_t       limit{0};
};

/** Materializing long-form MLDP data into ordered, bounded wide batches. */
struct PhysicalPivot {
    PhysicalNodePtr          input;
    std::string              row_key_column;
    std::string              pivot_key_column;
    std::string              value_column;
    std::vector<std::string> output_column_labels;
    uint32_t                 output_batch_size{4096};
};

struct PhysicalSort {
    PhysicalNodePtr       input;
    std::vector<SortKey> keys;
};

enum class JoinType { INNER, LEFT_OUTER };
enum class JoinAlgorithm { HASH, NESTED_LOOP, BLOCK_NESTED_LOOP };

struct JoinCondition {
    std::string left_column;
    std::string right_column;
};

struct PhysicalHashJoin {
    JoinType                  type{JoinType::INNER};
    JoinCondition             condition;
    JoinAlgorithm             algorithm{JoinAlgorithm::HASH};
    PhysicalNodePtr           left;
    PhysicalNodePtr           right;
    std::vector<std::string>  warnings;
};

struct PhysicalNestedLoopJoin {
    JoinType        type{JoinType::INNER};
    JoinCondition   condition;
    JoinAlgorithm   algorithm{JoinAlgorithm::NESTED_LOOP};
    PhysicalNodePtr outer;
    PhysicalNodePtr inner;
    bool            correlated_push{false};
};

struct PhysicalBlockNestedLoopJoin {
    JoinType                 type{JoinType::INNER};
    JoinCondition            condition;
    JoinAlgorithm            algorithm{JoinAlgorithm::BLOCK_NESTED_LOOP};
    PhysicalNodePtr          outer;
    PhysicalNodePtr          inner;
    std::vector<std::string> warnings;
};

struct PhysicalShowTables {
};

struct PhysicalShowFunctions {
};

struct PhysicalShowOperators {
};

struct PhysicalDescribe {
    std::string table_name;
};

struct PhysicalExplain {
    std::string plan_text;
};

struct PhysicalCreateTable {
    std::string     table_name;
    bool            temporary{false};
    PhysicalNodePtr query;
};

struct PhysicalDropTable {
    std::string table_name;
};

using PhysicalNodeVariant = std::variant<PhysicalTableScan,
                                         PhysicalFilter,
                                         PhysicalProject,
                                         PhysicalSort,
                                         PhysicalLimit,
                                         PhysicalPivot,
                                         PhysicalHashJoin,
                                         PhysicalNestedLoopJoin,
                                         PhysicalBlockNestedLoopJoin,
                                         PhysicalShowTables,
                                         PhysicalShowFunctions,
                                         PhysicalShowOperators,
                                         PhysicalDescribe,
                                         PhysicalExplain,
                                         PhysicalCreateTable,
                                         PhysicalDropTable>;

struct PhysicalNode {
    PhysicalNodeVariant value;
};

inline PhysicalNodePtr makeNode(PhysicalNodeVariant value)
{
    return std::make_shared<PhysicalNode>(PhysicalNode{std::move(value)});
}

std::string physicalPlanToString(const PhysicalNodePtr& root);

} // namespace mldp_pvxs_driver::query::plan
