//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/StateInternal.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeRelationalExecutionState(const plan::PhysicalNodeVariant& value, const plan::PhysicalNodePtr& physical, const ExecutionContext& context, QueryStats& stats)
{
    if (const auto* node = std::get_if<plan::PhysicalFilter>(&value)) return makeFilterExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalProject>(&value)) return makeProjectExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalSort>(&value)) return makeSortExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalLimit>(&value)) return makeLimitExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalHashJoin>(&value)) return makeHashJoinExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalNestedLoopJoin>(&value)) return makeNestedLoopJoinExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&value)) return makeBlockNestedLoopJoinExecutionState(*node, physical, context, stats);
    return nullptr;
}
