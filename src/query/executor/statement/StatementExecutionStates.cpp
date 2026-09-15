//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/executor/StateInternal.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeStatementExecutionState(const plan::PhysicalNodeVariant& value, const plan::PhysicalNodePtr& physical, const ExecutionContext& context, QueryStats& stats)
{
    if (const auto* node = std::get_if<plan::PhysicalShowTables>(&value)) return makeShowTablesExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalShowFunctions>(&value)) return makeShowFunctionsExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalShowOperators>(&value)) return makeShowOperatorsExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalDescribe>(&value)) return makeDescribeExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalExplain>(&value)) return makeExplainExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalCreateTable>(&value)) return makeCreateTableExecutionState(*node, physical, context, stats);
    if (const auto* node = std::get_if<plan::PhysicalDropTable>(&value)) return makeDropTableExecutionState(*node, physical, context, stats);
    return nullptr;
}
