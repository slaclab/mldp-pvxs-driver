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
    if (auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        markJoinOutputQualification(filter->input, under_join);
        return;
    }
    if (auto* project = std::get_if<plan::PhysicalProject>(&node->value))
    {
        markJoinOutputQualification(project->input, under_join);
        return;
    }
    if (auto* limit = std::get_if<plan::PhysicalLimit>(&node->value))
    {
        markJoinOutputQualification(limit->input, under_join);
        return;
    }
    if (auto* join = std::get_if<plan::PhysicalHashJoin>(&node->value))
    {
        markJoinOutputQualification(join->left, true);
        markJoinOutputQualification(join->right, true);
        return;
    }
    if (auto* join = std::get_if<plan::PhysicalNestedLoopJoin>(&node->value))
    {
        markJoinOutputQualification(join->outer, true);
        markJoinOutputQualification(join->inner, true);
        return;
    }
    if (auto* join = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&node->value))
    {
        markJoinOutputQualification(join->outer, true);
        markJoinOutputQualification(join->inner, true);
    }
}

plan::PhysicalNodePtr rewrite(const plan::PhysicalNodePtr& node)
{
    if (!node)
    {
        return node;
    }
    if (auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        filter->input = rewrite(filter->input);
        return node;
    }
    if (auto* project = std::get_if<plan::PhysicalProject>(&node->value))
    {
        project->input = rewrite(project->input);
        return node;
    }
    if (auto* limit = std::get_if<plan::PhysicalLimit>(&node->value))
    {
        limit->input = rewrite(limit->input);
        return node;
    }
    if (auto* hash_join = std::get_if<plan::PhysicalHashJoin>(&node->value))
    {
        hash_join->left = rewrite(hash_join->left);
        hash_join->right = rewrite(hash_join->right);
        if (!std::holds_alternative<plan::PhysicalTableScan>(hash_join->right->value))
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
    if (auto* nested_join = std::get_if<plan::PhysicalNestedLoopJoin>(&node->value))
    {
        nested_join->outer = rewrite(nested_join->outer);
        nested_join->inner = rewrite(nested_join->inner);
        return node;
    }
    if (auto* block_join = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&node->value))
    {
        block_join->outer = rewrite(block_join->outer);
        block_join->inner = rewrite(block_join->inner);
    }
    return node;
}

} // namespace

plan::PhysicalNodePtr mldp_pvxs_driver::query::planner::applyCorrelatedPushOptimizer(plan::PhysicalNodePtr root)
{
    auto optimized = rewrite(std::move(root));
    markJoinOutputQualification(optimized, false);
    return optimized;
}
