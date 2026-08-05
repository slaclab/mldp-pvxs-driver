//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryCommandListener.h
 * @brief Declares UI-neutral query lifecycle observation. */
#pragma once

#include <query/QueryProgress.h>
#include <query/QueryStats.h>

#include <cstdint>
#include <string_view>

namespace mldp_pvxs_driver::cli {

/** @brief Immutable metadata for a record batch made available to an interactive client. */
struct QueryResultBatchDescriptor
{
    uint64_t rows{0};   ///< Number of rows in the batch.
    uint64_t bytes{0};  ///< Estimated byte size of the batch.
};

/** @brief Receives ordered immutable query lifecycle observations. */
class QueryCommandListener
{
public:
    virtual ~QueryCommandListener() = default;

    /** @brief Called when a query is submitted for execution.
     * @param[in] sql  The submitted SQL text. */
    virtual void querySubmitted(std::string_view sql) = 0;

    /** @brief Called when execution progress changes.
     * @param[in] progress  Latest progress snapshot. */
    virtual void progressChanged(const query::QueryProgressSnapshot& progress) = 0;

    /** @brief Called when a formatted output batch is ready.
     * @param[in] descriptor  Metadata describing the batch. */
    virtual void resultBatchAvailable(const QueryResultBatchDescriptor& descriptor) = 0;

    /** @brief Called when the query completes successfully.
     * @param[in] stats  Final execution statistics. */
    virtual void queryCompleted(const query::QueryStats& stats) = 0;

    /** @brief Called when the query is cancelled by the user. */
    virtual void queryCancelled() = 0;

    /** @brief Called when the query fails with an error.
     * @param[in] error  Human-readable error description. */
    virtual void queryFailed(std::string_view error) = 0;

    /** @brief Called when the query subsystem returns to idle state. */
    virtual void queryIdle() = 0;
};

} // namespace mldp_pvxs_driver::cli
