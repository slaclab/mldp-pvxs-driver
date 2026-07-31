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
    int64_t  slice_ns{1'000'000'000LL};
    uint64_t series_per_shard{1};
};

/** @brief Predicate after schema binding but before executable literal conversion. */
struct PlannerPredicate {
    std::string              column;
    std::string              table_alias;
    PredicateOp              op{PredicateOp::EQ};
    std::vector<PlannerLiteralValue> values;
    ColumnType               column_type{ColumnType::STRING};
    bool                     required_column{false};
    std::set<PredicateOp>    pushable_ops;
    std::set<PredicateOp>    filterable_ops;
};

// An IN predicate whose values are produced by a child SELECT at execution time.
/** @brief Membership predicate whose values are produced by a child SELECT at execution time. */
struct BoundInSubquery {
    PlannerPredicate                 predicate;
    std::shared_ptr<SelectStatement> child;
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
    std::string               table_name;
    std::string               table_alias;
    std::vector<ColumnSchema> schema;
    std::vector<PlannerPredicate> pushable_predicates;
    std::set<std::string>     projection_hint;
    std::string               ipc_path;
    bool                      arrow_ipc{false};
    std::shared_ptr<SelectStatement> derived_query;
    std::vector<BoundInSubquery>     in_subqueries;
    std::shared_ptr<SelectStatement> window_subquery;
    std::optional<std::array<PlannerLiteralValue, 2>> window_literal;
    WindowShardSpec           window_shards{};
};

/** @brief Applies predicates that remain after scan pushdown. */
struct LogicalFilter {
    LogicalNodePtr         input;
    std::vector<PlannerPredicate> predicates;
};

/** @brief Selects output columns and computed expressions. */
struct LogicalProject {
    LogicalNodePtr         input;
    bool                   select_all{false};
    std::vector<std::string> columns;
    std::vector<ExpressionPtr> expressions;
    std::vector<std::string> names;
};

/** @brief Restricts the number of rows emitted by a logical input. */
struct LogicalLimit {
    LogicalNodePtr input;
    uint64_t       limit{0};
};

/** @brief Bound expression and direction used to sort a logical input. */
struct SortKey {
    std::string column;
    ExpressionPtr expression;
    bool        descending{false};
};

/** @brief Orders rows from a logical input by one or more sort keys. */
struct LogicalSort {
    LogicalNodePtr       input;
    std::vector<SortKey> keys;
};

/** @brief Joins two logical inputs with planner-selected bounds and warnings. */
struct LogicalJoin {
    LogicalJoinType             type{LogicalJoinType::INNER};
    LogicalJoinCondition        condition;
    LogicalNodePtr              left;
    LogicalNodePtr              right;
    std::vector<PlannerPredicate> predicates;
    bool                        left_bounded{false};
    bool                        right_bounded{false};
    std::vector<std::string>    warnings;
};

using LogicalNodeVariant = std::variant<LogicalScan, LogicalFilter, LogicalProject, LogicalSort, LogicalLimit, LogicalJoin>;

/** @brief Variant wrapper that forms a logical-plan tree. */
struct LogicalNode {
    LogicalNodeVariant value;
};

inline LogicalNodePtr makeNode(LogicalNodeVariant value)
{
    return std::make_shared<LogicalNode>(LogicalNode{std::move(value)});
}

/** @brief Table reference resolved against a queryable or catalog schema. */
struct BoundTable {
    std::string                 table_name;
    std::string                 table_alias;
    std::vector<ColumnSchema>   schema;
    std::vector<PlannerPredicate> predicates;
    std::string                 ipc_path;
    bool                        arrow_ipc{false};
    std::shared_ptr<SelectStatement> derived_query;
    std::vector<BoundInSubquery>     in_subqueries;
    std::shared_ptr<SelectStatement> window_subquery;
    std::optional<std::array<PlannerLiteralValue, 2>> window_literal;
    WindowShardSpec           window_shards{};
};

/** @brief Bound table and condition for one SELECT join clause. */
struct BoundJoinClause {
    LogicalJoinType    type{LogicalJoinType::INNER};
    BoundTable         table;
    LogicalJoinCondition condition;
};

/** @brief Fully bound SELECT statement ready for logical planning. */
struct BoundSelect {
    BoundTable                  from;
    std::vector<BoundJoinClause> joins;
    bool                        select_all{false};
    std::vector<std::string>    select_columns;
    std::vector<ExpressionPtr>  select_expressions;
    std::vector<std::string>    select_names;
    std::vector<SortKey>        order_by;
    std::optional<uint64_t>     limit;
    std::optional<std::string>  page_token;
};

} // namespace mldp_pvxs_driver::query::plan
