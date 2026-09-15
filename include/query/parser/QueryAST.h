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

/** @brief Scalar function call in a parsed expression. */
struct FunctionCall {
    std::string                name;      ///< Function name (case-insensitive).
    std::vector<ExpressionPtr> arguments; ///< Ordered argument expressions.
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

/** @brief Parsed SELECT statement and its relational clauses. */
struct SelectStatement {
    bool                         select_all{false}; ///< True for SELECT *.
    std::vector<QualifiedColumn> columns;           ///< Explicit selected columns (qualified form).
    std::vector<SelectItem>      select_items;      ///< Computed selected items with optional aliases.
    TableRef                     from;              ///< Primary FROM source.
    std::vector<JoinClause>      joins;             ///< JOIN clauses.
    std::vector<WherePredicate>  predicates;        ///< WHERE predicate list.
    std::vector<OrderByItem>     order_by;          ///< ORDER BY items.
    std::optional<uint64_t>      limit;             ///< LIMIT value if present.
    std::optional<std::string>   page_token;        ///< PAGE TOKEN value for REPL paging if present.
};

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
