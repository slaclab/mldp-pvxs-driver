//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file WindowBackendScanRecordBatchStream.h
 * @brief Streams time-series window shards with bounded backend concurrency. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

#include <future>
#include <memory>
#include <utility>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Schedules PV shards for each time window while respecting backend concurrency. */
class WindowBackendScanRecordBatchStream final : public IRecordBatchStream
{
public:
    /** @brief Constructs a windowed scan stream and dispatches the first slice immediately.
     * @param[in] scan Physical scan node.
     * @param[in] context Execution context; series_per_shard governs shard fanout.
     * @param[in] stats Shared statistics accumulator.
     * @param[in] windows Ordered [begin_ns, end_ns] window pairs.
     * @throws std::runtime_error If windows is empty or no PV predicate is found. */
    WindowBackendScanRecordBatchStream(const plan::PhysicalTableScan&           scan,
                                       ExecutionContext                         context,
                                       std::shared_ptr<QueryStats>              stats,
                                       std::vector<std::pair<int64_t, int64_t>> windows);
    /** @brief Returns the next batch from the current shard in deterministic order, or nullptr at global EOF.
     * @return Batch or nullptr.
     * @throws std::runtime_error On backend shard failure. */
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    /** @brief Result of asynchronously pulling the next batch from one shard. */
    struct PullResult
    {
        IRecordBatchStreamUPtr              stream; ///< Shard stream; null when the shard returned its final batch.
        std::shared_ptr<arrow::RecordBatch> batch;  ///< Pulled batch; null on clean shard EOF.
    };

    /** @brief State for one PV shard in the current window slice. */
    struct Group
    {
        std::size_t             index{0};             ///< 1-based shard index within the current slice.
        uint64_t                slice_index{0};       ///< 1-based time-slice index within the current window.
        uint64_t                series_in_shard{0};   ///< Number of PV series in this shard.
        int64_t                 begin_seconds{0};     ///< Shard start time in Unix seconds.
        int64_t                 end_seconds{0};       ///< Shard end time in Unix seconds.
        int64_t                 begin_ns{0};          ///< Shard start time in nanoseconds.
        int64_t                 end_ns{0};            ///< Shard end time in nanoseconds.
        bool                    final_slice{false};   ///< True if this is the last time slice of the window.
        std::vector<Predicate>  predicates;           ///< Executable predicates for this shard (time + PV range).
        std::size_t             trace_entry_index{0}; ///< Index into the ShardTraceCollector for this shard.
        IRecordBatchStreamUPtr  stream;               ///< Owned backend stream; valid between first and final batch.
        std::future<PullResult> next;                 ///< Future holding the in-flight async pull result.
    };

    /** @brief Dispatches an async task to open the backend stream and pull the first batch for a shard.
     * @param[in,out] group Shard group to schedule. */
    void scheduleFirst(Group& group);
    /** @brief Dispatches an async task to pull the next batch from an already-open shard stream.
     * @param[in,out] group Shard group to advance. */
    void scheduleNext(Group& group);
    /** @brief Builds Group entries for the next time slice and pre-launches up to concurrency shards. */
    void prepareNextSlice();
    /** @brief Initializes slice bounds from the current window entry. */
    void selectWindow();

    plan::PhysicalTableScan                  scan_;                   ///< Physical scan parameters.
    ExecutionContext                         context_;                ///< Execution context.
    std::shared_ptr<QueryStats>              stats_;                  ///< Shared statistics accumulator.
    IQueryableUPtr                           queryable_;              ///< Backend queryable instance.
    std::vector<std::string>                 requested_pvs_;          ///< PV names extracted from pushable predicates.
    std::vector<Group>                       groups_;                 ///< All shard groups for the current time slice.
    std::size_t                              group_index_{0};         ///< Index of the group currently being consumed.
    std::size_t                              next_group_to_start_{0}; ///< Index of the next group to pre-launch asynchronously.
    uint64_t                                 parallel_shard_limit_{0};///< Concurrency cap for the current slice.
    std::vector<std::pair<int64_t, int64_t>> windows_;               ///< Ordered [begin_ns, end_ns] window pairs.
    std::size_t                              window_index_{0};        ///< Index into windows_ currently being scanned.
    int64_t                                  window_begin_ns_{0};     ///< Start of the current window in nanoseconds.
    int64_t                                  window_end_ns_{0};       ///< End of the current window in nanoseconds.
    int64_t                                  slice_begin_ns_{0};      ///< Start of the current time slice in nanoseconds.
    int64_t                                  slice_end_ns_{0};        ///< End of the current time slice in nanoseconds.
    bool                                     final_slice_{false};     ///< True when the current slice reaches window_end_ns_.
};

} // namespace mldp_pvxs_driver::query
