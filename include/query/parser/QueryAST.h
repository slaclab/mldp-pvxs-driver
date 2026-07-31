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
    int64_t offset_seconds{0};
};

using LiteralValue = std::variant<std::string, int64_t, double, bool, TimestampNsLiteral, DurationNsLiteral, NowLiteral>;

/** @brief Column reference with an optional source qualifier and field path. */
struct QualifiedColumn {
    std::optional<std::string> qualifier;
    std::string                name;
    std::vector<std::string>   path;
};

struct Expression;
using ExpressionPtr = std::shared_ptr<Expression>;

/** @brief Scalar function call in a parsed expression. */
struct FunctionCall {
    std::string                name;
    std::vector<ExpressionPtr> arguments;
};

/** @brief Unary operator expression. */
struct UnaryExpression {
    std::string operator_name;
    ExpressionPtr operand;
};

/** @brief Binary operator expression. */
struct BinaryExpression {
    std::string operator_name;
    ExpressionPtr left;
    ExpressionPtr right;
};

using ExpressionValue = std::variant<LiteralValue, QualifiedColumn, FunctionCall, UnaryExpression, BinaryExpression>;

/** @brief Recursive SQL expression node. */
struct Expression {
    ExpressionValue value;
};

/** @brief Selected expression with an optional output alias. */
struct SelectItem {
    ExpressionPtr              expression;
    std::optional<std::string> alias;
};

/** @brief Equality predicate comparing a column with a literal or expression. */
struct EqPredicate {
    QualifiedColumn column;
    LiteralValue    value;
    ExpressionPtr   expression;
};

/** @brief Literal-list, subquery, or window membership predicate. */
struct InPredicate {
    QualifiedColumn           column;
    std::vector<LiteralValue> values;
    std::vector<ExpressionPtr> expressions;
    std::shared_ptr<SelectStatement> subquery;
    /** @brief Sharding option attached to a time-series window input. */
    struct WindowShardOption {
        std::string  name;
        LiteralValue value;
    };
    std::vector<WindowShardOption> window_options;
};

/** @brief Predicate requiring a non-null column value. */
struct IsNotNullPredicate {
    QualifiedColumn column;
};

/** @brief Predicate requiring a null column value. */
struct IsNullPredicate {
    QualifiedColumn column;
};

/** @brief Inclusive range predicate with literal or expression endpoints. */
struct RangePredicate {
    QualifiedColumn column;
    LiteralValue    lower;
    LiteralValue    upper;
    ExpressionPtr   lower_expression;
    ExpressionPtr   upper_expression;
};

/** @brief Binary comparison operators represented in parsed WHERE predicates. */
enum class PredicateBinaryOp { NEQ, LT, LTE, GT, GTE, LIKE, CONTAINS, PREFIX };

/** @brief Binary comparison predicate other than equality. */
struct OpPredicate {
    QualifiedColumn    column;
    PredicateBinaryOp  op;
    LiteralValue       value;
    ExpressionPtr      expression;
};

using WherePredicate = std::variant<EqPredicate, InPredicate, RangePredicate, OpPredicate, IsNullPredicate, IsNotNullPredicate>;

/** @brief FROM source, optionally named or represented by a derived SELECT. */
struct TableRef {
    std::string              table_name;
    std::optional<std::string> alias;
    std::shared_ptr<SelectStatement> derived_query;
};

/** @brief SQL join modes supported by the parser. */
enum class JoinType { INNER, LEFT_OUTER };

/** @brief Parsed pair of join-side expressions or column references. */
struct JoinCondition {
    QualifiedColumn left;
    QualifiedColumn right;
    ExpressionPtr   expression;
};

/** @brief Parsed JOIN clause and its condition. */
struct JoinClause {
    JoinType      type{JoinType::INNER};
    TableRef      table;
    JoinCondition condition;
};

/** @brief Ordering direction for an ORDER BY item. */
enum class SortDirection { ASCENDING, DESCENDING };

/** @brief One parsed ORDER BY key and direction. */
struct OrderByItem {
    QualifiedColumn column;
    ExpressionPtr   expression;
    SortDirection   direction{SortDirection::ASCENDING};
};

/** @brief Parsed SELECT statement and its relational clauses. */
struct SelectStatement {
    bool                         select_all{false};
    std::vector<QualifiedColumn> columns;
    std::vector<SelectItem>      select_items;
    TableRef                     from;
    std::vector<JoinClause>      joins;
    std::vector<WherePredicate>  predicates;
    std::vector<OrderByItem>      order_by;
    std::optional<uint64_t>      limit;
    std::optional<std::string>   page_token;
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
    std::string table_name;
};

/** @brief Parsed EXPLAIN command wrapping a SELECT statement. */
struct ExplainStatement {
    SelectStatement query;
};

/** @brief Parsed CREATE [TEMP] TABLE AS SELECT statement. */
struct CreateTableStatement {
    std::string     table_name;
    bool            temporary{false};
    SelectStatement query;
};

/** @brief Parsed DROP TABLE command. */
struct DropTableStatement {
    std::string table_name;
};

using QueryStatement = std::variant<SelectStatement, ShowTablesStatement, ShowFunctionsStatement, ShowOperatorsStatement, DescribeStatement, ExplainStatement, CreateTableStatement, DropTableStatement>;

} // namespace mldp_pvxs_driver::query
