//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
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

/** @brief Non-owning and shared resources available to query execution. */
struct ExecutionContext {
    arrow::MemoryPool*                    pool{nullptr};
    std::shared_ptr<SpillManager>         spill;
    uint64_t                              memory_limit_bytes{0};
    uint32_t                              spill_partitions{16};
    uint32_t                              join_batch_size{0};
    // A non-zero value splits independent backend requests by PV/series.
    // Zero preserves the direct-client one-request compatibility contract.
    uint64_t                              series_per_shard{0};
    // Optional per-query cap.  Zero uses IQueryable::maxConcurrentStreams().
    uint64_t                              max_parallel_requests{0};
    std::shared_ptr<arrow::fs::FileSystem> spill_fs;
    std::string                           spill_dir;
    std::shared_ptr<QueryTableCatalog>    table_catalog;
    std::shared_ptr<QueryProgressTracker> progress;
    std::shared_ptr<QueryCancellation>    cancellation;
};

} // namespace mldp_pvxs_driver::query
