//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/PredicatePushdown.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

plan::LogicalNodePtr rewrite(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return node;
    }

    if (auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        limit->input = rewrite(limit->input);
        return node;
    }
    if (auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        join->left = rewrite(join->left);
        join->right = rewrite(join->right);
        return node;
    }
    if (auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        project->input = rewrite(project->input);
        return node;
    }
    if (auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        filter->input = rewrite(filter->input);
        auto* scan = std::get_if<plan::LogicalScan>(&filter->input->value);
        if (!scan)
        {
            return node;
        }

        std::vector<plan::PlannerPredicate> post_filter;
        post_filter.reserve(filter->predicates.size());
        for (const auto& predicate : filter->predicates)
        {
            if (predicate.pushable_ops.contains(predicate.op))
            {
                scan->pushable_predicates.push_back(predicate);
                // Metadata criteria are an optimization only.  Retain them as
                // a local filter so deployments that ignore a supported
                // criterion still observe SQL semantics.
                if (scan->table_name != "mldp.time_series_table" &&
                    (predicate.column == "tag" || predicate.column.rfind("attributes.", 0) == 0 ||
                     predicate.column.rfind("provenance.", 0) == 0))
                {
                    post_filter.push_back(predicate);
                }
            }
            else
            {
                post_filter.push_back(predicate);
            }
        }

        filter->predicates = std::move(post_filter);
        if (filter->predicates.empty())
        {
            return filter->input;
        }
        return node;
    }

    return node;
}

} // namespace

plan::LogicalNodePtr mldp_pvxs_driver::query::planner::applyPredicatePushdown(plan::LogicalNodePtr root)
{
    return rewrite(root);
}
