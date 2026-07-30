//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace mldp_pvxs_driver::query {

enum class QueryProgressPhase
{
    Idle,
    Parsing,
    Planning,
    Executing,
    BackendRpc,
    Formatting,
    Cancelling,
    Complete,
    Failed
};

struct QueryProgressSnapshot
{
    QueryProgressPhase         phase{QueryProgressPhase::Idle};
    std::chrono::milliseconds elapsed{0};
    std::string                table_name;
    std::string                detail;
    std::string                operation;
    uint64_t                   rpc_calls_started{0};
    uint64_t                   rpc_calls_completed{0};
    uint64_t                   rows_from_backend{0};
    uint64_t                   rows_returned{0};
    uint64_t                   bytes_spilled{0};
    uint64_t                   materialized_bytes{0};
    uint64_t                   materialized_files{0};
    uint64_t                   peak_memory_bytes{0};
    uint64_t                   stream_batches{0};
    uint64_t                   output_batches{0};
    uint64_t                   cursor_next_requests{0};
    uint64_t                   cursor_responses{0};
    uint64_t                   result_page{0};
    uint64_t                   window_index{0};
    uint64_t                   slice_index{0};
    uint64_t                   series_shard_index{0};
    uint64_t                   series_in_shard{0};
    uint64_t                   active_parallel_shards{0};
    uint64_t                   parallel_shard_limit{0};
    uint64_t                   completed_shards{0};
};

class QueryProgressTracker
{
public:
    QueryProgressTracker()
        : started_(std::chrono::steady_clock::now())
    {
    }

    void setPhase(const QueryProgressPhase phase, std::string detail = {})
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = phase;
        if (!detail.empty()) detail_ = std::move(detail);
    }

    void beginBackendRpc(std::string table_name, std::string detail = {})
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = QueryProgressPhase::BackendRpc;
        table_name_ = std::move(table_name);
        detail_ = std::move(detail);
        ++rpc_calls_started_;
    }

    void finishBackendRpc(const uint64_t rows_from_backend)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++rpc_calls_completed_;
        rows_from_backend_ += rows_from_backend;
    }

    void setActivity(std::string table_name, std::string operation, std::string detail = {})
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = QueryProgressPhase::Executing;
        table_name_ = std::move(table_name);
        operation_ = std::move(operation);
        if (!detail.empty()) detail_ = std::move(detail);
    }

    void setWindowShard(const uint64_t window_index, const uint64_t slice_index,
                        const uint64_t series_shard_index, const uint64_t series_in_shard)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_index_ = window_index;
        slice_index_ = slice_index;
        series_shard_index_ = series_shard_index;
        series_in_shard_ = series_in_shard;
    }

    void setParallelShards(const uint64_t active, const uint64_t limit)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_parallel_shards_ = active;
        parallel_shard_limit_ = limit;
    }

    void cursorNext()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++cursor_next_requests_;
    }

    void cursorResponse(const uint64_t rows)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++cursor_responses_;
        ++stream_batches_;
        static_cast<void>(rows);
    }

    void outputBatch(const uint64_t rows)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++output_batches_;
        rows_returned_ += rows;
    }

    void completeShard()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++completed_shards_;
    }

    void setResultPage(const uint64_t page)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_page_ = page;
    }

    void updateStats(const uint64_t rows_returned,
                     const uint64_t bytes_spilled,
                     const uint64_t materialized_bytes,
                     const uint64_t materialized_files,
                     const uint64_t peak_memory_bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rows_returned_ = rows_returned;
        bytes_spilled_ = bytes_spilled;
        materialized_bytes_ = materialized_bytes;
        materialized_files_ = materialized_files;
        peak_memory_bytes_ = peak_memory_bytes;
    }

    QueryProgressSnapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return QueryProgressSnapshot{
            .phase = phase_,
            .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_),
            .table_name = table_name_,
            .detail = detail_,
            .operation = operation_,
            .rpc_calls_started = rpc_calls_started_,
            .rpc_calls_completed = rpc_calls_completed_,
            .rows_from_backend = rows_from_backend_,
            .rows_returned = rows_returned_,
            .bytes_spilled = bytes_spilled_,
            .materialized_bytes = materialized_bytes_,
            .materialized_files = materialized_files_,
            .peak_memory_bytes = peak_memory_bytes_,
            .stream_batches = stream_batches_,
            .output_batches = output_batches_,
            .cursor_next_requests = cursor_next_requests_,
            .cursor_responses = cursor_responses_,
            .result_page = result_page_,
            .window_index = window_index_,
            .slice_index = slice_index_,
            .series_shard_index = series_shard_index_,
            .series_in_shard = series_in_shard_,
            .active_parallel_shards = active_parallel_shards_,
            .parallel_shard_limit = parallel_shard_limit_,
            .completed_shards = completed_shards_,
        };
    }

private:
    const std::chrono::steady_clock::time_point started_;
    mutable std::mutex                          mutex_;
    QueryProgressPhase                          phase_{QueryProgressPhase::Idle};
    std::string                                 table_name_;
    std::string                                 detail_;
    std::string                                 operation_;
    uint64_t                                    rpc_calls_started_{0};
    uint64_t                                    rpc_calls_completed_{0};
    uint64_t                                    rows_from_backend_{0};
    uint64_t                                    rows_returned_{0};
    uint64_t                                    bytes_spilled_{0};
    uint64_t                                    materialized_bytes_{0};
    uint64_t                                    materialized_files_{0};
    uint64_t                                    peak_memory_bytes_{0};
    uint64_t                                    stream_batches_{0};
    uint64_t                                    output_batches_{0};
    uint64_t                                    cursor_next_requests_{0};
    uint64_t                                    cursor_responses_{0};
    uint64_t                                    result_page_{0};
    uint64_t                                    window_index_{0};
    uint64_t                                    slice_index_{0};
    uint64_t                                    series_shard_index_{0};
    uint64_t                                    series_in_shard_{0};
    uint64_t                                    active_parallel_shards_{0};
    uint64_t                                    parallel_shard_limit_{0};
    uint64_t                                    completed_shards_{0};
};

inline const char* queryProgressPhaseName(const QueryProgressPhase phase) noexcept
{
    switch (phase)
    {
        case QueryProgressPhase::Idle: return "idle";
        case QueryProgressPhase::Parsing: return "parsing";
        case QueryProgressPhase::Planning: return "planning";
        case QueryProgressPhase::Executing: return "executing";
        case QueryProgressPhase::BackendRpc: return "backend RPC";
        case QueryProgressPhase::Formatting: return "formatting";
        case QueryProgressPhase::Cancelling: return "cancelling";
        case QueryProgressPhase::Complete: return "complete";
        case QueryProgressPhase::Failed: return "failed";
    }
    return "unknown";
}

} // namespace mldp_pvxs_driver::query
