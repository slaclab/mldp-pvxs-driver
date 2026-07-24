//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#include <query/QueryExecutor.h>


#include <query/executor/ExecutionState.h>

#include <arrow/memory_pool.h>

#include <chrono>

using namespace mldp_pvxs_driver::query;

namespace {

void collectPlanWarnings(const plan::PhysicalNodePtr& node, std::vector<std::string>& warnings)
{
    if (!node) return;
    if (const auto* hash = std::get_if<plan::PhysicalHashJoin>(&node->value)) { warnings.insert(warnings.end(), hash->warnings.begin(), hash->warnings.end()); collectPlanWarnings(hash->left, warnings); collectPlanWarnings(hash->right, warnings); return; }
    if (const auto* nested = std::get_if<plan::PhysicalNestedLoopJoin>(&node->value)) { collectPlanWarnings(nested->outer, warnings); collectPlanWarnings(nested->inner, warnings); return; }
    if (const auto* block = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&node->value)) { warnings.insert(warnings.end(), block->warnings.begin(), block->warnings.end()); collectPlanWarnings(block->outer, warnings); collectPlanWarnings(block->inner, warnings); return; }
    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&node->value)) { collectPlanWarnings(filter->input, warnings); return; }
    if (const auto* project = std::get_if<plan::PhysicalProject>(&node->value)) { collectPlanWarnings(project->input, warnings); return; }
    if (const auto* limit = std::get_if<plan::PhysicalLimit>(&node->value)) collectPlanWarnings(limit->input, warnings);
}

} // namespace

QueryExecutionResult QueryExecutor::execute(const plan::PhysicalNodePtr& root, const ExecutionContext& context) const
{
    QueryExecutionResult result;
    const auto start = std::chrono::steady_clock::now();
    auto execution_state = executor::makeExecutionState(root, context, result.stats);
    result.batches = execution_state->execute();
    collectPlanWarnings(root, result.stats.plan_warnings);
    result.stats.plan_summary = plan::physicalPlanToString(root);
    if (context.pool != nullptr) result.stats.peak_memory_bytes = static_cast<uint64_t>(context.pool->max_memory());
    for (const auto& batch : result.batches) result.stats.rows_returned += static_cast<uint64_t>(batch->num_rows());
    result.stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return result;
}
