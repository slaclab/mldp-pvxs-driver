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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query {

struct SelectStatement;

struct NowLiteral {
    int64_t offset_seconds{0};
};

using LiteralValue = std::variant<std::string, int64_t, double, NowLiteral>;

struct QualifiedColumn {
    std::optional<std::string> qualifier;
    std::string                name;
    std::vector<std::string>   path;
};

struct Expression;
using ExpressionPtr = std::shared_ptr<Expression>;

struct FunctionCall {
    std::string                name;
    std::vector<ExpressionPtr> arguments;
};

using ExpressionValue = std::variant<LiteralValue, QualifiedColumn, FunctionCall>;

struct Expression {
    ExpressionValue value;
};

struct SelectItem {
    ExpressionPtr              expression;
    std::optional<std::string> alias;
};

struct EqPredicate {
    QualifiedColumn column;
    LiteralValue    value;
    ExpressionPtr   expression;
};

struct InPredicate {
    QualifiedColumn           column;
    std::vector<LiteralValue> values;
    std::vector<ExpressionPtr> expressions;
    std::shared_ptr<SelectStatement> subquery;
};

struct IsNotNullPredicate {
    QualifiedColumn column;
};

struct RangePredicate {
    QualifiedColumn column;
    LiteralValue    lower;
    LiteralValue    upper;
    ExpressionPtr   lower_expression;
    ExpressionPtr   upper_expression;
};

enum class PredicateBinaryOp { NEQ, LT, LTE, GT, GTE, LIKE, CONTAINS, PREFIX };

struct OpPredicate {
    QualifiedColumn    column;
    PredicateBinaryOp  op;
    LiteralValue       value;
    ExpressionPtr      expression;
};

using WherePredicate = std::variant<EqPredicate, InPredicate, RangePredicate, OpPredicate, IsNotNullPredicate>;

struct TableRef {
    std::string              table_name;
    std::optional<std::string> alias;
    std::shared_ptr<SelectStatement> derived_query;
};

enum class JoinType { INNER, LEFT_OUTER };

struct JoinCondition {
    QualifiedColumn left;
    QualifiedColumn right;
};

struct JoinClause {
    JoinType      type{JoinType::INNER};
    TableRef      table;
    JoinCondition condition;
};

enum class SortDirection { ASCENDING, DESCENDING };

struct OrderByItem {
    QualifiedColumn column;
    ExpressionPtr   expression;
    SortDirection   direction{SortDirection::ASCENDING};
};

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

struct ShowTablesStatement {
};

struct DescribeStatement {
    std::string table_name;
};

struct ExplainStatement {
    SelectStatement query;
};

struct CreateTableStatement {
    std::string     table_name;
    bool            temporary{false};
    SelectStatement query;
};

struct DropTableStatement {
    std::string table_name;
};

using QueryStatement = std::variant<SelectStatement, ShowTablesStatement, DescribeStatement, ExplainStatement, CreateTableStatement, DropTableStatement>;

} // namespace mldp_pvxs_driver::query
