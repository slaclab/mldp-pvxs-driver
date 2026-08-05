//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/ShardTrace.h>

#include <utility>

using namespace mldp_pvxs_driver::query;

std::size_t ShardTraceCollector::begin(const uint64_t           window_index,
                                       const uint64_t           slice_index,
                                       const uint64_t           shard_index,
                                       std::vector<std::string> pvs,
                                       const int64_t            begin_seconds,
                                       const int64_t            end_seconds)
{
    const std::lock_guard lock(mutex_);
    entries_.push_back(ShardTraceEntry{
        .window_index = window_index,
        .slice_index = slice_index,
        .shard_index = shard_index,
        .pvs = std::move(pvs),
        .begin_seconds = begin_seconds,
        .end_seconds = end_seconds,
        .dispatched_at = std::chrono::steady_clock::now(),
    });
    return entries_.size() - 1;
}

void ShardTraceCollector::recordBatch(const std::size_t entry_index, const uint64_t rows)
{
    const std::lock_guard lock(mutex_);
    auto&                 entry = entries_.at(entry_index);
    const auto            now = std::chrono::steady_clock::now();
    if (entry.batches == 0)
    {
        entry.first_response_at = now;
        entry.first_response_rows = rows;
        entry.first_response_sequence = ++first_response_sequence_;
    }
    ++entry.batches;
    entry.rows += rows;
}

void ShardTraceCollector::complete(const std::size_t entry_index)
{
    const std::lock_guard lock(mutex_);
    auto&                 entry = entries_.at(entry_index);
    if (entry.completion_sequence != 0)
        return;
    entry.completed_at = std::chrono::steady_clock::now();
    entry.completion_sequence = ++completion_sequence_;
}

void ShardTraceCollector::fail(const std::size_t entry_index, std::string failure)
{
    const std::lock_guard lock(mutex_);
    auto&                 entry = entries_.at(entry_index);
    entry.failure = std::move(failure);
    if (entry.first_response_sequence == 0)
        entry.first_response_sequence = ++first_response_sequence_;
    if (entry.completion_sequence != 0)
        return;
    entry.completed_at = std::chrono::steady_clock::now();
    entry.completion_sequence = ++completion_sequence_;
}

std::vector<ShardTraceEntry> ShardTraceCollector::entries() const
{
    const std::lock_guard lock(mutex_);
    return entries_;
}
