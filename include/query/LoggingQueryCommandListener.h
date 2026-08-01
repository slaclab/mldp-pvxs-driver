//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
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
    void querySubmitted(std::string_view sql) override;
    void progressChanged(const query::QueryProgressSnapshot& progress) override;
    void resultBatchAvailable(const QueryResultBatchDescriptor& descriptor) override;
    void queryCompleted(const query::QueryStats& stats) override;
    void queryCancelled() override;
    void queryFailed(std::string_view error) override;
    void queryIdle() override;
};

} // namespace mldp_pvxs_driver::cli
