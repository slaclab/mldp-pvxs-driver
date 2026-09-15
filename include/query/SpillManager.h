//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
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
    std::string                    path;        ///< Absolute filesystem path to the Arrow IPC file.
    std::shared_ptr<arrow::Schema> schema;      ///< Arrow schema of the batches stored in this artifact.
    int64_t                        byte_count{0};  ///< Total bytes written to the file.
    int                            batch_count{0}; ///< Number of record batches stored.
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

    /** @brief Returns the next batch from the spill file, or an error status at EOF.
     *  @return Arrow Result containing a batch, or a null shared_ptr at EOF. */
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
    /** @brief Constructs a manager rooted at the given directory on the given file system.
     *  @param[in] file_system       File system for artifact creation.
     *  @param[in] spill_directory   Root directory for spill files. */
    SpillManager(std::shared_ptr<arrow::fs::FileSystem> file_system, std::string spill_directory);

    /** @brief Writes batches to a new spill artifact and returns its handle.
     *  @param[in] query_id Identifier used to name the artifact.
     *  @param[in] batches  Batches to write.
     *  @return Arrow Result with the handle, or an error status. */
    arrow::Result<SpillHandle> spill(const std::string& query_id,
                                     const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);

    /** @brief Opens a streaming writer for incremental spill.
     *  @param[in] query_id Identifier used to name the artifact.
     *  @param[in] schema   Schema of batches to be written.
     *  @return Arrow Result with the writer, or an error status. */
    arrow::Result<SpillWriter> openWriter(const std::string& query_id,
                                          std::shared_ptr<arrow::Schema> schema);

    /** @brief Opens a reader for a previously written spill artifact.
     *  @param[in] handle Handle returned by spill() or SpillWriter::finish().
     *  @return Arrow Result with the reader, or an error status. */
    arrow::Result<SpillReader> read(const SpillHandle& handle);

    /** @brief Removes all spill artifacts tracked by this manager.
     *  @return Arrow Status indicating success or the first removal error. */
    arrow::Status cleanup();

private:
    friend class SpillReader;
    using State = SpillReader::State;
    std::shared_ptr<State> state_;
};

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

    /** @brief Appends one batch to the spill artifact.
     *  @param[in] batch Batch to write; must match the schema passed to openWriter().
     *  @return Arrow Status. */
    arrow::Status append(const std::shared_ptr<arrow::RecordBatch>& batch);

    /** @brief Flushes, closes, and returns the completed spill artifact handle.
     *  @return Arrow Result with the handle, or an error status. */
    arrow::Result<SpillHandle> finish();

private:
    friend class SpillManager;
    SpillWriter(std::shared_ptr<SpillReader::State> state,
                std::string path,
                std::shared_ptr<arrow::Schema> schema,
                std::shared_ptr<arrow::io::OutputStream> output,
                std::shared_ptr<arrow::ipc::RecordBatchWriter> writer);

    void abort();

    std::shared_ptr<SpillReader::State>            state_;         ///< Shared cleanup state with the owning manager.
    std::string                                    path_;          ///< Artifact file path.
    std::shared_ptr<arrow::Schema>                 schema_;        ///< Schema of batches being written.
    std::shared_ptr<arrow::io::OutputStream>       output_;        ///< Arrow output stream wrapping the artifact file.
    std::shared_ptr<arrow::ipc::RecordBatchWriter> writer_;        ///< Arrow IPC writer.
    int                                            batch_count_{0}; ///< Number of batches appended so far.
    bool                                           finished_{false}; ///< True after finish() is called successfully.
};

} // namespace mldp_pvxs_driver::query
