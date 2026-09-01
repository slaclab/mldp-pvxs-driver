//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryStats.h
 * @brief Defines execution statistics accumulated for a query. */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Cumulative work and resource usage observed while executing one query. */
struct QueryStats {
    std::chrono::milliseconds elapsed{0};            ///< Wall-clock time from execution start to final batch, in milliseconds.
    uint64_t                  rows_from_backend{0};  ///< Total rows received across all backend RPC calls.
    uint64_t                  rows_returned{0};      ///< Rows delivered to the caller after filtering and projection.
    uint64_t                  rpc_calls{0};          ///< Number of backend executeStream calls issued.
    uint64_t                  bytes_spilled{0};      ///< Bytes written to spill storage during execution.
    uint64_t                  spill_files{0};        ///< Number of spill artifact files created.
    uint64_t                  materialized_bytes{0}; ///< Bytes held in in-memory materialized buffers at peak.
    uint64_t                  materialized_files{0}; ///< Arrow IPC catalog files read during execution.
    uint64_t                  peak_memory_bytes{0};  ///< Peak Arrow memory pool usage in bytes.
    std::string               plan_summary;          ///< Human-readable physical plan tree produced after planning.
    std::vector<std::string>  plan_warnings;         ///< Planner warnings surfaced at runtime (e.g. serial-path degradation).
};

} // namespace mldp_pvxs_driver::query
