//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file AggregateRecordBatchStream.h
 * @brief Declares the streaming GROUP BY operator. */
#pragma once

#include <query/IQueryable.h>
#include <query/executor/ExecutionState.h>
#include <query/executor/GroupedAggregator.h>
#include <query/plan/PhysicalPlan.h>

#include <cstddef>

namespace mldp_pvxs_driver::query::executor {

/** @brief Pulls every input batch into a GroupedAggregator, then emits the groups.
 *
 *  Input batches are released as soon as they are folded, so memory follows the
 *  number of groups rather than the number of input rows. */
class AggregateRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Wraps @p input with the aggregation described by @p node. */
    AggregateRecordBatchStream(IRecordBatchStreamUPtr input, const plan::PhysicalAggregate& node, ExecutionContext context);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;      ///< Upstream pull stream; released after EOF.
    GroupedAggregator      aggregator_; ///< Group state.
    RecordBatches          output_;     ///< Result batches after finish().
    std::size_t            next_{0};    ///< Next output batch to emit.
    bool                   finished_{false};
};

} // namespace mldp_pvxs_driver::query::executor
