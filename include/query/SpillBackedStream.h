//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file SpillBackedStream.h
 * @brief IRecordBatchStream backed by a SpillReader and spillAndStream() helper. */
#pragma once

#include <query/IQueryable.h>
#include <query/SpillManager.h>
#include <query/ExecutionContext.h>

#include <arrow/filesystem/localfs.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Pulls Arrow batches from a SpillReader; nullptr on clean EOF. */
class SpillBackedStream final : public IRecordBatchStream
{
public:
    explicit SpillBackedStream(SpillReader reader) : reader_(std::move(reader)) {}

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    SpillReader reader_;
};

/** @brief Serves an in-memory batch vector via next(); no disk I/O. */
class MaterializedBatchStream final : public IRecordBatchStream
{
public:
    explicit MaterializedBatchStream(std::vector<std::shared_ptr<arrow::RecordBatch>> batches) : batches_(std::move(batches)) {}

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    std::size_t                                       index_{0};
};

/**
 * @brief Wraps an already fully-materialized batch vector in an in-memory stream.
 *
 * Use this instead of spillAndStream() for results a caller has already fully
 * drained into memory (a single built batch, or a fully-consumed cursor) —
 * spilling to disk and immediately reopening it provides no memory-bounding
 * benefit there, since peak memory has already occurred. It also avoids a
 * real hazard: concurrent shard/window fan-out (std::async) calling this
 * repeatedly opens many simultaneous Arrow IPC files from OS threads outside
 * Arrow's own managed executor, which has been observed to crash inside
 * Arrow's thread-pool/executor internals under that load.
 */
IRecordBatchStreamUPtr materializedStream(std::vector<std::shared_ptr<arrow::RecordBatch>> batches);

/**
 * @brief Spill a batch vector to a temporary Arrow IPC file and return a reading stream.
 *
 * Uses context.spill when available; otherwise falls back to a local SpillManager under
 * the system temp directory.
 */
IRecordBatchStreamUPtr spillAndStream(std::vector<std::shared_ptr<arrow::RecordBatch>> batches,
                                      const ExecutionContext&                          context,
                                      std::string_view                                 label);

} // namespace mldp_pvxs_driver::query
