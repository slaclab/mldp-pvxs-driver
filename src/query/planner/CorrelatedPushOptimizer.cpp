//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/CorrelatedPushOptimizer.h>

#include <query/plan/PlanVisit.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

void markJoinOutputQualification(const plan::PhysicalNodePtr& node, const bool under_join)
{
    if (!node)
    {
        return;
    }
    if (auto* scan = std::get_if<plan::PhysicalTableScan>(&node->value))
    {
        scan->qualify_output = under_join;
        return;
    }
    // CREATE TABLE sources are planned on their own.
    if (std::holds_alternative<plan::PhysicalCreateTable>(node->value))
    {
        return;
    }
    const bool child_under_join = under_join || plan::isJoin(*node);
    plan::forEachChild(*node, [child_under_join](plan::PhysicalNodePtr& child) { markJoinOutputQualification(child, child_under_join); });
}

plan::PhysicalNodePtr rewrite(const plan::PhysicalNodePtr& node)
{
    if (!node || std::holds_alternative<plan::PhysicalCreateTable>(node->value))
    {
        return node;
    }
    plan::forEachChild(*node, [](plan::PhysicalNodePtr& child) { child = rewrite(child); });
    // A hash join whose probe side is a plain scan becomes a correlated nested-loop push.
    const auto* hash_join = std::get_if<plan::PhysicalHashJoin>(&node->value);
    if (hash_join == nullptr || !std::holds_alternative<plan::PhysicalTableScan>(hash_join->right->value))
    {
        return node;
    }
    return plan::makeNode(plan::PhysicalNestedLoopJoin{
        .type = hash_join->type,
        .condition = hash_join->condition,
        .algorithm = plan::JoinAlgorithm::NESTED_LOOP,
        .outer = hash_join->left,
        .inner = hash_join->right,
        .correlated_push = true});
}

} // namespace

plan::PhysicalNodePtr mldp_pvxs_driver::query::planner::applyCorrelatedPushOptimizer(plan::PhysicalNodePtr root)
{
    auto optimized = rewrite(std::move(root));
    markJoinOutputQualification(optimized, false);
    return optimized;
}
