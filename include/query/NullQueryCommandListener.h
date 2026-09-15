//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file NullQueryCommandListener.h
 * @brief Declares the silent query lifecycle listener. */
#pragma once

#include <query/QueryCommandListener.h>

namespace mldp_pvxs_driver::cli {

/** @brief Discards query lifecycle observations for silent call sites. */
class NullQueryCommandListener final : public QueryCommandListener
{
public:
    /** @brief Silently discards the query submitted notification.
     * @param[in] sql  Unused. */
    void querySubmitted(std::string_view sql) override;

    /** @brief Silently discards the progress update.
     * @param[in] progress  Unused. */
    void progressChanged(const query::QueryProgressSnapshot& progress) override;

    /** @brief Silently discards the batch notification.
     * @param[in] descriptor  Unused. */
    void resultBatchAvailable(const QueryResultBatchDescriptor& descriptor) override;

    /** @brief Silently discards the completion notification.
     * @param[in] stats  Unused. */
    void queryCompleted(const query::QueryStats& stats) override;

    /** @brief Silently discards the cancellation notification. */
    void queryCancelled() override;

    /** @brief Silently discards the failure notification.
     * @param[in] error  Unused. */
    void queryFailed(std::string_view error) override;

    /** @brief Silently discards the idle notification. */
    void queryIdle() override;
};

} // namespace mldp_pvxs_driver::cli
