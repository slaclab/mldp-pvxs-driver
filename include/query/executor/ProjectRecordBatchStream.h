//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file ProjectRecordBatchStream.h
 * @brief Projects expressions and columns from an upstream pull stream. */
#pragma once

#include <query/IQueryable.h>
#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::executor {

/** @brief Evaluates a physical projection for each pulled input batch. */
class ProjectRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Constructs a projection stream that evaluates the physical project for each batch.
     * @param[in] input Upstream pull stream.
     * @param[in] project Physical projection descriptor. */
    ProjectRecordBatchStream(IRecordBatchStreamUPtr input, plan::PhysicalProject project);

    /** @brief Returns the next projected batch, or nullptr at EOF.
     * @return Projected batch or nullptr. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_; ///< Upstream pull stream.
    plan::PhysicalProject project_; ///< Projection descriptor to apply.
};

} // namespace mldp_pvxs_driver::query::executor
