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
    Table,  ///< Terminal-compatible bordered table.
    Json,   ///< JSON Lines (one JSON object per row).
    Csv,    ///< RFC 4180 comma-separated values with header row.
    Arrow   ///< Apache Arrow IPC stream format.
};

/** @brief Presentation options for tabular terminal output. */
struct TableRenderOptions
{
    std::optional<std::size_t> viewport_width{};  ///< Terminal width in columns for wrapping; empty = no width limit.
};

/** @brief Formats materialized query batches to the given stream.
 * @param[in]  result         Batches and schema to format.
 * @param[in]  format         Target output encoding.
 * @param[out] output         Destination stream.
 * @param[in]  expanded       True to use expanded (vertical) table layout.
 * @param[in]  table_options  Viewport constraints for tabular output.
 * @param[in]  cancellation   Optional cancellation token; null disables checks. */
void formatQueryResult(const query::QueryExecutionResult& result,
                       QueryOutputFormat                  format,
                       std::ostream&                      output,
                       bool                               expanded = false,
                       const TableRenderOptions&          table_options = {},
                       std::shared_ptr<mldp_pvxs_driver::query::QueryCancellation> cancellation = nullptr);

/** @brief Pulls and formats all batches from a lazy stream.
 * @details Writes each completed batch before requesting the next, enabling interactive display.
 * @param[in,out] stream         Source pull stream; drained to EOF.
 * @param[in]     format         Target output encoding.
 * @param[out]    output         Destination stream.
 * @param[in]     expanded       True to use expanded table layout.
 * @param[in]     table_options  Viewport constraints.
 * @param[in]     cancellation   Optional cancellation token.
 * @param[in]     progress       Optional progress tracker.
 * @param[in]     output_mutex   Optional mutex protecting concurrent writes to output. */
void formatQueryStream(mldp_pvxs_driver::query::IRecordBatchStream& stream,
                       QueryOutputFormat                            format,
                       std::ostream&                                output,
                       bool                                         expanded = false,
                       const TableRenderOptions&                    table_options = {},
                       std::shared_ptr<mldp_pvxs_driver::query::QueryCancellation> cancellation = nullptr,
                       std::shared_ptr<mldp_pvxs_driver::query::QueryProgressTracker> progress = nullptr,
                       std::shared_ptr<std::mutex> output_mutex = nullptr);

/** @brief Writes a one-line query statistics summary to the stream.
 * @param[in]  stats   Completed query statistics.
 * @param[out] output  Destination stream. */
void printQueryStats(const query::QueryStats& stats, std::ostream& output);

/** @brief Formats query statistics into a single-line human-readable string.
 * @param[in] stats  Completed query statistics.
 * @return Formatted statistics line. */
std::string queryStatsLine(const query::QueryStats& stats);

} // namespace mldp_pvxs_driver::cli
