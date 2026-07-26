//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ExecutionState.h>

#include <query/executor/StateInternal.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeExecutionState(const plan::PhysicalNodePtr& root,
                                                    const ExecutionContext&      context,
                                                    QueryStats&                  stats)
{
    if (!root)
    {
        throw std::runtime_error("Cannot initialize execution state from a null physical plan node");
    }
    if (const auto* scan = std::get_if<plan::PhysicalTableScan>(&root->value))
    {
        return makeScanExecutionState(*scan, root, context, stats);
    }
    if (auto state = makeRelationalExecutionState(root->value, root, context, stats))
    {
        return state;
    }
    if (auto state = makeStatementExecutionState(root->value, root, context, stats))
    {
        return state;
    }
    throw std::runtime_error("Unsupported physical plan node while initializing execution state");
}
