//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file LogicalPlan.h
 * @brief Defines bound logical query-plan nodes and scan requirements. */
#pragma once

#include <query/IQueryable.h>
#include <query/parser/QueryAST.h>

#include <array>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query::plan {

using PlannerLiteralValue = std::variant<std::string, int64_t, double, bool, TimestampNsLiteral, DurationNsLiteral, NowLiteral>;

/** @brief Splits a time window into duration slices and PV shards. */
struct WindowShardSpec {
    int64_t  slice_ns{0};         ///< Time-slice duration in nanoseconds; 0 = auto-choose from window duration.
    uint64_t series_per_shard{1}; ///< PV series per backend shard; 1 = one request per series.
};

/** @brief Predicate after schema binding but before executable literal conversion. */
struct PlannerPredicate {
    std::string              column;                         ///< Column name this predicate applies to.
    std::string              table_alias;                    ///< Optional table alias qualifying the column.
    PredicateOp              op{PredicateOp::EQ};            ///< Comparison or membership operator.
    std::vector<PlannerLiteralValue> values;                 ///< Predicate operands in planner literal form.
    ColumnType               column_type{ColumnType::STRING};///< Resolved logical type of the column.
    bool                     required_column{false};         ///< True if the column predicate is mandatory for execution.
    std::set<PredicateOp>    pushable_ops;                   ///< Operators the backend accepts for this column.
    std::set<PredicateOp>    filterable_ops;                 ///< Operators supported by the local Arrow filter layer.
};

/** @brief Boolean tree of bound predicates (WHERE OR/NOT) on one table; never pushed down. */
struct PlannerPredicateGroup {
    using Kind = PredicateGroup::Kind;
    Kind                               kind{Kind::LEAF}; ///< Node kind.
    PlannerPredicate                   leaf;             ///< Leaf predicate when @c kind is LEAF.
    std::vector<PlannerPredicateGroup> children;         ///< Operands for AND/OR and NOT.
};

/** @brief Membership predicate whose values are produced by a child SELECT at execution time. */
struct BoundInSubquery {
    PlannerPredicate                 predicate; ///< Predicate template filled with subquery results at execution time.
    std::shared_ptr<SelectStatement> child;     ///< Child SELECT that produces the membership values.
};

/** @brief Join modes available to logical planning. */
enum class LogicalJoinType { INNER, LEFT_OUTER };

/** @brief Pair of bound column names used as a logical join condition. */
struct LogicalJoinCondition {
    std::string left_column;
    std::string right_column;
};

struct LogicalNode;
using LogicalNodePtr = std::shared_ptr<LogicalNode>;

/** @brief Logical source scan with pushdown candidates and runtime inputs. */
struct LogicalScan {
    std::string               table_name;                    ///< Backend or catalog table name.
    std::string               table_alias;                   ///< SQL alias for this table reference.
    std::vector<ColumnSchema> schema;                        ///< Resolved column schema for this table.
    std::vector<PlannerPredicate> pushable_predicates;       ///< Predicates that can be pushed to the backend.
    std::set<std::string>     projection_hint;               ///< Set of column names the backend may pre-project.
    std::string               ipc_path;                      ///< Path to an Arrow IPC file for arrow_ipc scans.
    bool                      arrow_ipc{false};              ///< True when this scan reads from an Arrow IPC file.
    std::shared_ptr<SelectStatement> derived_query;          ///< Non-null for derived-table (subquery-in-FROM) scans.
    std::vector<BoundInSubquery>     in_subqueries;          ///< IN predicates whose values are produced by child SELECTs.
    std::shared_ptr<SelectStatement> window_subquery;        ///< Time-series window range produced by a child SELECT.
    std::optional<std::array<PlannerLiteralValue, 2>> window_literal; ///< Literal [begin, end] time-series window in planner values.
    WindowShardSpec           window_shards{};               ///< Slice and series-per-shard settings for windowed scans.
    bool                      projection_explicit{false};    ///< True when projection_hint came from an explicit select list.
};

/** @brief Applies predicates that remain after scan pushdown. */
struct LogicalFilter {
    LogicalNodePtr         input;                    ///< Input plan node to filter.
    std::vector<PlannerPredicate> predicates;        ///< Residual predicates applied after scan pushdown.
    std::vector<PlannerPredicateGroup> predicate_groups; ///< OR/NOT predicate trees; a row passes when every tree is true.
    std::vector<ExpressionPtr> conditions;           ///< Boolean expressions (e.g. column-to-column comparisons); a row passes when all are true.
};

/** @brief Selects output columns and computed expressions. */
struct LogicalProject {
    LogicalNodePtr         input;                    ///< Input plan node to project.
    bool                   select_all{false};        ///< True for SELECT *.
    std::vector<std::string> columns;               ///< Explicit output column names.
    std::vector<ExpressionPtr> expressions;         ///< Computed expressions for derived columns.
    std::vector<std::string> names;                 ///< Output names corresponding to expressions.
    bool                   distinct{false};          ///< True for SELECT DISTINCT: drop duplicate output rows.
    std::vector<ExpressionPtr> distinct_on;         ///< DISTINCT ON keys evaluated on the input; empty compares whole output rows.
};

/** @brief Restricts the number of rows emitted by a logical input. */
struct LogicalLimit {
    LogicalNodePtr input;       ///< Input plan node.
    uint64_t       limit{0};   ///< Maximum rows to emit.
};

/** @brief Bound expression and direction used to sort a logical input. */
struct SortKey {
    std::string column;             ///< Output column name to sort by.
    ExpressionPtr expression;       ///< Optional computed sort expression.
    bool        descending{false};  ///< True for descending order.
};

/** @brief One aggregate computed per group. */
struct AggregateCall {
    std::string   function;        ///< Lower-case AggregateRegistry function name (count, sum, ...).
    ExpressionPtr argument;        ///< Input expression; null for COUNT(*).
    bool          distinct{false}; ///< True for COUNT(DISTINCT x).
    std::string   name;            ///< Internal output column name.
};

/** @brief One GROUP BY key evaluated on the aggregate input. */
struct GroupKey {
    ExpressionPtr expression; ///< Key expression bound against the input tables.
    std::string   name;       ///< Internal output column name.
};

/** @brief GROUP BY / aggregate specification.
 *
 *  The aggregate output holds one column per key followed by one per aggregate,
 *  named by GroupKey::name and AggregateCall::name.  HAVING and the ORDER BY /
 *  projection above the aggregate reference those internal names. */
struct AggregateSpec {
    std::vector<GroupKey>      keys;       ///< GROUP BY keys; empty for a single global group.
    std::vector<AggregateCall> aggregates; ///< Aggregates computed per group.
    ExpressionPtr              having;     ///< Boolean HAVING condition over the aggregate output; may be null.
};

/** @brief Groups input rows and computes aggregates (GROUP BY). */
struct LogicalAggregate {
    LogicalNodePtr input; ///< Input plan node (scan, filter or join).
    AggregateSpec  spec;  ///< Keys, aggregates and HAVING condition.
};

/** @brief Resolved window frame; offsets are in rows (ROWS) or ORDER BY key units (RANGE; nanoseconds for timestamps). */
struct WindowFrameSpec {
    bool            rows{false};                                       ///< True for ROWS, false for RANGE.
    WindowBoundKind start{WindowBoundKind::UNBOUNDED_PRECEDING};       ///< Frame start kind.
    int64_t         start_offset{0};                                   ///< Start offset for PRECEDING / FOLLOWING.
    WindowBoundKind end{WindowBoundKind::CURRENT_ROW};                 ///< Frame end kind.
    int64_t         end_offset{0};                                     ///< End offset for PRECEDING / FOLLOWING.
};

/** @brief One window function call computed per input row. */
struct WindowCall {
    std::string     function;        ///< Lower-case function name (row_number, lag, sum, ...).
    ExpressionPtr   argument;        ///< Value argument; null for ROW_NUMBER/RANK/DENSE_RANK and COUNT(*).
    int64_t         offset{1};       ///< Row offset for LAG / LEAD.
    ExpressionPtr   default_value;   ///< LAG / LEAD default when the offset row is outside the partition; may be null.
    WindowFrameSpec frame;           ///< Frame used by value and aggregate functions.
    std::string     name;            ///< Internal output column name (__win_N).
};

/** @brief Window calls sharing one PARTITION BY / ORDER BY, evaluated over a single sort. */
struct WindowGroup {
    std::vector<ExpressionPtr> partition_by; ///< Partition key expressions.
    std::vector<SortKey>       order_by;     ///< Order keys inside each partition (SortKey::expression is evaluated).
    std::vector<WindowCall>    calls;        ///< Calls computed by this group.
};

/** @brief Appends window function results (one column per call) to every input row. */
struct LogicalWindow {
    LogicalNodePtr           input;  ///< Input plan node.
    std::vector<WindowGroup> groups; ///< Window groups, evaluated in order; later groups see earlier outputs.
};

/** @brief Orders rows from a logical input by one or more sort keys. */
struct LogicalSort {
    LogicalNodePtr       input; ///< Input plan node.
    std::vector<SortKey> keys;  ///< Ordered sort keys.
};

/** @brief Joins two logical inputs with planner-selected bounds and warnings. */
struct LogicalJoin {
    LogicalJoinType             type{LogicalJoinType::INNER}; ///< INNER or LEFT OUTER join.
    LogicalJoinCondition        condition;                    ///< Equi-join column pair.
    LogicalNodePtr              left;                         ///< Left input plan node.
    LogicalNodePtr              right;                        ///< Right input plan node.
    std::vector<PlannerPredicate> predicates;                 ///< Residual predicates evaluated post-join.
    bool                        left_bounded{false};          ///< True if the left side is bounded by a LIMIT.
    bool                        right_bounded{false};         ///< True if the right side is bounded by a LIMIT.
    std::vector<std::string>    warnings;                     ///< Planner-generated warnings about this join.
};

using LogicalNodeVariant = std::variant<LogicalScan, LogicalFilter, LogicalProject, LogicalSort, LogicalLimit, LogicalJoin, LogicalAggregate, LogicalWindow>;

/** @brief Variant wrapper that forms a logical-plan tree. */
struct LogicalNode {
    LogicalNodeVariant value; ///< Variant holding the concrete logical plan node.
};

/** @brief Allocates a LogicalNode wrapping the given variant value.
 * @param[in] value Logical plan node variant.
 * @return Shared pointer to the new node. */
inline LogicalNodePtr makeNode(LogicalNodeVariant value)
{
    return std::make_shared<LogicalNode>(LogicalNode{std::move(value)});
}

/** @brief Table reference resolved against a queryable or catalog schema. */
struct BoundTable {
    std::string                 table_name;                  ///< Backend or catalog table name.
    std::string                 table_alias;                 ///< SQL alias for this table reference.
    std::vector<ColumnSchema>   schema;                      ///< Resolved column schema.
    std::vector<PlannerPredicate> predicates;                ///< Bound predicates against this table.
    std::vector<PlannerPredicateGroup> predicate_groups;     ///< Bound OR/NOT predicate trees, filtered locally.
    std::string                 ipc_path;                    ///< Path to an Arrow IPC file for arrow_ipc scans.
    bool                        arrow_ipc{false};            ///< True when this table is an Arrow IPC file scan.
    std::shared_ptr<SelectStatement> derived_query;          ///< Non-null for derived-table scans.
    std::vector<BoundInSubquery>     in_subqueries;          ///< IN predicates with child SELECT producers.
    std::shared_ptr<SelectStatement> window_subquery;        ///< Time-series window from a child SELECT.
    std::optional<std::array<PlannerLiteralValue, 2>> window_literal; ///< Literal [begin, end] time-series window.
    WindowShardSpec           window_shards{};               ///< Slice and series-per-shard settings.
};

/** @brief Bound table and condition for one SELECT join clause. */
struct BoundJoinClause {
    LogicalJoinType    type{LogicalJoinType::INNER}; ///< Join type.
    BoundTable         table;                        ///< Right-side bound table reference.
    LogicalJoinCondition condition;                  ///< Equi-join column pair.
};

/** @brief Fully bound SELECT statement ready for logical planning. */
struct BoundSelect {
    BoundTable                  from;                        ///< Primary FROM table.
    std::vector<BoundJoinClause> joins;                     ///< JOIN clauses.
    bool                        distinct{false};             ///< True for SELECT DISTINCT.
    std::vector<ExpressionPtr>  distinct_on;                 ///< DISTINCT ON key expressions.
    std::shared_ptr<const AggregateSpec> aggregate;          ///< Grouping step for aggregate queries; null otherwise.
    std::vector<WindowGroup>    windows;                     ///< Window groups evaluated after grouping; empty without OVER.
    bool                        select_all{false};           ///< True for SELECT *.
    std::vector<std::string>    select_columns;              ///< Explicit output column names.
    std::vector<ExpressionPtr>  select_expressions;         ///< Computed expressions for derived columns.
    std::vector<std::string>    select_names;               ///< Output names corresponding to select_expressions.
    std::vector<ColumnType>     select_types;               ///< Types of select_expressions when known (same size), else empty.
    std::vector<SortKey>        order_by;                   ///< ORDER BY sort keys.
    std::optional<uint64_t>     limit;                      ///< LIMIT value if present.
    std::optional<std::string>  page_token;                 ///< PAGE TOKEN value for REPL paging if present.
    std::vector<ExpressionPtr>  conditions;                 ///< WHERE column-to-column comparisons applied after joins.
};

} // namespace mldp_pvxs_driver::query::plan
