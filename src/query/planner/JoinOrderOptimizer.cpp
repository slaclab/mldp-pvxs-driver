//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/JoinOrderOptimizer.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

bool isBounded(const plan::LogicalNodePtr& node);
int64_t boundedScore(const plan::LogicalNodePtr& node);

bool isBoundedScan(const plan::LogicalScan& scan)
{
    if (scan.pushable_predicates.empty())
    {
        return false;
    }
    for (const auto& predicate : scan.pushable_predicates)
    {
        if (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN || predicate.op == PredicateOp::BETWEEN)
        {
            return true;
        }
    }
    for (const auto& column : scan.schema)
    {
        if (!column.required)
        {
            continue;
        }
        for (const auto& predicate : scan.pushable_predicates)
        {
            if (predicate.column == column.name)
            {
                return true;
            }
        }
    }
    return false;
}

bool isBounded(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return false;
    }
    if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        return isBoundedScan(*scan);
    }
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        if (!filter->predicates.empty())
        {
            return true;
        }
        return isBounded(filter->input);
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        return isBounded(project->input);
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        return true;
    }
    if (const auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        return isBounded(join->left) || isBounded(join->right);
    }
    return false;
}

int64_t boundedScore(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return 1000;
    }
    if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        return static_cast<int64_t>(scan->pushable_predicates.size());
    }
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        return static_cast<int64_t>(filter->predicates.size()) + boundedScore(filter->input);
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        return boundedScore(project->input);
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        return 100000 + static_cast<int64_t>(limit->limit);
    }
    if (const auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        return boundedScore(join->left) + boundedScore(join->right);
    }
    return 0;
}

plan::LogicalNodePtr rewrite(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return node;
    }
    if (auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        filter->input = rewrite(filter->input);
        return node;
    }
    if (auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        project->input = rewrite(project->input);
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
        join->left_bounded = isBounded(join->left);
        join->right_bounded = isBounded(join->right);

        if (!join->left_bounded && join->right_bounded && join->type == plan::LogicalJoinType::INNER)
        {
            std::swap(join->left, join->right);
            std::swap(join->condition.left_column, join->condition.right_column);
            std::swap(join->left_bounded, join->right_bounded);
        }
        else if (join->left_bounded && join->right_bounded && join->type == plan::LogicalJoinType::INNER)
        {
            if (boundedScore(join->left) > boundedScore(join->right))
            {
                std::swap(join->left, join->right);
                std::swap(join->condition.left_column, join->condition.right_column);
            }
        }
        else if (!join->left_bounded && !join->right_bounded)
        {
            join->warnings.push_back("PlanWarning: joining two unbounded sides; spill is expected under memory pressure");
        }
    }
    return node;
}

} // namespace

plan::LogicalNodePtr mldp_pvxs_driver::query::planner::applyJoinOrderOptimizer(plan::LogicalNodePtr root)
{
    return rewrite(std::move(root));
}
