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

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

const plan::LogicalScan* findScan(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return nullptr;
    }
    if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        return scan;
    }
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        return findScan(filter->input);
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        return findScan(project->input);
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        return findScan(limit->input);
    }
    return nullptr;
}

} // namespace

void mldp_pvxs_driver::query::planner::requiredColumnCheck(const plan::LogicalNodePtr& root)
{
    const auto* scan = findScan(root);
    if (!scan)
    {
        return;
    }

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
            throw plan::PlannerException(plan::PlanError{
                .message = "Required column '" + column.name + "' must have a pushable predicate"});
        }
    }
}
