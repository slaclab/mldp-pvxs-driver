//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////
#include <query/executor/FinalizingRecordBatchStream.h>

#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>

#include <arrow/memory_pool.h>

#include <utility>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

FinalizingRecordBatchStream::FinalizingRecordBatchStream(IRecordBatchStreamUPtr stream,
                                                         ExecutionContext context,
                                                         std::shared_ptr<QueryStats> stats,
                                                         std::chrono::steady_clock::time_point start)
    : stream_(std::move(stream)), context_(std::move(context)), stats_(std::move(stats)), start_(start)
{
}

std::shared_ptr<arrow::RecordBatch> FinalizingRecordBatchStream::next()
{
    if (finished_) return nullptr;
    if (context_.cancellation) context_.cancellation->throwIfCancelled();
    auto batch = stream_->next();
    if (batch)
    {
        stats_->rows_returned += static_cast<uint64_t>(batch->num_rows());
        return batch;
    }
    finished_ = true;
    stats_->elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
    if (context_.pool != nullptr) stats_->peak_memory_bytes = static_cast<uint64_t>(context_.pool->max_memory());
    if (context_.progress)
        context_.progress->updateStats(stats_->rows_returned, stats_->bytes_spilled, stats_->materialized_bytes,
                                       stats_->materialized_files, stats_->peak_memory_bytes);
    return nullptr;
}
