//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryFormatter.h
 * @brief Declares query-result formatting for interactive and machine-readable output. */
#pragma once

#include <query/QueryCancellation.h>
#include <query/QueryExecutor.h>
#include <query/QueryProgress.h>

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mldp_pvxs_driver::cli {

/** @brief Output encodings supported by the query formatter. */
enum class QueryOutputFormat
{
    Table,
    Json,
    Csv,
    Arrow
};

/** @brief Presentation options for tabular terminal output. */
struct TableRenderOptions
{
    std::optional<std::size_t> viewport_width{};
};

void formatQueryResult(const query::QueryExecutionResult& result,
                       QueryOutputFormat                  format,
                       std::ostream&                      output,
                       bool                               expanded = false,
                       const TableRenderOptions&          table_options = {},
                       std::shared_ptr<mldp_pvxs_driver::query::QueryCancellation> cancellation = nullptr);

/** Consume a pull stream and make each completed Arrow batch visible before requesting the next one. */
void formatQueryStream(mldp_pvxs_driver::query::IRecordBatchStream& stream,
                       QueryOutputFormat                            format,
                       std::ostream&                                output,
                       bool                                         expanded = false,
                       const TableRenderOptions&                    table_options = {},
                       std::shared_ptr<mldp_pvxs_driver::query::QueryCancellation> cancellation = nullptr,
                       std::shared_ptr<mldp_pvxs_driver::query::QueryProgressTracker> progress = nullptr,
                       std::shared_ptr<std::mutex> output_mutex = nullptr);

void printQueryStats(const query::QueryStats& stats, std::ostream& output);

std::string queryStatsLine(const query::QueryStats& stats);

} // namespace mldp_pvxs_driver::cli
