//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file ExecutionContext.h
 * @brief Collects resources and per-query execution controls. */
#pragma once

#include <memory>
#include <string>

namespace arrow {
class MemoryPool;

namespace fs {
    class FileSystem;
}
} // namespace arrow

namespace mldp_pvxs_driver::query {

class SpillManager;
class QueryTableCatalog;
class QueryProgressTracker;
class QueryCancellation;
class ShardTraceCollector;

/** @brief Non-owning and shared resources available to query execution. */
struct ExecutionContext
{
    arrow::MemoryPool*            pool{nullptr};             ///< Arrow memory pool used for all allocations; may be null.
    std::shared_ptr<SpillManager> spill;                     ///< Disk-backed spill manager for bounded execution.
    uint64_t                      memory_limit_bytes{0};     ///< Soft memory limit in bytes; 0 = unlimited.
    uint32_t                      spill_partitions{16};      ///< Number of partitions used during spill-based operations.
    uint32_t                      join_batch_size{0};        ///< Maximum rows per join probe batch; 0 = use default.
    uint64_t                      series_per_shard{0};       ///< PVs per backend shard; 0 disables shard splitting.
    uint64_t                      max_parallel_requests{0};  ///< Max concurrent shard requests; 0 defers to IQueryable::maxConcurrentStreams().
    std::shared_ptr<arrow::fs::FileSystem> spill_fs;         ///< File system used to create and remove spill artifacts.
    std::string                            spill_dir;        ///< Root directory for spill artifacts.
    std::shared_ptr<QueryTableCatalog>     table_catalog;    ///< Session and persistent table catalog; may be null.
    std::shared_ptr<QueryProgressTracker>  progress;         ///< Live progress tracker; null disables progress reporting.
    std::shared_ptr<QueryCancellation>     cancellation;     ///< Shared cancellation token; null disables cancellation checks.
    std::shared_ptr<ShardTraceCollector>   shard_trace;      ///< Optional per-shard timing collector; null disables tracing.
};

} // namespace mldp_pvxs_driver::query
