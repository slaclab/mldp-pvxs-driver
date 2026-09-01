//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file InputBuffer.h
 * @brief Buffer that tracks the latest source batches and assembles snapshots.
 */

#pragma once

#include <processor/AlignedSnapshot.h>
#include <processor/AlignmentPolicy.h>
#include <processor/TriggerPolicy.h>
#include <util/bus/IDataBus.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mldp_pvxs_driver::processor {

/**
 * @class InputBuffer
 * @brief Buffers required source updates until a snapshot can be emitted.
 * @details
 * The buffer stores the latest batch per configured source and tracks which
 * sources have produced fresh data since the last reset. Snapshot eligibility
 * is evaluated according to the requested trigger policy.
 */
class InputBuffer
{
public:
    /**
     * @brief Create an input buffer for a fixed set of required source names.
     * @param[in] source_names Ordered list of required source identifiers.
     * @param[in] policy Alignment strategy used when retaining source batches.
     */
    explicit InputBuffer(const std::vector<std::string>& source_names,
                         AlignmentPolicy                 policy,
                         std::size_t                     max_depth = 0);

    /**
     * @brief Ingest a time-series payload for one source.
     * @param[in] root_source_name Root source name associated with the payload.
     * @param[in] payload Incoming time-series payload to buffer.
     */
    void ingest(const std::string& root_source_name,
                const util::bus::TimeSeriesPayload& payload);

    /**
     * @brief Attempt to assemble a snapshot under the requested trigger policy.
     * @param[in] trigger Policy that determines whether a snapshot is ready.
     * @return A snapshot when the trigger condition is satisfied; otherwise `std::nullopt`.
     */
    std::optional<AlignedSnapshot> trySnapshot(TriggerPolicy trigger);

    /** @brief Clear the fresh-update tracking without removing buffered data. */
    void resetFreshFlags();

    /** @brief Remove all buffered source data and freshness state. */
    void clear();

    /** @brief Return the deepest retained timestamp count across buffered sources. */
    std::size_t bufferDepth() const noexcept;

    /** @brief Return the retained timestamp count for one buffered source. */
    std::size_t bufferDepthForSource(const std::string& root_source_name) const noexcept;

private:
    std::unordered_map<std::string, util::bus::DataBatch> slots_;                  ///< Latest buffered batch per source.
    std::unordered_set<std::string>                       fresh_;                  ///< Sources updated since the last freshness reset.
    std::unordered_set<std::string>                       required_source_lookup_; ///< Fast lookup set for accepted source names.
    std::vector<std::string>                              required_sources_;       ///< Ordered required source list for trigger checks.
    std::unordered_set<std::string>                       seen_sources_;           ///< Dynamically tracked sources in open mode.
    AlignmentPolicy                                       alignment_;              ///< Alignment mode used when buffering payloads.
    std::size_t                                           max_depth_{0};           ///< Maximum retained samples per source; zero means unlimited.
};

} // namespace mldp_pvxs_driver::processor
