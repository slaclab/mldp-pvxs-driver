//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/ColumnPruning.h>

#include <query/plan/PlanVisit.h>

#include <map>
#include <set>
#include <type_traits>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

std::pair<std::string, std::string> splitQualifiedColumn(const std::string&           value,
                                                         const std::set<std::string>& table_aliases)
{
    const auto dot = value.find('.');
    if (dot == std::string::npos || !table_aliases.contains(value.substr(0, dot)))
    {
        return {"", value};
    }
    return {value.substr(0, dot), value.substr(dot + 1)};
}

void collectExpressionColumns(const ExpressionPtr&                              expression,
                              std::map<std::string, std::set<std::string>>& columns,
                              const std::set<std::string>&                     table_aliases)
{
    if (!expression)
    {
        return;
    }

    std::visit(
        [&](const auto& value)
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, QualifiedColumn>)
            {
                const auto name = value.qualifier ? *value.qualifier + "." + value.name : value.name;
                const auto [alias, local] = splitQualifiedColumn(name, table_aliases);
                columns[alias].insert(local);
            }
            else if constexpr (std::is_same_v<T, FunctionCall>)
            {
                for (const auto& argument : value.arguments)
                {
                    collectExpressionColumns(argument, columns, table_aliases);
                }
            }
            else if constexpr (std::is_same_v<T, UnaryExpression>)
            {
                collectExpressionColumns(value.operand, columns, table_aliases);
            }
            else if constexpr (std::is_same_v<T, BinaryExpression>)
            {
                collectExpressionColumns(value.left, columns, table_aliases);
                collectExpressionColumns(value.right, columns, table_aliases);
            }
        },
        expression->value);
}

void collectReferencedColumns(const plan::LogicalNodePtr&                   node,
                              std::map<std::string, std::set<std::string>>& columns,
                              const std::set<std::string>&                  table_aliases)
{
    if (!node)
    {
        return;
    }
    const auto insert = [&columns, &table_aliases](const std::string& qualified)
    {
        const auto [alias, name] = splitQualifiedColumn(qualified, table_aliases);
        columns[alias].insert(name);
    };
    // Every node type is listed: a new one must declare the columns it reads.
    std::visit(plan::overloaded{
                   [&](const plan::LogicalScan& scan)
                   {
                       for (const auto& predicate : scan.pushable_predicates)
                       {
                           columns[scan.table_alias].insert(predicate.column == "tag" ? "tags" : predicate.column);
                       }
                       for (const auto& subquery : scan.in_subqueries)
                       {
                           // A local-only IN subquery needs the target field present in the
                           // backend result so the executor can apply the resolved predicate.
                           if (!subquery.predicate.pushable_ops.contains(PredicateOp::IN))
                           {
                               columns[scan.table_alias].insert(subquery.predicate.column == "tag" ? "tags" : subquery.predicate.column);
                           }
                       }
                   },
                   [&](const plan::LogicalFilter& filter)
                   {
                       for (const auto& predicate : filter.predicates)
                       {
                           columns[predicate.table_alias].insert(predicate.column == "tag" ? "tags" : predicate.column);
                       }
                   },
                   [&](const plan::LogicalAggregate& aggregate)
                   {
                       // Nodes above an aggregate reference its internal output columns; the
                       // input only needs the key and aggregate argument columns.
                       columns.clear();
                       for (const auto& key : aggregate.spec.keys)
                       {
                           collectExpressionColumns(key.expression, columns, table_aliases);
                       }
                       for (const auto& call : aggregate.spec.aggregates)
                       {
                           collectExpressionColumns(call.argument, columns, table_aliases);
                       }
                   },
                   [&](const plan::LogicalProject& project)
                   {
                       for (const auto& column : project.columns)
                       {
                           insert(column);
                       }
                       for (const auto& expression : project.expressions)
                       {
                           collectExpressionColumns(expression, columns, table_aliases);
                       }
                       for (const auto& expression : project.distinct_on)
                       {
                           collectExpressionColumns(expression, columns, table_aliases);
                       }
                   },
                   [&](const plan::LogicalSort& sort)
                   {
                       for (const auto& key : sort.keys)
                       {
                           insert(key.column);
                       }
                   },
                   [](const plan::LogicalLimit&) {},
                   [&](const plan::LogicalJoin& join)
                   {
                       insert(join.condition.left_column);
                       insert(join.condition.right_column);
                   },
               },
               node->value);
    plan::forEachChild(*node, [&columns, &table_aliases](plan::LogicalNodePtr& child) { collectReferencedColumns(child, columns, table_aliases); });
}

void collectTableAliases(const plan::LogicalNodePtr& node, std::set<std::string>& table_aliases)
{
    if (!node)
    {
        return;
    }
    if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        table_aliases.insert(scan->table_alias);
        return;
    }
    plan::forEachChild(*node, [&table_aliases](plan::LogicalNodePtr& child) { collectTableAliases(child, table_aliases); });
}

void applyProjectionHint(const plan::LogicalNodePtr&                         node,
                         const std::map<std::string, std::set<std::string>>& columns,
                         const bool                                          select_all)
{
    if (!node)
    {
        return;
    }

    if (auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        scan->projection_hint.clear();
        const auto alias_match = columns.find(scan->table_alias);
        if (alias_match != columns.end())
        {
            scan->projection_hint.insert(alias_match->second.begin(), alias_match->second.end());
        }
        if (const auto default_match = columns.find(""); default_match != columns.end())
        {
            scan->projection_hint.insert(default_match->second.begin(), default_match->second.end());
        }
        scan->projection_explicit = !select_all && !scan->projection_hint.empty();
        if (scan->projection_hint.empty())
        {
            for (const auto& column : scan->schema)
            {
                if (column.is_output)
                {
                    scan->projection_hint.insert(column.name);
                }
            }
        }
        return;
    }
    // A projection sets whether its scans are explicit; below an aggregate only
    // the referenced key and argument columns are needed.
    const bool child_select_all = std::visit(plan::overloaded{
                                                 [](const plan::LogicalProject& project) { return project.select_all; },
                                                 [](const plan::LogicalAggregate&) { return false; },
                                                 [select_all](const auto&) { return select_all; },
                                             },
                                             node->value);
    plan::forEachChild(*node, [&columns, child_select_all](plan::LogicalNodePtr& child) { applyProjectionHint(child, columns, child_select_all); });
}

} // namespace

plan::LogicalNodePtr mldp_pvxs_driver::query::planner::applyColumnPruning(plan::LogicalNodePtr root)
{
    std::map<std::string, std::set<std::string>> columns;
    std::set<std::string>                        table_aliases;
    collectTableAliases(root, table_aliases);
    collectReferencedColumns(root, columns, table_aliases);
    applyProjectionHint(root, columns, true);
    return root;
}
