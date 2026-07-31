//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryResult.h
 * @brief Defines one backend page and its continuation token. */
#pragma once

#include <arrow/record_batch.h>

#include <memory>
#include <string>

namespace mldp_pvxs_driver::query {

/** @brief One backend response page, optionally followed by a continuation token. */
struct QueryResult {
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string                          next_page_token;
};

} // namespace mldp_pvxs_driver::query
