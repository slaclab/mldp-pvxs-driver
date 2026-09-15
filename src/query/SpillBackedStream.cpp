//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/SpillBackedStream.h>

using namespace mldp_pvxs_driver::query;

std::shared_ptr<arrow::RecordBatch> SpillBackedStream::next()
{
    auto result = reader_.next();
    if (!result.ok())
        throw std::runtime_error("SpillBackedStream read error: " + result.status().ToString());
    return *result;
}

std::shared_ptr<arrow::RecordBatch> MaterializedBatchStream::next()
{
    return index_ < batches_.size() ? batches_[index_++] : nullptr;
}

IRecordBatchStreamUPtr mldp_pvxs_driver::query::materializedStream(std::vector<std::shared_ptr<arrow::RecordBatch>> batches)
{
    return std::make_unique<MaterializedBatchStream>(std::move(batches));
}

IRecordBatchStreamUPtr mldp_pvxs_driver::query::spillAndStream(std::vector<std::shared_ptr<arrow::RecordBatch>> batches,
                                                               const ExecutionContext&                          context,
                                                               std::string_view                                 label)
{
    if (batches.empty() || !batches.front())
        return std::make_unique<SpillBackedStream>(SpillReader{});

    std::shared_ptr<SpillManager> mgr = context.spill;
    if (!mgr)
        mgr = std::make_shared<SpillManager>(
            std::make_shared<arrow::fs::LocalFileSystem>(),
            (std::filesystem::temp_directory_path() / "mldp-query-spill").string());

    auto schema = batches.front()->schema();
    auto writer_result = mgr->openWriter(std::string(label), schema);
    if (!writer_result.ok())
        throw std::runtime_error("spillAndStream: failed to open spill writer: " + writer_result.status().ToString());
    auto writer = std::move(*writer_result);
    for (const auto& batch : batches)
    {
        if (!batch || batch->num_rows() == 0)
            continue;
        auto status = writer.append(batch);
        if (!status.ok())
            throw std::runtime_error("spillAndStream: append failed: " + status.ToString());
    }
    auto handle_result = writer.finish();
    if (!handle_result.ok())
        throw std::runtime_error("spillAndStream: finish failed: " + handle_result.status().ToString());
    auto reader_result = mgr->read(*handle_result);
    if (!reader_result.ok())
        throw std::runtime_error("spillAndStream: open reader failed: " + reader_result.status().ToString());
    return std::make_unique<SpillBackedStream>(std::move(*reader_result));
}
