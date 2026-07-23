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
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query {

struct NowLiteral {
    int64_t offset_seconds{0};
};

using LiteralValue = std::variant<std::string, int64_t, NowLiteral>;

struct QualifiedColumn {
    std::optional<std::string> qualifier;
    std::string                name;
    std::vector<std::string>   path;
};

struct EqPredicate {
    QualifiedColumn column;
    LiteralValue    value;
};

struct InPredicate {
    QualifiedColumn           column;
    std::vector<LiteralValue> values;
};

struct RangePredicate {
    QualifiedColumn column;
    LiteralValue    lower;
    LiteralValue    upper;
};

enum class PredicateBinaryOp { NEQ, LT, LTE, GT, GTE, LIKE, CONTAINS, PREFIX };

struct OpPredicate {
    QualifiedColumn    column;
    PredicateBinaryOp  op;
    LiteralValue       value;
};

using WherePredicate = std::variant<EqPredicate, InPredicate, RangePredicate, OpPredicate>;

struct TableRef {
    std::string              table_name;
    std::optional<std::string> alias;
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

struct SelectStatement {
    bool                         select_all{false};
    std::vector<QualifiedColumn> columns;
    TableRef                     from;
    std::vector<JoinClause>      joins;
    std::vector<WherePredicate>  predicates;
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

using QueryStatement = std::variant<SelectStatement, ShowTablesStatement, DescribeStatement, ExplainStatement>;

} // namespace mldp_pvxs_driver::query
