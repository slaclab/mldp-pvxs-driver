//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file GroupedAggregator.h
 * @brief Declares the GROUP BY execution core shared by the streaming and materialized paths. */
#pragma once

#include <query/AggregateRegistry.h>
#include <query/ExecutionContext.h>
#include <query/SpillManager.h>
#include <query/executor/ExecutionState.h>
#include <query/plan/LogicalPlan.h>

#include <arrow/record_batch.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace mldp_pvxs_driver::query::executor {

/** @brief Incremental GROUP BY: consume() input batches, then finish() the groups.
 *
 *  Memory grows with the number of groups, not input rows.  When the state
 *  passes the context memory limit and a spill manager is available, input is
 *  hash-partitioned by key into spill files and each partition is aggregated
 *  separately at finish(); partitions hold disjoint keys, so no merge is needed.
 *  Group order is first appearance, except after a spill (partition order). */
class GroupedAggregator
{
public:
    /** @brief Prepares the aggregator for @p spec. */
    GroupedAggregator(plan::AggregateSpec spec, ExecutionContext context);
    ~GroupedAggregator();

    /** @brief Folds one input batch into the groups. */
    void consume(const std::shared_ptr<arrow::RecordBatch>& batch);

    /** @brief Emits the aggregate output (keys then aggregates, HAVING applied) in batches of at most @p batch_rows rows. */
    RecordBatches finish(int64_t batch_rows = 65536);

    /** @brief Current number of in-memory groups. */
    std::size_t groupCount() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

/** @brief Runs a whole GROUP BY over materialized input (convenience for the materialized path). */
RecordBatches applyAggregate(const RecordBatches& input, const plan::AggregateSpec& spec, const ExecutionContext& context);

} // namespace mldp_pvxs_driver::query::executor
