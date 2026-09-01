//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file ShardTrace.h
 * @brief Collects optional per-shard timing diagnostics for windowed scans. */
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Diagnostic timing record for one PV shard execution within a windowed scan. */
struct ShardTraceEntry
{
    uint64_t                              window_index{0};            ///< 1-based window index within the query.
    uint64_t                              slice_index{0};             ///< 1-based time-slice index within the window.
    uint64_t                              shard_index{0};             ///< 1-based PV shard index within the slice.
    std::vector<std::string>              pvs;                        ///< PV names included in this shard.
    int64_t                               begin_seconds{0};           ///< Shard time range start in Unix seconds.
    int64_t                               end_seconds{0};             ///< Shard time range end in Unix seconds.
    std::chrono::steady_clock::time_point dispatched_at;              ///< Time point when the shard was asynchronously dispatched.
    std::chrono::steady_clock::time_point first_response_at;          ///< Time point of the first batch received from the backend.
    std::chrono::steady_clock::time_point completed_at;               ///< Time point when the shard finished or failed.
    uint64_t                              first_response_rows{0};     ///< Row count in the first batch received.
    uint64_t                              rows{0};                    ///< Total rows received across all batches.
    uint64_t                              batches{0};                 ///< Total batches received.
    uint64_t                              first_response_sequence{0}; ///< Global ordering counter for the first-batch event across all shards.
    uint64_t                              completion_sequence{0};     ///< Global ordering counter for the completion event across all shards.
    std::string                           failure;                    ///< Non-empty error message if the shard failed.
};

/** @brief Thread-safe, opt-in collector for one query's backend shard events. */
class ShardTraceCollector
{
public:
    /** @brief Opens a new trace entry for a dispatched shard.
     *  @param[in] window_index   1-based window index.
     *  @param[in] slice_index    1-based slice index.
     *  @param[in] shard_index    1-based shard index.
     *  @param[in] pvs            PV names in the shard.
     *  @param[in] begin_seconds  Shard start time in Unix seconds.
     *  @param[in] end_seconds    Shard end time in Unix seconds.
     *  @return Entry index to pass to recordBatch(), complete(), and fail(). */
    std::size_t                  begin(uint64_t                 window_index,
                                       uint64_t                 slice_index,
                                       uint64_t                 shard_index,
                                       std::vector<std::string> pvs,
                                       int64_t                  begin_seconds,
                                       int64_t                  end_seconds);

    /** @brief Records arrival of one backend batch for a shard.
     *  @param[in] entry_index Index returned by begin().
     *  @param[in] rows        Row count in the batch. */
    void                         recordBatch(std::size_t entry_index, uint64_t rows);

    /** @brief Marks a shard as completed successfully.
     *  @param[in] entry_index Index returned by begin(). */
    void                         complete(std::size_t entry_index);

    /** @brief Marks a shard as failed with an error message.
     *  @param[in] entry_index Index returned by begin().
     *  @param[in] failure     Error description. */
    void                         fail(std::size_t entry_index, std::string failure);

    /** @brief Returns a snapshot of all trace entries collected so far.
     *  @return Copy of all entries; thread-safe. */
    std::vector<ShardTraceEntry> entries() const;

private:
    mutable std::mutex           mutex_;                       ///< Guards all mutable state below.
    std::vector<ShardTraceEntry> entries_;                     ///< Collected trace entries.
    uint64_t                     first_response_sequence_{0}; ///< Monotonic counter for first-batch events.
    uint64_t                     completion_sequence_{0};      ///< Monotonic counter for completion events.
};

} // namespace mldp_pvxs_driver::query
