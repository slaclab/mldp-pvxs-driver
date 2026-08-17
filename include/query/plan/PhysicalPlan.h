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
    Predicate                        predicate;                      ///< Executable predicate filled at runtime with subquery results.
    ColumnType                       column_type{ColumnType::STRING};///< Logical type of the membership column.
    bool                             pushable{false};                ///< True if results can be pushed as a backend predicate.
    std::shared_ptr<SelectStatement> child;                          ///< Child SELECT that produces values.
};

/** @brief Backend, catalog, or derived-table scan executable by the runtime. */
struct PhysicalTableScan {
    std::string            table_name;                       ///< Backend or catalog table name.
    std::string            table_alias;                      ///< SQL alias for this scan.
    bool                   qualify_output{false};            ///< True to prefix output columns with the table alias.
    std::vector<Predicate> pushable_predicates;              ///< Predicates forwarded to the backend.
    std::set<std::string>  projection_hint;                  ///< Column names the backend may pre-project.
    std::string            ipc_path;                         ///< Arrow IPC file path for arrow_ipc scans.
    bool                   arrow_ipc{false};                 ///< True for Arrow IPC file scans.
    std::shared_ptr<SelectStatement> derived_query;          ///< Non-null for derived-table scans.
    std::vector<PhysicalInSubquery>  in_subqueries;          ///< IN predicates resolved at execution time.
    std::shared_ptr<SelectStatement> window_subquery;        ///< Window range SELECT resolved at execution time.
    std::optional<std::array<int64_t, 2>> window_literal;    ///< Literal [begin_ns, end_ns] window bounds.
    WindowShardSpec           window_shards{};               ///< Slice and shard settings for windowed scans.
};

/** @brief Executes residual predicates over a physical input. */
struct PhysicalFilter {
    PhysicalNodePtr        input;                    ///< Physical input node.
    std::vector<Predicate> predicates;               ///< Residual predicates applied to each batch.
};

/** @brief Evaluates selected physical output columns and expressions. */
struct PhysicalProject {
    PhysicalNodePtr          input;                  ///< Physical input node.
    std::vector<std::string> columns;               ///< Pass-through output column names.
    std::vector<ExpressionPtr> expressions;         ///< Computed output expressions.
    std::vector<std::string> names;                 ///< Output names for computed columns.
};

/** @brief Limits rows emitted by a physical input. */
struct PhysicalLimit {
    PhysicalNodePtr input;      ///< Physical input node.
    uint64_t       limit{0};   ///< Maximum rows to emit.
};

/** @brief Pivots long-form MLDP time-series into aligned wide record batches. */
struct PhysicalPivot {
    PhysicalNodePtr          input;                          ///< Long-form input node.
    std::string              row_key_column;                 ///< Column used as the output row key (typically "time").
    std::string              pivot_key_column;               ///< Column whose distinct values become output column names (typically "pv").
    std::string              value_column;                   ///< Column providing the values placed at each row/column intersection.
    std::vector<std::string> output_column_labels;           ///< Ordered output column names (one per PV).
    uint32_t                 output_batch_size{4096};        ///< Maximum rows per output record batch.
};

/** @brief Sorts rows from a physical input using logical sort keys. */
struct PhysicalSort {
    PhysicalNodePtr       input; ///< Physical input node.
    std::vector<SortKey> keys;   ///< Ordered sort keys.
};

/** @brief Join modes available to physical execution. */
enum class JoinType { INNER, LEFT_OUTER };
/** @brief Algorithms available to execute a physical join. */
enum class JoinAlgorithm { HASH, NESTED_LOOP, BLOCK_NESTED_LOOP };

/** @brief Pair of physical column names used as a join key. */
struct JoinCondition {
    std::string left_column;    ///< Output column name from the left input.
    std::string right_column;   ///< Output column name from the right input.
};

/** @brief Join node executed by the hash-join algorithm. */
struct PhysicalHashJoin {
    JoinType                  type{JoinType::INNER};          ///< INNER or LEFT OUTER.
    JoinCondition             condition;                       ///< Equi-join column pair.
    JoinAlgorithm             algorithm{JoinAlgorithm::HASH}; ///< Always HASH for this node type.
    PhysicalNodePtr           left;                           ///< Build-side input.
    PhysicalNodePtr           right;                          ///< Probe-side input.
    std::vector<std::string>  warnings;                       ///< Planner warnings about this join.
};

/** @brief Join node executed by nested-loop iteration. */
struct PhysicalNestedLoopJoin {
    JoinType        type{JoinType::INNER};                         ///< INNER or LEFT OUTER.
    JoinCondition   condition;                                     ///< Equi-join column pair.
    JoinAlgorithm   algorithm{JoinAlgorithm::NESTED_LOOP};        ///< Always NESTED_LOOP.
    PhysicalNodePtr outer;                                         ///< Outer (driving) input.
    PhysicalNodePtr inner;                                         ///< Inner (scanned per outer row) input.
    bool            correlated_push{false};                        ///< True when correlated predicates are pushed into inner.
};

/** @brief Join node executed by bounded block nested-loop iteration. */
struct PhysicalBlockNestedLoopJoin {
    JoinType                 type{JoinType::INNER};                       ///< INNER or LEFT OUTER.
    JoinCondition            condition;                                    ///< Equi-join column pair.
    JoinAlgorithm            algorithm{JoinAlgorithm::BLOCK_NESTED_LOOP}; ///< Always BLOCK_NESTED_LOOP.
    PhysicalNodePtr          outer;                                        ///< Outer input.
    PhysicalNodePtr          inner;                                        ///< Inner input.
    std::vector<std::string> warnings;                                     ///< Planner warnings about this join.
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
    std::string table_name; ///< Name of the table to describe.
};

/** @brief Physical command that returns textual plan output. */
struct PhysicalExplain {
    std::string plan_text; ///< Pre-formatted plan text to display.
};

/** @brief Materializes a child plan into a session or persistent catalog table. */
struct PhysicalCreateTable {
    std::string     table_name;      ///< Name of the table to create.
    bool            temporary{false};///< True for CREATE TEMP TABLE (session-scoped).
    PhysicalNodePtr query;           ///< Source SELECT query.
};

/** @brief Physical command that removes a catalog table. */
struct PhysicalDropTable {
    std::string table_name; ///< Name of the table to drop.
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
    PhysicalNodeVariant value; ///< Variant holding the concrete physical plan node.
};

/** @brief Allocates a PhysicalNode wrapping the given variant value.
 * @param[in] value Physical plan node variant.
 * @return Shared pointer to the new node. */
inline PhysicalNodePtr makeNode(PhysicalNodeVariant value)
{
    return std::make_shared<PhysicalNode>(PhysicalNode{std::move(value)});
}

/** @brief Renders a physical plan tree as an indented human-readable string.
 * @param[in] root Physical plan root node; may be null.
 * @return Formatted plan text. */
std::string physicalPlanToString(const PhysicalNodePtr& root);

} // namespace mldp_pvxs_driver::query::plan
