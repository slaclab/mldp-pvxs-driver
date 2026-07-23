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

#include <set>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

void collectReferencedColumns(const plan::LogicalNodePtr& node, std::set<std::string>& columns)
{
    if (!node)
    {
        return;
    }

    if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        for (const auto& predicate : scan->pushable_predicates)
        {
            columns.insert(predicate.column);
        }
        return;
    }
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        for (const auto& predicate : filter->predicates)
        {
            columns.insert(predicate.column);
        }
        collectReferencedColumns(filter->input, columns);
        return;
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        for (const auto& column : project->columns)
        {
            columns.insert(column);
        }
        collectReferencedColumns(project->input, columns);
        return;
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        collectReferencedColumns(limit->input, columns);
        return;
    }
}

void applyProjectionHint(const plan::LogicalNodePtr& node, const std::set<std::string>& columns)
{
    if (!node)
    {
        return;
    }

    if (auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        scan->projection_hint = columns;
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
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        applyProjectionHint(limit->input, columns);
        return;
    }
}

} // namespace

plan::LogicalNodePtr mldp_pvxs_driver::query::planner::applyColumnPruning(plan::LogicalNodePtr root)
{
    std::set<std::string> columns;
    collectReferencedColumns(root, columns);
    applyProjectionHint(root, columns);
    return root;
}
