//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file LoggingQueryCommandListener.h
 * @brief Declares the driver-log lifecycle listener for query commands. */
#pragma once

#include <query/QueryCommandListener.h>

namespace mldp_pvxs_driver::cli {

/** @brief Writes query lifecycle observations through the normal driver logger. */
class LoggingQueryCommandListener final : public QueryCommandListener
{
public:
    /** @brief Logs the submitted SQL text via the driver logger.
     * @param[in] sql  The submitted SQL text. */
    void querySubmitted(std::string_view sql) override;

    /** @brief Logs progress changes at trace level.
     * @param[in] progress  Latest progress snapshot. */
    void progressChanged(const query::QueryProgressSnapshot& progress) override;

    /** @brief Logs batch availability at trace level.
     * @param[in] descriptor  Batch metadata. */
    void resultBatchAvailable(const QueryResultBatchDescriptor& descriptor) override;

    /** @brief Logs query completion with statistics.
     * @param[in] stats  Final execution statistics. */
    void queryCompleted(const query::QueryStats& stats) override;

    /** @brief Logs query cancellation. */
    void queryCancelled() override;

    /** @brief Logs query failure with the error message.
     * @param[in] error  Error description. */
    void queryFailed(std::string_view error) override;

    /** @brief Logs return to idle state. */
    void queryIdle() override;
};

} // namespace mldp_pvxs_driver::cli
