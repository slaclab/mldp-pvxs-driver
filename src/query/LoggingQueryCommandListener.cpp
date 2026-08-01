//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/LoggingQueryCommandListener.h>

#include <util/log/Logger.h>

using namespace mldp_pvxs_driver::cli;

void LoggingQueryCommandListener::querySubmitted(const std::string_view sql)
{
    util::log::info("Query submitted: " + std::string(sql));
}

void LoggingQueryCommandListener::progressChanged(const query::QueryProgressSnapshot& progress)
{
    util::log::debug(std::string("Query progress: ") + query::queryProgressPhaseName(progress.phase));
}

void LoggingQueryCommandListener::resultBatchAvailable(const QueryResultBatchDescriptor& descriptor)
{
    util::log::debug("Query result batch: " + std::to_string(descriptor.rows) + " rows, " + std::to_string(descriptor.bytes) + " bytes");
}

void LoggingQueryCommandListener::queryCompleted(const query::QueryStats& stats)
{
    util::log::info("Query completed: " + std::to_string(stats.rows_returned) + " rows in " + std::to_string(stats.elapsed.count()) + " ms");
}

void LoggingQueryCommandListener::queryCancelled()
{
    util::log::warn("Query cancelled");
}

void LoggingQueryCommandListener::queryFailed(const std::string_view error)
{
    util::log::error("Query failed: " + std::string(error));
}

void LoggingQueryCommandListener::queryIdle()
{
    util::log::debug("Query command idle");
}
