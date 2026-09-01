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

#include <query/SpillManager.h>

#include <arrow/ipc/writer.h>

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <utility>

using namespace mldp_pvxs_driver::query;

struct SpillReader::State {
    State(std::shared_ptr<arrow::fs::FileSystem> file_system_value,
          std::string spill_directory_value)
        : file_system(std::move(file_system_value))
        , spill_directory(std::move(spill_directory_value))
    {
    }

    std::shared_ptr<arrow::fs::FileSystem> file_system;
    std::string                             spill_directory;
    std::mutex                              mutex;
    std::unordered_set<std::string>         outstanding_paths;
    std::atomic<uint64_t>                    next_sequence{0};
};

namespace {

arrow::Status removePath(const std::shared_ptr<SpillReader::State>& state, const std::string& path)
{
    const auto status = state->file_system->DeleteFile(path);
    if (!status.ok())
    {
        return status;
    }

    std::lock_guard lock(state->mutex);
    state->outstanding_paths.erase(path);
    return arrow::Status::OK();
}

std::string joinPath(const std::string& directory, const std::string& filename)
{
    if (directory.empty() || directory == "/")
    {
        return directory == "/" ? "/" + filename : filename;
    }
    return directory.back() == '/' ? directory + filename : directory + "/" + filename;
}

} // namespace

SpillReader::SpillReader(std::shared_ptr<State> state,
                         std::string path,
                         std::shared_ptr<arrow::ipc::RecordBatchFileReader> reader)
    : state_(std::move(state))
    , path_(std::move(path))
    , reader_(std::move(reader))
{
}

SpillReader::~SpillReader()
{
    release();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> SpillReader::next()
{
    if (!reader_)
    {
        return std::shared_ptr<arrow::RecordBatch>{};
    }

    if (next_batch_ >= reader_->num_record_batches())
    {
        const auto status = removePath(state_, path_);
        if (!status.ok())
        {
            return status;
        }
        reader_.reset();
        path_.clear();
        return std::shared_ptr<arrow::RecordBatch>{};
    }

    ARROW_ASSIGN_OR_RAISE(auto batch, reader_->ReadRecordBatch(next_batch_++));
    if (next_batch_ == reader_->num_record_batches())
    {
        const auto status = removePath(state_, path_);
        if (!status.ok())
        {
            return status;
        }
        reader_.reset();
        path_.clear();
    }
    return batch;
}

void SpillReader::release()
{
    if (state_ && !path_.empty())
    {
        static_cast<void>(removePath(state_, path_));
    }
    path_.clear();
    reader_.reset();
}

SpillManager::SpillManager(std::shared_ptr<arrow::fs::FileSystem> file_system,
                           std::string spill_directory)
    : state_(std::make_shared<State>(std::move(file_system), std::move(spill_directory)))
{
}

arrow::Result<SpillHandle> SpillManager::spill(
    const std::string& query_id,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches)
{
    if (batches.empty() || !batches.front())
    {
        return arrow::Status::Invalid("SpillManager::spill requires at least one record batch");
    }

    const auto schema = batches.front()->schema();
    for (const auto& batch : batches)
    {
        if (!batch || !batch->schema()->Equals(*schema))
        {
            return arrow::Status::Invalid("SpillManager::spill requires batches with one schema");
        }
    }

    RETURN_NOT_OK(state_->file_system->CreateDir(state_->spill_directory, true));
    const auto sequence = state_->next_sequence.fetch_add(1);
    const auto path = joinPath(state_->spill_directory,
                               "spill_" + query_id + "_" + std::to_string(sequence) + ".arrow");

    ARROW_ASSIGN_OR_RAISE(auto output, state_->file_system->OpenOutputStream(path));
    ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeFileWriter(output, schema));
    for (const auto& batch : batches)
    {
        RETURN_NOT_OK(writer->WriteRecordBatch(*batch));
    }
    RETURN_NOT_OK(writer->Close());
    RETURN_NOT_OK(output->Close());

    ARROW_ASSIGN_OR_RAISE(auto info, state_->file_system->GetFileInfo(path));
    {
        std::lock_guard lock(state_->mutex);
        state_->outstanding_paths.insert(path);
    }
    return SpillHandle{.path = path,
                       .schema = schema,
                       .byte_count = info.size(),
                       .batch_count = static_cast<int>(batches.size())};
}

arrow::Result<SpillWriter> SpillManager::openWriter(const std::string& query_id,
                                                     std::shared_ptr<arrow::Schema> schema)
{
    if (!schema) return arrow::Status::Invalid("SpillManager::openWriter requires a schema");
    RETURN_NOT_OK(state_->file_system->CreateDir(state_->spill_directory, true));
    const auto sequence = state_->next_sequence.fetch_add(1);
    const auto path = joinPath(state_->spill_directory,
                               "spill_" + query_id + "_" + std::to_string(sequence) + ".arrow");
    ARROW_ASSIGN_OR_RAISE(auto output, state_->file_system->OpenOutputStream(path));
    ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeFileWriter(output, schema));
    {
        std::lock_guard lock(state_->mutex);
        state_->outstanding_paths.insert(path);
    }
    return SpillWriter(state_, path, std::move(schema), std::move(output), std::move(writer));
}

arrow::Result<SpillReader> SpillManager::read(const SpillHandle& handle)
{
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->outstanding_paths.contains(handle.path))
        {
            return arrow::Status::KeyError("SpillManager: unknown or consumed spill file: ", handle.path);
        }
    }
    ARROW_ASSIGN_OR_RAISE(auto input, state_->file_system->OpenInputFile(handle.path));
    ARROW_ASSIGN_OR_RAISE(auto reader, arrow::ipc::RecordBatchFileReader::Open(input));
    return SpillReader(state_, handle.path, std::move(reader));
}

arrow::Status SpillManager::cleanup()
{
    std::vector<std::string> paths;
    {
        std::lock_guard lock(state_->mutex);
        paths.assign(state_->outstanding_paths.begin(), state_->outstanding_paths.end());
    }
    for (const auto& path : paths)
    {
        RETURN_NOT_OK(removePath(state_, path));
    }
    return arrow::Status::OK();
}

SpillWriter::SpillWriter(std::shared_ptr<SpillReader::State> state,
                         std::string path,
                         std::shared_ptr<arrow::Schema> schema,
                         std::shared_ptr<arrow::io::OutputStream> output,
                         std::shared_ptr<arrow::ipc::RecordBatchWriter> writer)
    : state_(std::move(state))
    , path_(std::move(path))
    , schema_(std::move(schema))
    , output_(std::move(output))
    , writer_(std::move(writer))
{
}

SpillWriter::~SpillWriter()
{
    abort();
}

arrow::Status SpillWriter::append(const std::shared_ptr<arrow::RecordBatch>& batch)
{
    if (finished_) return arrow::Status::Invalid("SpillWriter is already finished");
    if (!batch || !batch->schema()->Equals(*schema_)) return arrow::Status::Invalid("SpillWriter batches must share one schema");
    RETURN_NOT_OK(writer_->WriteRecordBatch(*batch));
    ++batch_count_;
    return arrow::Status::OK();
}

arrow::Result<SpillHandle> SpillWriter::finish()
{
    if (finished_) return arrow::Status::Invalid("SpillWriter is already finished");
    if (batch_count_ == 0) return arrow::Status::Invalid("SpillWriter requires at least one record batch");
    RETURN_NOT_OK(writer_->Close());
    RETURN_NOT_OK(output_->Close());
    ARROW_ASSIGN_OR_RAISE(auto info, state_->file_system->GetFileInfo(path_));
    finished_ = true;
    writer_.reset();
    output_.reset();
    return SpillHandle{.path = std::move(path_), .schema = std::move(schema_), .byte_count = info.size(), .batch_count = batch_count_};
}

void SpillWriter::abort()
{
    if (finished_) return;
    if (writer_) (void)writer_->Close();
    if (output_) (void)output_->Close();
    writer_.reset();
    output_.reset();
    if (state_ && !path_.empty()) (void)removePath(state_, path_);
    path_.clear();
    finished_ = true;
}
