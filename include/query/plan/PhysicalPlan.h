//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file PhysicalPlan.h
 * @brief Defines executable physical query-plan nodes. */
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

/** @brief Executable membership subquery and its pushdown classification. */
struct PhysicalInSubquery {
    Predicate                        predicate;
    ColumnType                       column_type{ColumnType::STRING};
    bool                             pushable{false};
    std::shared_ptr<SelectStatement> child;
};

/** @brief Backend, catalog, or derived-table scan executable by the runtime. */
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

/** @brief Executes residual predicates over a physical input. */
struct PhysicalFilter {
    PhysicalNodePtr        input;
    std::vector<Predicate> predicates;
};

/** @brief Evaluates selected physical output columns and expressions. */
struct PhysicalProject {
    PhysicalNodePtr          input;
    std::vector<std::string> columns;
    std::vector<ExpressionPtr> expressions;
    std::vector<std::string> names;
};

/** @brief Limits rows emitted by a physical input. */
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

/** @brief Sorts rows from a physical input using logical sort keys. */
struct PhysicalSort {
    PhysicalNodePtr       input;
    std::vector<SortKey> keys;
};

/** @brief Join modes available to physical execution. */
enum class JoinType { INNER, LEFT_OUTER };
/** @brief Algorithms available to execute a physical join. */
enum class JoinAlgorithm { HASH, NESTED_LOOP, BLOCK_NESTED_LOOP };

/** @brief Pair of physical column names used as a join key. */
struct JoinCondition {
    std::string left_column;
    std::string right_column;
};

/** @brief Join node executed by the hash-join algorithm. */
struct PhysicalHashJoin {
    JoinType                  type{JoinType::INNER};
    JoinCondition             condition;
    JoinAlgorithm             algorithm{JoinAlgorithm::HASH};
    PhysicalNodePtr           left;
    PhysicalNodePtr           right;
    std::vector<std::string>  warnings;
};

/** @brief Join node executed by nested-loop iteration. */
struct PhysicalNestedLoopJoin {
    JoinType        type{JoinType::INNER};
    JoinCondition   condition;
    JoinAlgorithm   algorithm{JoinAlgorithm::NESTED_LOOP};
    PhysicalNodePtr outer;
    PhysicalNodePtr inner;
    bool            correlated_push{false};
};

/** @brief Join node executed by bounded block nested-loop iteration. */
struct PhysicalBlockNestedLoopJoin {
    JoinType                 type{JoinType::INNER};
    JoinCondition            condition;
    JoinAlgorithm            algorithm{JoinAlgorithm::BLOCK_NESTED_LOOP};
    PhysicalNodePtr          outer;
    PhysicalNodePtr          inner;
    std::vector<std::string> warnings;
};

/** @brief Physical command that lists registered and catalog tables. */
struct PhysicalShowTables {
};

/** @brief Physical command that lists registered scalar functions. */
struct PhysicalShowFunctions {
};

/** @brief Physical command that lists registered operators. */
struct PhysicalShowOperators {
};

/** @brief Physical command that describes one table schema. */
struct PhysicalDescribe {
    std::string table_name;
};

/** @brief Physical command that returns textual plan output. */
struct PhysicalExplain {
    std::string plan_text;
};

/** @brief Materializes a child plan into a session or persistent catalog table. */
struct PhysicalCreateTable {
    std::string     table_name;
    bool            temporary{false};
    PhysicalNodePtr query;
};

/** @brief Physical command that removes a catalog table. */
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

/** @brief Variant wrapper that forms an executable physical-plan tree. */
struct PhysicalNode {
    PhysicalNodeVariant value;
};

inline PhysicalNodePtr makeNode(PhysicalNodeVariant value)
{
    return std::make_shared<PhysicalNode>(PhysicalNode{std::move(value)});
}

std::string physicalPlanToString(const PhysicalNodePtr& root);

} // namespace mldp_pvxs_driver::query::plan
