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

struct ShardTraceEntry
{
    uint64_t                              window_index{0};
    uint64_t                              slice_index{0};
    uint64_t                              shard_index{0};
    std::vector<std::string>              pvs;
    int64_t                               begin_seconds{0};
    int64_t                               end_seconds{0};
    std::chrono::steady_clock::time_point dispatched_at;
    std::chrono::steady_clock::time_point first_response_at;
    std::chrono::steady_clock::time_point completed_at;
    uint64_t                              first_response_rows{0};
    uint64_t                              rows{0};
    uint64_t                              batches{0};
    uint64_t                              first_response_sequence{0};
    uint64_t                              completion_sequence{0};
    std::string                           failure;
};

/** @brief Thread-safe, opt-in collector for one query's backend shard events. */
class ShardTraceCollector
{
public:
    std::size_t                  begin(uint64_t                 window_index,
                                       uint64_t                 slice_index,
                                       uint64_t                 shard_index,
                                       std::vector<std::string> pvs,
                                       int64_t                  begin_seconds,
                                       int64_t                  end_seconds);
    void                         recordBatch(std::size_t entry_index, uint64_t rows);
    void                         complete(std::size_t entry_index);
    void                         fail(std::size_t entry_index, std::string failure);
    std::vector<ShardTraceEntry> entries() const;

private:
    mutable std::mutex           mutex_;
    std::vector<ShardTraceEntry> entries_;
    uint64_t                     first_response_sequence_{0};
    uint64_t                     completion_sequence_{0};
};

} // namespace mldp_pvxs_driver::query
