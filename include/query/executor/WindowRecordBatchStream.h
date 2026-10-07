//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file WindowRecordBatchStream.h
 * @brief Declares the streaming window-function (`OVER`) operator. */
#pragma once

#include <query/IQueryable.h>
#include <query/executor/WindowEvaluator.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

/** @brief Pulls every input batch into a WindowOperator, then emits the rows with the window columns appended.
 *
 *  Input above the memory limit is spilled in sorted runs; output is produced
 *  one partition at a time while the runs are merged. */
class WindowRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Wraps @p input with the window step described by @p node. */
    WindowRecordBatchStream(IRecordBatchStreamUPtr input, const plan::PhysicalWindow& node, ExecutionContext context);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;  ///< Upstream pull stream; released after EOF.
    WindowOperator         window_; ///< Window state.
};

} // namespace mldp_pvxs_driver::query::executor
