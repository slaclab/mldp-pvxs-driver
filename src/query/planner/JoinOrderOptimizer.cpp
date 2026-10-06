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

#include <query/plan/PlanVisit.h>

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
    return std::visit(plan::overloaded{
                          [](const plan::LogicalScan& scan) { return isBoundedScan(scan); },
                          [](const plan::LogicalFilter& filter) { return !filter.predicates.empty() || isBounded(filter.input); },
                          [](const plan::LogicalProject& project) { return isBounded(project.input); },
                          [](const plan::LogicalSort& sort) { return isBounded(sort.input); },
                          [](const plan::LogicalAggregate& aggregate) { return isBounded(aggregate.input); },
                          [](const plan::LogicalLimit&) { return true; },
                          [](const plan::LogicalJoin& join) { return isBounded(join.left) || isBounded(join.right); },
                      },
                      node->value);
}

int64_t boundedScore(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return 1000;
    }
    return std::visit(plan::overloaded{
                          [](const plan::LogicalScan& scan) { return static_cast<int64_t>(scan.pushable_predicates.size()); },
                          [](const plan::LogicalFilter& filter) { return static_cast<int64_t>(filter.predicates.size()) + boundedScore(filter.input); },
                          [](const plan::LogicalProject& project) { return boundedScore(project.input); },
                          [](const plan::LogicalSort& sort) { return boundedScore(sort.input); },
                          [](const plan::LogicalAggregate& aggregate) { return boundedScore(aggregate.input); },
                          [](const plan::LogicalLimit& limit) { return int64_t{100000} + static_cast<int64_t>(limit.limit); },
                          [](const plan::LogicalJoin& join) { return boundedScore(join.left) + boundedScore(join.right); },
                      },
                      node->value);
}

plan::LogicalNodePtr rewrite(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return node;
    }
    plan::forEachChild(*node, [](plan::LogicalNodePtr& child) { child = rewrite(child); });
    if (auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
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
