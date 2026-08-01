//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
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
    uint64_t rows{0};
    uint64_t bytes{0};
};

/** @brief Receives ordered immutable query lifecycle observations. */
class QueryCommandListener
{
public:
    virtual ~QueryCommandListener() = default;

    virtual void querySubmitted(std::string_view sql) = 0;
    virtual void progressChanged(const query::QueryProgressSnapshot& progress) = 0;
    virtual void resultBatchAvailable(const QueryResultBatchDescriptor& descriptor) = 0;
    virtual void queryCompleted(const query::QueryStats& stats) = 0;
    virtual void queryCancelled() = 0;
    virtual void queryFailed(std::string_view error) = 0;
    virtual void queryIdle() = 0;
};

} // namespace mldp_pvxs_driver::cli
