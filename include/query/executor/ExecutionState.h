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

/** Runtime counterpart to one immutable physical-plan node. */
/** @brief Materialized runtime counterpart for one physical-plan node. */
class IExecutionState
{
public:
    virtual ~IExecutionState() = default;

    virtual RecordBatches execute() = 0;
    virtual std::string_view typeName() const noexcept = 0;
    virtual const std::vector<std::unique_ptr<IExecutionState>>& children() const noexcept = 0;
};

/** Builds a runtime state tree once for a physical plan. */
std::unique_ptr<IExecutionState> makeExecutionState(const plan::PhysicalNodePtr& root,
                                                    const ExecutionContext&      context,
                                                    QueryStats&                  stats);

} // namespace mldp_pvxs_driver::query::executor
