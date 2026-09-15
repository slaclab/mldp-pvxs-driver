//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file FilterRecordBatchStream.h
 * @brief Filters batches from an upstream pull stream. */
#pragma once

#include <query/IQueryable.h>

namespace mldp_pvxs_driver::query::executor {

/** @brief Applies Arrow-local predicates as input batches are pulled. */
class FilterRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Constructs a filter stream that applies predicates to each pulled batch.
     * @param[in] input Upstream pull stream.
     * @param[in] predicates Arrow-local predicates to apply. */
    FilterRecordBatchStream(IRecordBatchStreamUPtr input, std::vector<Predicate> predicates);

    /** @brief Returns the next batch that passes all predicates, or nullptr at EOF.
     * @return Filtered batch or nullptr. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;      ///< Upstream pull stream.
    std::vector<Predicate> predicates_; ///< Predicates applied to each input batch.
};

} // namespace mldp_pvxs_driver::query::executor
