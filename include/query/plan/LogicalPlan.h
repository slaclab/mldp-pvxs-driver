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
#include <query/parser/QueryAST.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query::plan {

using PlannerLiteralValue = std::variant<std::string, int64_t, bool, NowLiteral>;

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

enum class LogicalJoinType { INNER, LEFT_OUTER };

struct LogicalJoinCondition {
    std::string left_column;
    std::string right_column;
};

struct LogicalNode;
using LogicalNodePtr = std::shared_ptr<LogicalNode>;

struct LogicalScan {
    std::string               table_name;
    std::string               table_alias;
    std::vector<ColumnSchema> schema;
    std::vector<PlannerPredicate> pushable_predicates;
    std::set<std::string>     projection_hint;
};

struct LogicalFilter {
    LogicalNodePtr         input;
    std::vector<PlannerPredicate> predicates;
};

struct LogicalProject {
    LogicalNodePtr         input;
    bool                   select_all{false};
    std::vector<std::string> columns;
};

struct LogicalLimit {
    LogicalNodePtr input;
    uint64_t       limit{0};
};

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

using LogicalNodeVariant = std::variant<LogicalScan, LogicalFilter, LogicalProject, LogicalLimit, LogicalJoin>;

struct LogicalNode {
    LogicalNodeVariant value;
};

inline LogicalNodePtr makeNode(LogicalNodeVariant value)
{
    return std::make_shared<LogicalNode>(LogicalNode{std::move(value)});
}

struct BoundTable {
    std::string                 table_name;
    std::string                 table_alias;
    std::vector<ColumnSchema>   schema;
    std::vector<PlannerPredicate> predicates;
};

struct BoundJoinClause {
    LogicalJoinType    type{LogicalJoinType::INNER};
    BoundTable         table;
    LogicalJoinCondition condition;
};

struct BoundSelect {
    BoundTable                  from;
    std::vector<BoundJoinClause> joins;
    bool                        select_all{false};
    std::vector<std::string>    select_columns;
    std::optional<uint64_t>     limit;
    std::optional<std::string>  page_token;
};

} // namespace mldp_pvxs_driver::query::plan
