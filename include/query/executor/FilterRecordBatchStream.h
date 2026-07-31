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
    FilterRecordBatchStream(IRecordBatchStreamUPtr input, std::vector<Predicate> predicates);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IRecordBatchStreamUPtr input_;
    std::vector<Predicate> predicates_;
};

} // namespace mldp_pvxs_driver::query::executor
