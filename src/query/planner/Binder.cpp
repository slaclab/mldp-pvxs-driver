//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/Binder.h>

#include <query/QueryableFactory.h>
#include <query/plan/PlannerError.h>

#include <type_traits>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

const ColumnSchema* findColumnSchema(const std::vector<ColumnSchema>& schema, const std::string& name)
{
    for (const auto& column : schema)
    {
        if (column.name == name)
        {
            return &column;
        }
    }
    return nullptr;
}

std::string resolveColumnName(const QualifiedColumn& column,
                              const std::string& alias,
                              const std::string& table_name,
                              const std::vector<ColumnSchema>& schema)
{
    if (column.qualifier.has_value() &&
        column.qualifier.value() != alias &&
        column.qualifier.value() != table_name)
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Unknown table qualifier '" + column.qualifier.value() + "'"});
    }

    const auto resolved = column.name;
    if (findColumnSchema(schema, resolved) != nullptr)
    {
        return resolved;
    }
    if (resolved.rfind("attr.", 0) == 0)
    {
        return resolved;
    }
    throw plan::PlannerException(plan::BindError{
        .message = "Unknown column '" + resolved + "' in table '" + table_name + "'"});
}

PredicateOp mapBinaryOp(const PredicateBinaryOp op)
{
    switch (op)
    {
    case PredicateBinaryOp::NEQ:
        return PredicateOp::NEQ;
    case PredicateBinaryOp::LT:
        return PredicateOp::LT;
    case PredicateBinaryOp::LTE:
        return PredicateOp::LTE;
    case PredicateBinaryOp::GT:
        return PredicateOp::GT;
    case PredicateBinaryOp::GTE:
        return PredicateOp::GTE;
    case PredicateBinaryOp::LIKE:
        return PredicateOp::CONTAINS;
    case PredicateBinaryOp::CONTAINS:
        return PredicateOp::CONTAINS;
    case PredicateBinaryOp::PREFIX:
        return PredicateOp::PREFIX;
    }
    return PredicateOp::EQ;
}

std::set<PredicateOp> defaultTextOps()
{
    return {PredicateOp::EQ, PredicateOp::NEQ, PredicateOp::IN, PredicateOp::PREFIX, PredicateOp::CONTAINS};
}

plan::PlannerLiteralValue toPlannerLiteral(const LiteralValue& value)
{
    if (std::holds_alternative<std::string>(value))
    {
        return std::get<std::string>(value);
    }
    if (std::holds_alternative<int64_t>(value))
    {
        return std::get<int64_t>(value);
    }
    return std::get<NowLiteral>(value);
}

plan::PlannerPredicate buildPredicate(const WherePredicate& where,
                                      const std::string& alias,
                                      const std::string& table_name,
                                      const std::vector<ColumnSchema>& schema)
{
    return std::visit(
        [&](const auto& predicate) -> plan::PlannerPredicate
        {
            const auto column_name = resolveColumnName(predicate.column, alias, table_name, schema);
            const auto* schema_column = findColumnSchema(schema, column_name);
            const bool is_attr_predicate = schema_column == nullptr && column_name.rfind("attr.", 0) == 0;
            if (schema_column == nullptr && !is_attr_predicate)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Unknown column '" + column_name + "'"});
            }

            plan::PlannerPredicate bound{};
            bound.column = column_name;
            if (schema_column != nullptr)
            {
                bound.column_type = schema_column->type;
                bound.required_column = schema_column->required;
                bound.pushable_ops = schema_column->pushable_ops;
                bound.filterable_ops = schema_column->filterable_ops;
            }
            else
            {
                bound.column_type = ColumnType::STRING;
                bound.pushable_ops = defaultTextOps();
                bound.filterable_ops = defaultTextOps();
            }

            if constexpr (std::is_same_v<std::decay_t<decltype(predicate)>, EqPredicate>)
            {
                bound.op = PredicateOp::EQ;
                bound.values.push_back(toPlannerLiteral(predicate.value));
            }
            else if constexpr (std::is_same_v<std::decay_t<decltype(predicate)>, InPredicate>)
            {
                bound.op = PredicateOp::IN;
                for (const auto& value : predicate.values)
                {
                    bound.values.push_back(toPlannerLiteral(value));
                }
            }
            else if constexpr (std::is_same_v<std::decay_t<decltype(predicate)>, RangePredicate>)
            {
                bound.op = PredicateOp::BETWEEN;
                bound.values.push_back(toPlannerLiteral(predicate.lower));
                bound.values.push_back(toPlannerLiteral(predicate.upper));
            }
            else
            {
                bound.op = mapBinaryOp(predicate.op);
                bound.values.push_back(toPlannerLiteral(predicate.value));
            }

            const bool pushable = bound.pushable_ops.contains(bound.op);
            const bool filterable = bound.filterable_ops.contains(bound.op);
            if (!pushable && !filterable)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Operator not supported for column '" + bound.column + "'"});
            }
            return bound;
        },
        where);
}

} // namespace

plan::BoundSelect mldp_pvxs_driver::query::planner::bindSelect(const SelectStatement& statement)
{
    if (!statement.joins.empty())
    {
        throw plan::PlannerException(plan::PlanError{
            .message = "JOIN planning is deferred to Phase 3b"});
    }

    auto queryable = QueryableFactory::instance().createByTable(statement.from.table_name);
    const auto schema = queryable->tableSchema(statement.from.table_name);
    const auto alias = statement.from.alias.value_or(statement.from.table_name);

    plan::BoundSelect bound{
        .table_name = statement.from.table_name,
        .table_alias = alias,
        .schema = schema,
        .select_all = statement.select_all,
        .select_columns = {},
        .predicates = {},
        .limit = statement.limit,
        .page_token = statement.page_token};

    if (!statement.select_all)
    {
        bound.select_columns.reserve(statement.columns.size());
        for (const auto& column : statement.columns)
        {
            bound.select_columns.push_back(resolveColumnName(column, alias, statement.from.table_name, schema));
        }
    }

    bound.predicates.reserve(statement.predicates.size());
    for (const auto& predicate : statement.predicates)
    {
        bound.predicates.push_back(buildPredicate(predicate, alias, statement.from.table_name, schema));
    }

    for (const auto& column : schema)
    {
        if (!column.required)
        {
            continue;
        }

        bool found = false;
        for (const auto& predicate : bound.predicates)
        {
            if (predicate.column == column.name)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            throw plan::PlannerException(plan::BindError{
                .message = "Missing required predicate for column '" + column.name + "'"});
        }
    }

    return bound;
}
