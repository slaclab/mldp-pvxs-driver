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

#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

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

struct ResolvedColumn {
    std::string table_alias;
    std::string column_name;
};

std::string qualify(const std::string& alias, const std::string& name)
{
    return alias + "." + name;
}

std::string resolveColumnOnTable(const QualifiedColumn& column,
                                 const plan::BoundTable& table)
{
    if (column.qualifier.has_value() && column.qualifier.value() != table.table_alias && column.qualifier.value() != table.table_name)
    {
        return "";
    }

    if (findColumnSchema(table.schema, column.name) != nullptr)
    {
        return column.name;
    }
    if (column.name.rfind("attr.", 0) == 0)
    {
        return column.name;
    }
    return "";
}

std::string listAliases(const std::vector<plan::BoundTable>& tables)
{
    std::ostringstream out;
    for (size_t index = 0; index < tables.size(); ++index)
    {
        if (index != 0)
        {
            out << ", ";
        }
        out << "'" << tables[index].table_alias << "'";
    }
    return out.str();
}

ResolvedColumn resolveColumnReference(const QualifiedColumn& column,
                                      const std::vector<plan::BoundTable>& tables)
{
    std::vector<ResolvedColumn> matches;
    for (const auto& table : tables)
    {
        const auto resolved = resolveColumnOnTable(column, table);
        if (!resolved.empty())
        {
            matches.push_back(ResolvedColumn{.table_alias = table.table_alias, .column_name = resolved});
        }
    }

    if (matches.empty())
    {
        if (column.qualifier.has_value())
        {
            throw plan::PlannerException(plan::BindError{
                .message = "Unknown column '" + column.qualifier.value() + "." + column.name + "'"});
        }
        throw plan::PlannerException(plan::BindError{
            .message = "Unknown column '" + column.name + "'"});
    }

    if (matches.size() > 1)
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Ambiguous column '" + column.name + "'. Qualify it with one of: " + listAliases(tables)});
    }

    return matches.front();
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
                                      const std::vector<plan::BoundTable>& tables)
{
    return std::visit(
        [&](const auto& predicate) -> plan::PlannerPredicate
        {
            const auto resolved = resolveColumnReference(predicate.column, tables);
            const auto* bound_table = [&]() -> const plan::BoundTable* {
                for (const auto& table : tables)
                {
                    if (table.table_alias == resolved.table_alias)
                    {
                        return &table;
                    }
                }
                return nullptr;
            }();
            if (bound_table == nullptr)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Unknown table alias '" + resolved.table_alias + "' for predicate binding"});
            }

            const auto* schema_column = findColumnSchema(bound_table->schema, resolved.column_name);
            const bool is_attr_predicate = schema_column == nullptr && resolved.column_name.rfind("attr.", 0) == 0;
            if (schema_column == nullptr && !is_attr_predicate)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Unknown column '" + qualify(resolved.table_alias, resolved.column_name) + "'"});
            }

            plan::PlannerPredicate bound{};
            bound.column = resolved.column_name;
            bound.table_alias = resolved.table_alias;
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
                    .message = "Operator not supported for column '" + qualify(bound.table_alias, bound.column) + "'"});
            }
            return bound;
        },
        where);
}

plan::BoundTable makeBoundTable(const TableRef& table_ref)
{
    const auto registered_tables = QueryableFactory::instance().registeredTables();
    if (!registered_tables.contains(table_ref.table_name))
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Unknown table '" + table_ref.table_name + "'"});
    }

    IQueryableUPtr queryable;
    try
    {
        queryable = QueryableFactory::instance().createByTable(table_ref.table_name);
    }
    catch (const std::exception& ex)
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Failed to initialize query client for table '" + table_ref.table_name + "': " + ex.what()});
    }
    const auto alias = table_ref.alias.value_or(table_ref.table_name);
    return plan::BoundTable{
        .table_name = table_ref.table_name,
        .table_alias = alias,
        .schema = queryable->tableSchema(table_ref.table_name),
        .predicates = {}};
}

void requireUniqueAlias(const plan::BoundTable& table,
                        const std::unordered_set<std::string>& aliases)
{
    if (aliases.contains(table.table_alias))
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Duplicate table alias '" + table.table_alias + "'"});
    }
}

void ensureColumnCoveredForRequiredCheck(const plan::BoundTable& table,
                                         const std::unordered_set<std::string>& covered_columns)
{
    for (const auto& column : table.schema)
    {
        if (!column.required)
        {
            continue;
        }
        if (!covered_columns.contains(column.name))
        {
            throw plan::PlannerException(plan::BindError{
                .message = "Missing required predicate for column '" + qualify(table.table_alias, column.name) + "'"});
        }
    }
}

} // namespace

plan::BoundSelect mldp_pvxs_driver::query::planner::bindSelect(const SelectStatement& statement)
{
    auto from = makeBoundTable(statement.from);
    std::vector<plan::BoundJoinClause> joins;
    std::vector<std::pair<ResolvedColumn, ResolvedColumn>> join_required_columns;
    joins.reserve(statement.joins.size());
    join_required_columns.reserve(statement.joins.size());

    std::unordered_set<std::string> aliases;
    requireUniqueAlias(from, aliases);
    aliases.insert(from.table_alias);

    std::vector<plan::BoundTable> all_tables;
    all_tables.push_back(from);
    for (const auto& join_clause : statement.joins)
    {
        auto right_table = makeBoundTable(join_clause.table);
        requireUniqueAlias(right_table, aliases);

        std::vector<plan::BoundTable> join_scope = all_tables;
        join_scope.push_back(right_table);
        const auto left_ref = resolveColumnReference(join_clause.condition.left, join_scope);
        const auto right_ref = resolveColumnReference(join_clause.condition.right, join_scope);

        const bool left_is_existing = aliases.contains(left_ref.table_alias);
        const bool right_is_existing = aliases.contains(right_ref.table_alias);
        const bool left_is_right = left_ref.table_alias == right_table.table_alias;
        const bool right_is_right = right_ref.table_alias == right_table.table_alias;

        plan::LogicalJoinCondition condition;
        if (left_is_existing && right_is_right)
        {
            condition.left_column = qualify(left_ref.table_alias, left_ref.column_name);
            condition.right_column = qualify(right_ref.table_alias, right_ref.column_name);
            join_required_columns.emplace_back(left_ref, right_ref);
        }
        else if (right_is_existing && left_is_right)
        {
            condition.left_column = qualify(right_ref.table_alias, right_ref.column_name);
            condition.right_column = qualify(left_ref.table_alias, left_ref.column_name);
            join_required_columns.emplace_back(right_ref, left_ref);
        }
        else
        {
            throw plan::PlannerException(plan::BindError{
                .message = "JOIN ON must compare one column from already-joined tables to one column from table alias '" +
                    right_table.table_alias + "'"});
        }

        aliases.insert(right_table.table_alias);
        all_tables.push_back(right_table);
        joins.push_back(plan::BoundJoinClause{
            .type = join_clause.type == query::JoinType::LEFT_OUTER
                ? plan::LogicalJoinType::LEFT_OUTER
                : plan::LogicalJoinType::INNER,
            .table = std::move(right_table),
            .condition = std::move(condition)});
    }

    std::unordered_map<std::string, size_t> table_index;
    table_index.reserve(all_tables.size());
    for (size_t index = 0; index < all_tables.size(); ++index)
    {
        table_index[all_tables[index].table_alias] = index;
    }

    for (const auto& where : statement.predicates)
    {
        const auto predicate = buildPredicate(where, all_tables);
        all_tables[table_index.at(predicate.table_alias)].predicates.push_back(predicate);
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> covered_columns;
    for (const auto& table : all_tables)
    {
        covered_columns[table.table_alias] = {};
        for (const auto& predicate : table.predicates)
        {
            covered_columns[table.table_alias].insert(predicate.column);
        }
    }
    for (const auto& [left, right] : join_required_columns)
    {
        covered_columns[left.table_alias].insert(left.column_name);
        covered_columns[right.table_alias].insert(right.column_name);
    }
    for (const auto& table : all_tables)
    {
        ensureColumnCoveredForRequiredCheck(table, covered_columns[table.table_alias]);
    }

    from = all_tables.front();
    for (size_t index = 0; index < joins.size(); ++index)
    {
        joins[index].table = all_tables[index + 1];
    }

    plan::BoundSelect bound{
        .from = std::move(from),
        .joins = std::move(joins),
        .select_all = statement.select_all,
        .select_columns = {},
        .limit = statement.limit,
        .page_token = statement.page_token};

    if (!statement.select_all)
    {
        const bool multi_table = !bound.joins.empty();
        bound.select_columns.reserve(statement.columns.size());
        for (const auto& column : statement.columns)
        {
            const auto resolved = resolveColumnReference(column, all_tables);
            if (multi_table)
            {
                bound.select_columns.push_back(qualify(resolved.table_alias, resolved.column_name));
            }
            else
            {
                bound.select_columns.push_back(resolved.column_name);
            }
        }
    }

    return bound;
}
