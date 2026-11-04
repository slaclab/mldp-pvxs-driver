//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file WindowEvaluator.h
 * @brief Declares the window-function (`OVER`) execution core shared by the streaming and materialized paths. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/executor/ExecutionState.h>
#include <query/plan/PhysicalPlan.h>

#include <arrow/record_batch.h>

#include <memory>
#include <vector>

namespace mldp_pvxs_driver::query::executor {

class WindowGroupOperator;

/** @brief Computes every window group of a PhysicalWindow: consume() the input, then pull next().
 *
 *  Each group sorts its input by PARTITION BY then ORDER BY keys and appends one
 *  column per call; a later group consumes the previous group's output, so the
 *  final rows come out in the last group's partition order.  Input above the
 *  context memory limit is sorted in runs that are spilled and merged; one
 *  partition must still fit in memory. */
class WindowOperator
{
public:
    /** @brief Prepares the operator for @p node. */
    WindowOperator(const plan::PhysicalWindow& node, ExecutionContext context);
    ~WindowOperator();

    /** @brief Buffers one input batch. */
    void consume(const std::shared_ptr<arrow::RecordBatch>& batch);

    /** @brief Returns the next output batch (input columns, then one column per call), or null at the end. */
    std::shared_ptr<arrow::RecordBatch> next();

private:
    std::vector<std::unique_ptr<WindowGroupOperator>> groups_;
    bool                                              finished_{false};
};

/** @brief Runs a whole PhysicalWindow over materialized input. */
RecordBatches applyWindow(const RecordBatches& input, const plan::PhysicalWindow& node, const ExecutionContext& context);

} // namespace mldp_pvxs_driver::query::executor
