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

#include <map>
#include <set>

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

void collectReferencedColumns(const plan::LogicalNodePtr&                   node,
                              std::map<std::string, std::set<std::string>>& columns,
                              const std::set<std::string>&                  table_aliases)
{
    if (!node)
    {
        return;
    }

    if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        for (const auto& predicate : scan->pushable_predicates)
        {
            columns[scan->table_alias].insert(predicate.column == "tag" ? "tags" : predicate.column);
        }
        return;
    }
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        for (const auto& predicate : filter->predicates)
        {
            const auto alias = predicate.table_alias.empty() ? "" : predicate.table_alias;
            columns[alias].insert(predicate.column == "tag" ? "tags" : predicate.column);
        }
        collectReferencedColumns(filter->input, columns, table_aliases);
        return;
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        for (const auto& column : project->columns)
        {
            const auto [alias, name] = splitQualifiedColumn(column, table_aliases);
            columns[alias].insert(name);
        }
        collectReferencedColumns(project->input, columns, table_aliases);
        return;
    }
    if (const auto* sort = std::get_if<plan::LogicalSort>(&node->value))
    {
        for (const auto& key : sort->keys)
        {
            const auto [alias, name] = splitQualifiedColumn(key.column, table_aliases);
            columns[alias].insert(name);
        }
        collectReferencedColumns(sort->input, columns, table_aliases);
        return;
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        collectReferencedColumns(limit->input, columns, table_aliases);
        return;
    }
    if (const auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        const auto [left_alias, left_name] = splitQualifiedColumn(join->condition.left_column, table_aliases);
        const auto [right_alias, right_name] = splitQualifiedColumn(join->condition.right_column, table_aliases);
        columns[left_alias].insert(left_name);
        columns[right_alias].insert(right_name);
        collectReferencedColumns(join->left, columns, table_aliases);
        collectReferencedColumns(join->right, columns, table_aliases);
    }
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
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        collectTableAliases(filter->input, table_aliases);
        return;
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        collectTableAliases(project->input, table_aliases);
        return;
    }
    if (const auto* sort = std::get_if<plan::LogicalSort>(&node->value))
    {
        collectTableAliases(sort->input, table_aliases);
        return;
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        collectTableAliases(limit->input, table_aliases);
        return;
    }
    if (const auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        collectTableAliases(join->left, table_aliases);
        collectTableAliases(join->right, table_aliases);
    }
}

void applyProjectionHint(const plan::LogicalNodePtr&                         node,
                         const std::map<std::string, std::set<std::string>>& columns)
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
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        applyProjectionHint(filter->input, columns);
        return;
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        applyProjectionHint(project->input, columns);
        return;
    }
    if (const auto* sort = std::get_if<plan::LogicalSort>(&node->value))
    {
        applyProjectionHint(sort->input, columns);
        return;
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        applyProjectionHint(limit->input, columns);
        return;
    }
    if (const auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        applyProjectionHint(join->left, columns);
        applyProjectionHint(join->right, columns);
    }
}

} // namespace

plan::LogicalNodePtr mldp_pvxs_driver::query::planner::applyColumnPruning(plan::LogicalNodePtr root)
{
    std::map<std::string, std::set<std::string>> columns;
    std::set<std::string>                        table_aliases;
    collectTableAliases(root, table_aliases);
    collectReferencedColumns(root, columns, table_aliases);
    applyProjectionHint(root, columns);
    return root;
}
