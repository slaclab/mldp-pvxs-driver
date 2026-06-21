//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.h>

#include <H5Cpp.h>
#include <util/log/ILog.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1 {

using namespace util::bus;

namespace {

std::vector<std::string> readFixedStringDataset(H5::Group& group, const char* name)
{
    H5::DataSet ds = group.openDataSet(name);
    H5::DataSpace space = ds.getSpace();
    hsize_t dims[1]{0};
    space.getSimpleExtentDims(dims);
    const std::size_t count = dims[0];

    H5::StrType strType = ds.getStrType();
    const std::size_t strLen = strType.getSize();

    std::vector<char> buf(count * strLen, '\0');
    ds.read(buf.data(), strType);

    std::vector<std::string> result(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const char* ptr = buf.data() + i * strLen;
        result[i] = std::string(ptr, strnlen(ptr, strLen));
    }
    return result;
}

} // anonymous namespace

HDF5BsasGen1Reader::HDF5BsasGen1Reader(
    std::shared_ptr<IDataBus>        bus,
    std::shared_ptr<metrics::Metrics> metrics,
    const config::Config&             cfg)
    : Reader(std::move(bus), std::move(metrics))
    , config_(cfg)
{
    logger_ = util::log::newLogger("hdf5_bsas_gen1_reader");
    running_ = true;
    worker_ = std::thread([this]() { readFile(); });
}

HDF5BsasGen1Reader::~HDF5BsasGen1Reader()
{
    running_ = false;
    if (worker_.joinable())
        worker_.join();
}

void HDF5BsasGen1Reader::readFile()
{
    try
    {
        logger_->log(util::log::Level::Trace,
                     "readFile: opening " + config_.filePath() + " group=" + config_.groupName());
        H5::H5File file(config_.filePath(), H5F_ACC_RDONLY);
        H5::Group dataGroup = file.openGroup(config_.groupName());

        // Read block structure
        BlockInfo block0;
        block0.items = readFixedStringDataset(dataGroup, "block0_items");

        BlockInfo block1;
        block1.items = readFixedStringDataset(dataGroup, "block1_items");

        // Detect timestamp column order from block2_items
        auto block2Items = readFixedStringDataset(dataGroup, "block2_items");
        std::size_t secCol = 0;
        std::size_t nanoCol = 1;
        for (std::size_t i = 0; i < block2Items.size(); ++i)
        {
            if (block2Items[i] == "secondsPastEpoch")
                secCol = i;
            else if (block2Items[i] == "nanoseconds")
                nanoCol = i;
        }

        logger_->log(util::log::Level::Trace,
                     "readFile: block0 columns=" + std::to_string(block0.items.size()) +
                         " block1 columns=" + std::to_string(block1.items.size()) +
                         " ts order: secCol=" + std::to_string(secCol) +
                         " nanoCol=" + std::to_string(nanoCol));

        // Determine dimensions
        H5::DataSet block0Ds = dataGroup.openDataSet("block0_values");
        H5::DataSpace block0Space = block0Ds.getSpace();
        hsize_t block0Dims[2]{0, 0};
        block0Space.getSimpleExtentDims(block0Dims);
        const std::size_t totalRows = block0Dims[0];
        const std::size_t numFloatCols = block0Dims[1];

        H5::DataSet block1Ds = dataGroup.openDataSet("block1_values");
        H5::DataSpace block1Space = block1Ds.getSpace();
        hsize_t block1Dims[2]{0, 0};
        block1Space.getSimpleExtentDims(block1Dims);
        const std::size_t numIntCols = block1Dims[1];

        H5::DataSet block2Ds = dataGroup.openDataSet("block2_values");

        const std::size_t chunkSize = config_.chunkSize();
        const std::string sourceName = config_.name();
        const std::size_t totalChunks = (totalRows + chunkSize - 1) / chunkSize;

        logger_->log(util::log::Level::Info,
                     "readFile: totalRows=" + std::to_string(totalRows) +
                         " chunkSize=" + std::to_string(chunkSize) +
                         " totalChunks=" + std::to_string(totalChunks) +
                         " floatCols=" + std::to_string(numFloatCols) +
                         " intCols=" + std::to_string(numIntCols));

        std::size_t chunkIdx = 0;
        for (std::size_t startRow = 0; startRow < totalRows && running_; startRow += chunkSize)
        {
            const std::size_t numRows = std::min(chunkSize, totalRows - startRow);

            // Read timestamps (block2_values)
            std::vector<uint32_t> tsData(numRows * 2);
            {
                hsize_t offset[2] = {startRow, 0};
                hsize_t count[2] = {numRows, 2};
                H5::DataSpace fspace = block2Ds.getSpace();
                fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                H5::DataSpace mspace(2, count);
                block2Ds.read(tsData.data(), H5::PredType::NATIVE_UINT32, mspace, fspace);
            }

            std::vector<TimestampEntry> timestamps(numRows);
            for (std::size_t r = 0; r < numRows; ++r)
            {
                timestamps[r].epoch_seconds = tsData[r * 2 + secCol];
                timestamps[r].nanoseconds = tsData[r * 2 + nanoCol];
            }

            // Read float64 data (block0_values)
            std::vector<double> floatData(numRows * numFloatCols);
            {
                hsize_t offset[2] = {startRow, 0};
                hsize_t count[2] = {numRows, numFloatCols};
                H5::DataSpace fspace = block0Ds.getSpace();
                fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                H5::DataSpace mspace(2, count);
                block0Ds.read(floatData.data(), H5::PredType::NATIVE_DOUBLE, mspace, fspace);
            }

            // Read int16 data (block1_values)
            std::vector<int16_t> intData(numRows * numIntCols);
            {
                hsize_t offset[2] = {startRow, 0};
                hsize_t count[2] = {numRows, numIntCols};
                H5::DataSpace fspace = block1Ds.getSpace();
                fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                H5::DataSpace mspace(2, count);
                block1Ds.read(intData.data(), H5::PredType::NATIVE_INT16, mspace, fspace);
            }

            logger_->log(util::log::Level::Trace,
                         "readFile: emitting chunk " + std::to_string(chunkIdx + 1) + "/" +
                             std::to_string(totalChunks) + " rows=" + std::to_string(numRows));

            emitChunk(sourceName, timestamps, block0, floatData, block1, intData,
                      numRows, numFloatCols, numIntCols);
            ++chunkIdx;
        }

        logger_->log(util::log::Level::Info,
                     "readFile: completed " + std::to_string(chunkIdx) + " chunks, signaling completion");
    }
    catch (const H5::Exception& e)
    {
        if (logger_)
            logger_->log(util::log::Level::Error,
                         std::string("HDF5BsasGen1Reader: HDF5 error: ") + e.getCDetailMsg());
    }
    catch (const std::exception& e)
    {
        if (logger_)
            logger_->log(util::log::Level::Error,
                         std::string("HDF5BsasGen1Reader: ") + e.what());
    }

    signalCompleted();
}

void HDF5BsasGen1Reader::emitChunk(
    const std::string& sourceName,
    const std::vector<TimestampEntry>& timestamps,
    const BlockInfo& block0,
    const std::vector<double>& block0Data,
    const BlockInfo& block1,
    const std::vector<int16_t>& block1Data,
    std::size_t numRows, std::size_t numFloatCols, std::size_t numIntCols)
{
    IDataBus::EventBatch batch;
    batch.metadata = config_.staticMetadata();
    batch.metadata["source"] = sourceName;
    for (const auto& [k, v] : config_.provenance())
        batch.metadata["provenance." + k] = v;
    batch.payload = TimeSeriesPayload{
        .root_source_name = sourceName,
        .is_tabular = true};

    auto& tsp = std::get<TimeSeriesPayload>(batch.payload);

    // Emit one frame per float64 column
    for (std::size_t c = 0; c < numFloatCols && c < block0.items.size(); ++c)
    {
        DataBatch frame;
        frame.timestamps = timestamps;

        DataColumn col;
        col.name = block0.items[c];
        std::vector<double> values(numRows);
        for (std::size_t r = 0; r < numRows; ++r)
            values[r] = block0Data[r * numFloatCols + c];
        col.values = std::move(values);
        frame.columns.push_back(std::move(col));

        tsp.frames.push_back(std::move(frame));
    }

    // Emit one frame per int16 column (as int32)
    for (std::size_t c = 0; c < numIntCols && c < block1.items.size(); ++c)
    {
        DataBatch frame;
        frame.timestamps = timestamps;

        DataColumn col;
        col.name = block1.items[c];
        std::vector<int32_t> values(numRows);
        for (std::size_t r = 0; r < numRows; ++r)
            values[r] = static_cast<int32_t>(block1Data[r * numIntCols + c]);
        col.values = std::move(values);
        frame.columns.push_back(std::move(col));

        tsp.frames.push_back(std::move(frame));
    }

    bus_->push(std::move(batch));

    // Send end_of_batch_group marker
    IDataBus::EventBatch marker;
    marker.metadata["source"] = sourceName;
    marker.payload = TimeSeriesPayload{
        .root_source_name = sourceName,
        .end_of_batch_group = true,
        .is_tabular = true};
    bus_->push(std::move(marker));
}

} // namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1
