//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryAST.h
 * @brief Defines the parsed SQL abstract syntax tree. */
#pragma once

#include <query/LiteralValue.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query {

struct SelectStatement;

/** @brief SQL NOW expression represented as an offset in seconds. */
struct NowLiteral {
    int64_t offset_seconds{0}; ///< Offset from NOW() in seconds; 0 = the current time.
};

using LiteralValue = std::variant<std::string, int64_t, double, bool, TimestampNsLiteral, DurationNsLiteral, NowLiteral>;

/** @brief Column reference with an optional source qualifier and field path. */
struct QualifiedColumn {
    std::optional<std::string> qualifier; ///< Optional table alias or schema qualifier.
    std::string                name;      ///< Column name.
    std::vector<std::string>   path;      ///< Optional field path for nested column access.
};

struct Expression;
using ExpressionPtr = std::shared_ptr<Expression>;

struct WindowSpec;

/** @brief Scalar, aggregate or window function call in a parsed expression. */
struct FunctionCall {
    std::string                 name;      ///< Function name (case-insensitive).
    std::vector<ExpressionPtr>  arguments; ///< Ordered argument expressions.
    bool                        star{false};     ///< True for an aggregate called with '*' (COUNT(*)).
    bool                        distinct{false}; ///< True for an aggregate called with DISTINCT (COUNT(DISTINCT x)).
    std::shared_ptr<WindowSpec> over;            ///< Window specification for `f(...) OVER ...`; null for plain calls.
};

/** @brief Unary operator expression. */
struct UnaryExpression {
    std::string   operator_name; ///< Operator symbol.
    ExpressionPtr operand;       ///< Expression the operator is applied to.
};

/** @brief Binary operator expression. */
struct BinaryExpression {
    std::string   operator_name; ///< Operator symbol.
    ExpressionPtr left;          ///< Left operand.
    ExpressionPtr right;         ///< Right operand.
};

using ExpressionValue = std::variant<LiteralValue, QualifiedColumn, FunctionCall, UnaryExpression, BinaryExpression>;

/** @brief Recursive SQL expression node. */
struct Expression {
    ExpressionValue value; ///< Variant holding the concrete expression.
};

/** @brief Selected expression with an optional output alias. */
struct SelectItem {
    ExpressionPtr              expression; ///< Selected expression.
    std::optional<std::string> alias;      ///< Optional output column alias.
};

/** @brief Equality predicate comparing a column with a literal or expression. */
struct EqPredicate {
    QualifiedColumn column;     ///< Column being compared.
    LiteralValue    value;      ///< Literal right-hand side.
    ExpressionPtr   expression; ///< Expression right-hand side (alternative to value).
};

/** @brief Literal-list, subquery, or window membership predicate. */
struct InPredicate {
    QualifiedColumn            column;      ///< Column tested for membership.
    std::vector<LiteralValue>  values;      ///< Literal membership list.
    std::vector<ExpressionPtr> expressions; ///< Expression membership list.
    std::shared_ptr<SelectStatement> subquery; ///< Subquery that produces membership values at execution time.
    /** @brief Sharding option attached to a time-series window input. */
    struct WindowShardOption {
        std::string  name;  ///< Option name (e.g. "slice", "series_per_shard").
        LiteralValue value; ///< Option value.
    };
    std::vector<WindowShardOption> window_options; ///< Window sharding options attached to a time-series window IN.
};

/** @brief Predicate requiring a non-null column value. */
struct IsNotNullPredicate {
    QualifiedColumn column; ///< Column tested for non-null.
};

/** @brief Predicate requiring a null column value. */
struct IsNullPredicate {
    QualifiedColumn column; ///< Column tested for null.
};

/** @brief Inclusive range predicate with literal or expression endpoints. */
struct RangePredicate {
    QualifiedColumn column;           ///< Column tested.
    LiteralValue    lower;            ///< Inclusive lower bound.
    LiteralValue    upper;            ///< Inclusive upper bound.
    ExpressionPtr   lower_expression; ///< Expression lower bound (alternative to lower).
    ExpressionPtr   upper_expression; ///< Expression upper bound (alternative to upper).
};

/** @brief Binary comparison operators represented in parsed WHERE predicates. */
enum class PredicateBinaryOp { NEQ, LT, LTE, GT, GTE, LIKE, CONTAINS, PREFIX };

/** @brief Binary comparison predicate other than equality. */
struct OpPredicate {
    QualifiedColumn   column;     ///< Column being compared.
    PredicateBinaryOp op;         ///< Comparison operator.
    LiteralValue      value;      ///< Literal right-hand side.
    ExpressionPtr     expression; ///< Expression right-hand side (alternative to value).
};

using WherePredicate = std::variant<EqPredicate, InPredicate, RangePredicate, OpPredicate, IsNullPredicate, IsNotNullPredicate>;

/** @brief Boolean WHERE tree (OR, NOT, or a parenthesised AND) over leaf predicates; evaluated locally. */
struct WherePredicateTree {
    /** @brief Node kind: a leaf predicate or a boolean connective over @c children. */
    enum class Kind { LEAF, AND, OR, NOT };
    Kind                            kind{Kind::LEAF}; ///< Node kind.
    WherePredicate                  leaf;             ///< Leaf predicate when @c kind is LEAF.
    std::vector<WherePredicateTree> children;         ///< Operands for AND/OR (two or more) and NOT (one).
};

/** @brief FROM source, optionally named or represented by a derived SELECT. */
struct TableRef {
    std::string                      table_name;    ///< Physical or virtual table name.
    std::optional<std::string>       alias;         ///< Optional SQL alias.
    std::shared_ptr<SelectStatement> derived_query; ///< Non-null for derived-table (subquery-in-FROM) references.
};

/** @brief SQL join modes supported by the parser. */
enum class JoinType { INNER, LEFT_OUTER };

/** @brief Parsed pair of join-side expressions or column references. */
struct JoinCondition {
    QualifiedColumn left;       ///< Left-side join column reference.
    QualifiedColumn right;      ///< Right-side join column reference.
    ExpressionPtr   expression; ///< Optional join expression (alternative to column pair).
};

/** @brief Parsed JOIN clause and its condition. */
struct JoinClause {
    JoinType      type{JoinType::INNER}; ///< Join type.
    TableRef      table;                 ///< Right-side table reference.
    JoinCondition condition;             ///< Join condition.
};

/** @brief Ordering direction for an ORDER BY item. */
enum class SortDirection { ASCENDING, DESCENDING };

/** @brief One parsed ORDER BY key and direction. */
struct OrderByItem {
    QualifiedColumn column;                              ///< Column reference for ORDER BY.
    ExpressionPtr   expression;                          ///< Expression for ORDER BY (alternative to column).
    SortDirection   direction{SortDirection::ASCENDING}; ///< ASCENDING or DESCENDING.
};

/** @brief Unit of a window frame: physical rows or a value range of the ORDER BY key. */
enum class WindowFrameUnit { ROWS, RANGE };

/** @brief Kind of one window frame bound. */
enum class WindowBoundKind { UNBOUNDED_PRECEDING, PRECEDING, CURRENT_ROW, FOLLOWING, UNBOUNDED_FOLLOWING };

/** @brief Parsed window frame bound (`5 PRECEDING`, `CURRENT ROW`, ...). */
struct WindowFrameBound {
    WindowBoundKind kind{WindowBoundKind::CURRENT_ROW}; ///< Bound kind.
    ExpressionPtr   offset;                             ///< Offset for PRECEDING / FOLLOWING; null otherwise.
};

/** @brief Parsed `ROWS | RANGE BETWEEN start AND end` frame clause. */
struct WindowFrame {
    WindowFrameUnit  unit{WindowFrameUnit::RANGE}; ///< Frame unit.
    WindowFrameBound start;                        ///< Frame start bound.
    WindowFrameBound end;                          ///< Frame end bound.
};

/** @brief Parsed window specification: `[base] [PARTITION BY ...] [ORDER BY ...] [frame]`. */
struct WindowSpec {
    std::optional<std::string> base_name;    ///< Named window from the WINDOW clause this spec extends.
    std::vector<ExpressionPtr> partition_by; ///< PARTITION BY expressions.
    std::vector<OrderByItem>   order_by;     ///< ORDER BY items inside the window.
    std::optional<WindowFrame> frame;        ///< Explicit frame; default frame when absent.
};

/** @brief One `name AS (spec)` entry of the WINDOW clause. */
struct NamedWindow {
    std::string name; ///< Window name.
    WindowSpec  spec; ///< Window definition.
};

/** @brief Parsed DISTINCT / DISTINCT ON clause. */
struct DistinctClause {
    bool                       distinct{false}; ///< True for SELECT DISTINCT or DISTINCT ON.
    std::vector<ExpressionPtr> on;              ///< DISTINCT ON key expressions; empty for plain DISTINCT.
};

/** @brief One UNION [ALL] branch appended to a SELECT. */
struct SetOperand {
    bool                             all{false}; ///< True for UNION ALL (keep duplicates), false for UNION.
    std::shared_ptr<SelectStatement> query;      ///< Branch query; may itself carry ORDER BY / LIMIT when parenthesised.
};

/** @brief Parsed SELECT statement and its relational clauses. */
struct SelectStatement {
    bool                         distinct{false};   ///< True for SELECT DISTINCT or DISTINCT ON.
    std::vector<ExpressionPtr>   distinct_on;       ///< DISTINCT ON key expressions; empty for whole-row DISTINCT.
    bool                         select_all{false}; ///< True for SELECT *.
    std::vector<QualifiedColumn> columns;           ///< Explicit selected columns (qualified form).
    std::vector<SelectItem>      select_items;      ///< Computed selected items with optional aliases.
    TableRef                     from;              ///< Primary FROM source.
    std::vector<JoinClause>      joins;             ///< JOIN clauses.
    std::vector<WherePredicate>  predicates;        ///< Top-level WHERE AND conjuncts (pushdown candidates).
    std::vector<WherePredicateTree> predicate_groups; ///< Top-level WHERE conjuncts that are OR/NOT trees; filtered locally.
    std::vector<ExpressionPtr>   group_by;          ///< GROUP BY key expressions.
    ExpressionPtr                having;            ///< HAVING condition; null when absent.
    std::vector<OrderByItem>     order_by;          ///< ORDER BY items.
    std::vector<NamedWindow>     named_windows;     ///< WINDOW clause definitions.
    std::optional<uint64_t>      limit;             ///< LIMIT value if present.
    std::optional<std::string>   page_token;        ///< PAGE TOKEN value for REPL paging if present.
    std::vector<SetOperand>      set_operations;    ///< UNION [ALL] branches after this SELECT; order_by/limit/page_token then apply to the whole union.
};

/** @brief Returns the first branch of a UNION: @p statement without its set operations and union-level ORDER BY / LIMIT / PAGE TOKEN.
 * @param[in] statement SELECT carrying set operations.
 * @return Head SELECT of the union. */
inline SelectStatement setOperationHead(SelectStatement statement)
{
    statement.set_operations.clear();
    statement.order_by.clear();
    statement.limit.reset();
    statement.page_token.reset();
    return statement;
}

/** @brief Parsed SHOW TABLES command. */
struct ShowTablesStatement {
};

/** @brief Parsed SHOW FUNCTIONS command. */
struct ShowFunctionsStatement {
};

/** @brief Parsed SHOW OPERATORS command. */
struct ShowOperatorsStatement {
};

/** @brief Parsed DESCRIBE command for one table. */
struct DescribeStatement {
    std::string table_name; ///< Name of the table to describe.
};

/** @brief Parsed EXPLAIN command wrapping a SELECT statement. */
struct ExplainStatement {
    SelectStatement query; ///< SELECT statement to explain.
};

/** @brief Parsed CREATE [TEMP] TABLE AS SELECT statement. */
struct CreateTableStatement {
    std::string     table_name;       ///< Name of the table to create.
    bool            temporary{false}; ///< True for CREATE TEMP TABLE.
    SelectStatement query;            ///< Source SELECT statement.
};

/** @brief Parsed DROP TABLE command. */
struct DropTableStatement {
    std::string table_name; ///< Name of the table to drop.
};

/** @brief Discriminated union of all supported SQL statement types. */
using QueryStatement = std::variant<SelectStatement, ShowTablesStatement, ShowFunctionsStatement, ShowOperatorsStatement, DescribeStatement, ExplainStatement, CreateTableStatement, DropTableStatement>;

} // namespace mldp_pvxs_driver::query
