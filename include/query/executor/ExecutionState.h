//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file ExecutionState.h
 * @brief Declares runtime state nodes for materialized physical-plan execution. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

#include <arrow/record_batch.h>

#include <memory>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query::executor {

using RecordBatches = std::vector<std::shared_ptr<arrow::RecordBatch>>;

/** @brief Materialized runtime counterpart for one physical-plan node. */
class IExecutionState
{
public:
    virtual ~IExecutionState() = default;

    /** @brief Executes this node and returns all output batches.
     * @return Materialized output batches. */
    virtual RecordBatches execute() = 0;
    /** @brief Returns a short name identifying this node type (for diagnostics).
     * @return Null-terminated type label. */
    virtual std::string_view typeName() const noexcept = 0;
    /** @brief Returns child execution state nodes, if any.
     * @return Reference to the children vector. */
    virtual const std::vector<std::unique_ptr<IExecutionState>>& children() const noexcept = 0;
};

/** @brief Builds an execution state tree from a physical plan.
 * @param[in] root Physical plan root.
 * @param[in] context Shared execution context.
 * @param[in,out] stats Shared statistics accumulator.
 * @return Root execution state node.
 * @throws std::runtime_error On unknown plan node types. */
std::unique_ptr<IExecutionState> makeExecutionState(const plan::PhysicalNodePtr& root,
                                                    const ExecutionContext&      context,
                                                    QueryStats&                  stats);

} // namespace mldp_pvxs_driver::query::executor
