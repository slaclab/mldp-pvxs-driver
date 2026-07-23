//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/RequiredColumnCheck.h>

#include <query/plan/PlannerError.h>

#include <unordered_map>
#include <unordered_set>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

void splitQualified(const std::string& value, std::string& alias, std::string& column)
{
    const auto dot = value.find('.');
    if (dot == std::string::npos)
    {
        alias.clear();
        column = value;
        return;
    }
    alias = value.substr(0, dot);
    column = value.substr(dot + 1);
}

struct RequiredState {
    std::vector<const plan::LogicalScan*> scans;
    std::unordered_map<std::string, std::unordered_set<std::string>> join_columns;
};

void collectState(const plan::LogicalNodePtr& node, RequiredState& state)
{
    if (!node)
    {
        return;
    }
    if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        state.scans.push_back(scan);
        return;
    }
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        collectState(filter->input, state);
        return;
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        collectState(project->input, state);
        return;
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        collectState(limit->input, state);
        return;
    }
    if (const auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        std::string left_alias;
        std::string left_column;
        splitQualified(join->condition.left_column, left_alias, left_column);
        if (!left_alias.empty())
        {
            state.join_columns[left_alias].insert(left_column);
        }

        std::string right_alias;
        std::string right_column;
        splitQualified(join->condition.right_column, right_alias, right_column);
        if (!right_alias.empty())
        {
            state.join_columns[right_alias].insert(right_column);
        }
        collectState(join->left, state);
        collectState(join->right, state);
    }
}

} // namespace

void mldp_pvxs_driver::query::planner::requiredColumnCheck(const plan::LogicalNodePtr& root)
{
    RequiredState state;
    collectState(root, state);
    for (const auto* scan : state.scans)
    {
        for (const auto& column : scan->schema)
        {
            if (!column.required)
            {
                continue;
            }

            bool has_pushable_predicate = false;
            for (const auto& predicate : scan->pushable_predicates)
            {
                if (predicate.column == column.name)
                {
                    has_pushable_predicate = true;
                    break;
                }
            }
            if (!has_pushable_predicate)
            {
                const auto join_columns = state.join_columns.find(scan->table_alias);
                const bool covered_by_join = join_columns != state.join_columns.end() &&
                    join_columns->second.contains(column.name);
                if (!covered_by_join)
                {
                    throw plan::PlannerException(plan::PlanError{
                        .message = "Required column '" + scan->table_alias + "." + column.name +
                            "' must have a pushable predicate or be part of an equi-join"});
                }
            }
        }
    }
}
