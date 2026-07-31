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

/** @file SpillManager.h
 * @brief Manages temporary Arrow IPC storage used by bounded query execution. */
#pragma once

#include <arrow/filesystem/filesystem.h>
#include <arrow/ipc/writer.h>
#include <arrow/ipc/reader.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Identifies one Arrow IPC spill artifact owned by a spill manager. */
struct SpillHandle {
    std::string                    path;
    std::shared_ptr<arrow::Schema> schema;
    int64_t                        byte_count{0};
    int                            batch_count{0};
};

class SpillManager;
class SpillWriter;

/** @brief Reads batches from an Arrow IPC spill artifact. */
class SpillReader
{
public:
    struct State;

    SpillReader() = default;
    ~SpillReader();
    SpillReader(const SpillReader&) = delete;
    SpillReader& operator=(const SpillReader&) = delete;
    SpillReader(SpillReader&&) noexcept = default;
    SpillReader& operator=(SpillReader&&) noexcept = default;

    arrow::Result<std::shared_ptr<arrow::RecordBatch>> next();

private:
    friend class SpillManager;
    SpillReader(std::shared_ptr<State> state,
                std::string path,
                std::shared_ptr<arrow::ipc::RecordBatchFileReader> reader);

    void release();

    std::shared_ptr<State>                                      state_;
    std::string                                                 path_;
    std::shared_ptr<arrow::ipc::RecordBatchFileReader>         reader_;
    int                                                         next_batch_{0};
};

/** @brief Creates and tracks temporary Arrow IPC files for one query. */
class SpillManager
{
public:
    SpillManager(std::shared_ptr<arrow::fs::FileSystem> file_system, std::string spill_directory);

    arrow::Result<SpillHandle> spill(const std::string& query_id,
                                     const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);
    arrow::Result<SpillWriter> openWriter(const std::string& query_id,
                                          std::shared_ptr<arrow::Schema> schema);
    arrow::Result<SpillReader> read(const SpillHandle& handle);
    arrow::Status cleanup();

private:
    friend class SpillReader;
    using State = SpillReader::State;
    std::shared_ptr<State> state_;
};

/** Incrementally writes one-schema Arrow IPC spill data and cleans up on abandonment. */
/** @brief Incrementally writes batches into one spill artifact. */
class SpillWriter
{
public:
    SpillWriter() = default;
    ~SpillWriter();
    SpillWriter(const SpillWriter&) = delete;
    SpillWriter& operator=(const SpillWriter&) = delete;
    SpillWriter(SpillWriter&&) noexcept = default;
    SpillWriter& operator=(SpillWriter&&) noexcept = default;

    arrow::Status append(const std::shared_ptr<arrow::RecordBatch>& batch);
    arrow::Result<SpillHandle> finish();

private:
    friend class SpillManager;
    SpillWriter(std::shared_ptr<SpillReader::State> state,
                std::string path,
                std::shared_ptr<arrow::Schema> schema,
                std::shared_ptr<arrow::io::OutputStream> output,
                std::shared_ptr<arrow::ipc::RecordBatchWriter> writer);

    void abort();

    std::shared_ptr<SpillReader::State> state_;
    std::string path_;
    std::shared_ptr<arrow::Schema> schema_;
    std::shared_ptr<arrow::io::OutputStream> output_;
    std::shared_ptr<arrow::ipc::RecordBatchWriter> writer_;
    int batch_count_{0};
    bool finished_{false};
};

} // namespace mldp_pvxs_driver::query
