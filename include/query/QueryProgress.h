//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryProgress.h
 * @brief Defines thread-safe progress snapshots for query execution and formatting. */
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace mldp_pvxs_driver::query {

/** @brief High-level lifecycle phase reported by a query. */
enum class QueryProgressPhase
{
    Idle,        ///< No query is running.
    Parsing,     ///< SQL text is being tokenized and parsed.
    Planning,    ///< The parsed statement is being bound and lowered to a physical plan.
    Executing,   ///< The physical plan is being executed.
    BackendRpc,  ///< An active backend RPC call is in progress.
    Formatting,  ///< Output batches are being formatted for the caller.
    Cancelling,  ///< A cancellation request is being propagated.
    Complete,    ///< Execution finished successfully.
    Failed       ///< Execution finished with an error.
};

/** @brief Immutable view of query activity and cumulative execution statistics. */
struct QueryProgressSnapshot
{
    QueryProgressPhase         phase{QueryProgressPhase::Idle}; ///< Current lifecycle phase.
    std::chrono::milliseconds  elapsed{0};                      ///< Wall-clock time since execution started, in milliseconds.
    std::string                table_name;                      ///< Most recently active backend table name.
    std::string                detail;                          ///< Human-readable detail for the current operation.
    std::string                operation;                       ///< Short label for the current logical operation.
    uint64_t                   rpc_calls_started{0};            ///< Backend RPC calls initiated.
    uint64_t                   rpc_calls_completed{0};          ///< Backend RPC calls that returned at least one batch.
    uint64_t                   rows_from_backend{0};            ///< Rows received from all backend calls.
    uint64_t                   rows_returned{0};                ///< Rows delivered to the caller so far.
    uint64_t                   bytes_spilled{0};                ///< Bytes written to disk-backed spill storage.
    uint64_t                   materialized_bytes{0};           ///< In-memory bytes held in materialized buffers.
    uint64_t                   materialized_files{0};           ///< Arrow IPC catalog files read.
    uint64_t                   peak_memory_bytes{0};            ///< Peak Arrow memory pool usage in bytes.
    uint64_t                   stream_batches{0};               ///< Arrow batches received from backend cursors.
    uint64_t                   output_batches{0};               ///< Batches handed to the output formatter.
    uint64_t                   cursor_next_requests{0};         ///< Number of cursor next() calls issued.
    uint64_t                   cursor_responses{0};             ///< Number of cursor responses received.
    uint64_t                   result_page{0};                  ///< Continuation page index for REPL paging (1 = first page).
    uint64_t                   window_index{0};                 ///< 1-based index of the window being processed.
    uint64_t                   slice_index{0};                  ///< 1-based time-slice index within the current window.
    uint64_t                   series_shard_index{0};           ///< 1-based PV shard index within the current slice.
    uint64_t                   series_in_shard{0};              ///< Number of PV series in the current shard.
    uint64_t                   active_parallel_shards{0};       ///< Shards currently in-flight concurrently.
    uint64_t                   parallel_shard_limit{0};         ///< Maximum concurrent shards allowed for this slice.
    uint64_t                   stage_completed_shards{0};       ///< Shards finished in the current stage.
    uint64_t                   stage_total_shards{0};           ///< Total shards in the current stage.
    uint64_t                   completed_shards{0};             ///< Cumulative shards completed across all stages.
};

/** @brief Synchronizes progress updates from query execution threads. */
class QueryProgressTracker
{
public:
    QueryProgressTracker()
        : started_(std::chrono::steady_clock::now())
    {
    }

    /** @brief Sets the current lifecycle phase and optional detail string.
     *  @param[in] phase  New phase.
     *  @param[in] detail Optional human-readable detail. */
    void setPhase(const QueryProgressPhase phase, std::string detail = {})
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = phase;
        if (!detail.empty()) detail_ = std::move(detail);
    }

    /** @brief Records the start of a backend RPC call.
     *  @param[in] table_name Table being queried.
     *  @param[in] detail     Optional detail label. */
    void beginBackendRpc(std::string table_name, std::string detail = {})
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = QueryProgressPhase::BackendRpc;
        table_name_ = std::move(table_name);
        detail_ = std::move(detail);
        ++rpc_calls_started_;
    }

    /** @brief Records the completion of a backend RPC call and its row count.
     *  @param[in] rows_from_backend Rows received in this call. */
    void finishBackendRpc(const uint64_t rows_from_backend)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++rpc_calls_completed_;
        rows_from_backend_ += rows_from_backend;
    }

    /** @brief Sets the active operation within the executing phase.
     *  @param[in] table_name Table being processed.
     *  @param[in] operation  Short operation label.
     *  @param[in] detail     Optional detail string. */
    void setActivity(std::string table_name, std::string operation, std::string detail = {})
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = QueryProgressPhase::Executing;
        table_name_ = std::move(table_name);
        operation_ = std::move(operation);
        if (!detail.empty()) detail_ = std::move(detail);
    }

    /** @brief Updates the current window/slice/shard position.
     *  @param[in] window_index       1-based window index.
     *  @param[in] slice_index        1-based time-slice index.
     *  @param[in] series_shard_index 1-based PV shard index.
     *  @param[in] series_in_shard    PV count in this shard. */
    void setWindowShard(const uint64_t window_index, const uint64_t slice_index,
                        const uint64_t series_shard_index, const uint64_t series_in_shard)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_index_ = window_index;
        slice_index_ = slice_index;
        series_shard_index_ = series_shard_index;
        series_in_shard_ = series_in_shard;
    }

    /** @brief Updates in-flight and maximum concurrent shard counts.
     *  @param[in] active Currently active shard count.
     *  @param[in] limit  Maximum concurrent shards for this slice. */
    void setParallelShards(const uint64_t active, const uint64_t limit)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_parallel_shards_ = active;
        parallel_shard_limit_ = limit;
    }

    /** @brief Starts a new shard stage with the given total count.
     *  @param[in] total Total shards in this stage. */
    void beginShardStage(const uint64_t total)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stage_completed_shards_ = 0;
        stage_total_shards_ = total;
    }

    /** @brief Increments the cursor next() request counter. */
    void cursorNext()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++cursor_next_requests_;
    }

    /** @brief Records one cursor batch response.
     *  @param[in] rows Row count in the response batch (currently unused for stats). */
    void cursorResponse(const uint64_t rows)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++cursor_responses_;
        ++stream_batches_;
        static_cast<void>(rows);
    }

    /** @brief Records one output batch delivered to the formatter.
     *  @param[in] rows Number of rows in the batch. */
    void outputBatch(const uint64_t rows)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++output_batches_;
        rows_returned_ += rows;
    }

    /** @brief Marks the current shard as finished and increments stage and cumulative counters. */
    void completeShard()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stage_completed_shards_ < stage_total_shards_) ++stage_completed_shards_;
        ++completed_shards_;
    }

    /** @brief Sets the continuation page index for REPL paging.
     *  @param[in] page 1-based page number. */
    void setResultPage(const uint64_t page)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_page_ = page;
    }

    /** @brief Updates cumulative statistics from a completed execution.
     *  @param[in] rows_returned      Total rows returned.
     *  @param[in] bytes_spilled      Bytes written to spill.
     *  @param[in] materialized_bytes In-memory materialized bytes.
     *  @param[in] materialized_files Catalog files read.
     *  @param[in] peak_memory_bytes  Peak Arrow pool usage. */
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

    /** @brief Returns an immutable snapshot of the current progress state.
     *  @return Snapshot of all tracked fields, safe to read without holding a lock. */
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
            .stage_completed_shards = stage_completed_shards_,
            .stage_total_shards = stage_total_shards_,
            .completed_shards = completed_shards_,
        };
    }

private:
    const std::chrono::steady_clock::time_point started_;                         ///< Time point at construction; used to compute elapsed.
    mutable std::mutex                          mutex_;                           ///< Guards all mutable state below.
    QueryProgressPhase                          phase_{QueryProgressPhase::Idle}; ///< Current lifecycle phase.
    std::string                                 table_name_;                      ///< Most recently active table.
    std::string                                 detail_;                          ///< Most recently set detail string.
    std::string                                 operation_;                       ///< Most recently set operation label.
    uint64_t                                    rpc_calls_started_{0};            ///< Backend RPC calls initiated.
    uint64_t                                    rpc_calls_completed_{0};          ///< Backend RPC calls completed.
    uint64_t                                    rows_from_backend_{0};            ///< Rows received from backend.
    uint64_t                                    rows_returned_{0};                ///< Rows delivered to caller.
    uint64_t                                    bytes_spilled_{0};                ///< Spill bytes written.
    uint64_t                                    materialized_bytes_{0};           ///< In-memory bytes materialized.
    uint64_t                                    materialized_files_{0};           ///< Catalog IPC files read.
    uint64_t                                    peak_memory_bytes_{0};            ///< Peak pool usage in bytes.
    uint64_t                                    stream_batches_{0};               ///< Backend batches received.
    uint64_t                                    output_batches_{0};               ///< Batches passed to the formatter.
    uint64_t                                    cursor_next_requests_{0};         ///< Cursor next() calls.
    uint64_t                                    cursor_responses_{0};             ///< Cursor batch responses.
    uint64_t                                    result_page_{0};                  ///< Current REPL continuation page.
    uint64_t                                    window_index_{0};                 ///< Current window index.
    uint64_t                                    slice_index_{0};                  ///< Current slice index.
    uint64_t                                    series_shard_index_{0};           ///< Current shard index within the slice.
    uint64_t                                    series_in_shard_{0};              ///< PVs in the current shard.
    uint64_t                                    active_parallel_shards_{0};       ///< Shards currently in flight.
    uint64_t                                    parallel_shard_limit_{0};         ///< Concurrency cap for the current slice.
    uint64_t                                    stage_completed_shards_{0};       ///< Completed shards in the current stage.
    uint64_t                                    stage_total_shards_{0};           ///< Total shards in the current stage.
    uint64_t                                    completed_shards_{0};             ///< Cumulative completed shards.
};

/** @brief Returns a short human-readable name for a query progress phase.
 *  @param[in] phase The phase to describe.
 *  @return Null-terminated ASCII string; always valid. */
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
