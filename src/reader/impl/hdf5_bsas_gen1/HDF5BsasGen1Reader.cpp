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
#include <util/fs/FSUtil.h>
#include <util/log/ILog.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <stdexcept>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1 {

using namespace util::bus;

namespace {

using mldp_pvxs_driver::util::fsutil::FSUtil;

std::string readStringAttr(H5::H5Object& obj, const char* name)
{
    if (!obj.attrExists(name))
        return {};
    H5::Attribute attr = obj.openAttribute(name);
    H5::StrType strType = attr.getStrType();
    std::string value;
    value.resize(strType.getSize());
    attr.read(strType, value.data());
    auto end = value.find('\0');
    if (end != std::string::npos)
        value.resize(end);
    return value;
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
        const auto files = FSUtil::findFilesByGlob(config_.filePath());
        if (files.empty())
        {
            throw std::runtime_error("no files matched configured file-path: " + config_.filePath());
        }

        logger_->log(util::log::Level::Trace,
                     "readFile: matched " + std::to_string(files.size()) + " file(s) for " +
                         config_.filePath());

        for (const auto& filePath : files)
        {
            logger_->log(util::log::Level::Trace,
                         "readFile: opening " + filePath.string());
            H5::H5File file(filePath.string(), H5F_ACC_RDONLY);

            // Discover datasets at root level
            std::vector<ColumnInfo> columns;
            std::size_t totalRows = 0;
            bool hasSeconds = false;
            bool hasNanos = false;

            const hsize_t numObjects = file.getNumObjs();
            for (hsize_t i = 0; i < numObjects; ++i)
            {
                if (file.getObjTypeByIdx(i) != H5G_DATASET)
                    continue;

                std::string dsName = file.getObjnameByIdx(i);

                if (dsName == "secondsPastEpoch")
                {
                    hasSeconds = true;
                    H5::DataSet ds = file.openDataSet(dsName);
                    H5::DataSpace sp = ds.getSpace();
                    hsize_t dims[2]{0, 0};
                    sp.getSimpleExtentDims(dims);
                    totalRows = dims[0];
                    continue;
                }
                if (dsName == "nanoseconds")
                {
                    hasNanos = true;
                    continue;
                }

                H5::DataSet ds = file.openDataSet(dsName);
                H5::DataType dtype = ds.getDataType();
                std::string label = readStringAttr(ds, "label");

                ColumnInfo col;
                col.name = dsName;
                col.label = label.empty() ? dsName : label;

                if (dtype.getClass() == H5T_FLOAT && dtype.getSize() == 8)
                    col.type = ColumnInfo::Type::Float64;
                else if (dtype.getClass() == H5T_INTEGER && dtype.getSize() == 2)
                    col.type = ColumnInfo::Type::Int16;
                else
                    continue;

                columns.push_back(std::move(col));
            }

            if (!hasSeconds || !hasNanos)
                throw std::runtime_error("missing secondsPastEpoch/nanoseconds in " + filePath.string());

            // Sort: float64 first, then int16 (stable order by discovery)
            std::stable_sort(columns.begin(), columns.end(),
                [](const ColumnInfo& a, const ColumnInfo& b) {
                    return static_cast<int>(a.type) < static_cast<int>(b.type);
                });

            std::size_t numFloatCols = 0;
            std::size_t numIntCols = 0;
            for (const auto& col : columns)
            {
                if (col.type == ColumnInfo::Type::Float64) ++numFloatCols;
                else ++numIntCols;
            }

            const std::size_t chunkSize = config_.chunkSize();
            const std::string sourceName = config_.name();
            const std::size_t totalChunks = (totalRows + chunkSize - 1) / chunkSize;
            const bool useLabel = config_.useLabelAsName();

            logger_->log(util::log::Level::Info,
                         "readFile: totalRows=" + std::to_string(totalRows) +
                             " chunkSize=" + std::to_string(chunkSize) +
                             " totalChunks=" + std::to_string(totalChunks) +
                             " floatCols=" + std::to_string(numFloatCols) +
                             " intCols=" + std::to_string(numIntCols));

            // Open all datasets once
            H5::DataSet secDs = file.openDataSet("secondsPastEpoch");
            H5::DataSet nanoDs = file.openDataSet("nanoseconds");

            std::vector<H5::DataSet> colDatasets;
            colDatasets.reserve(columns.size());
            for (const auto& col : columns)
                colDatasets.push_back(file.openDataSet(col.name));

            // Build column info with resolved names
            std::vector<ColumnInfo> resolvedColumns = columns;
            if (useLabel)
            {
                for (auto& col : resolvedColumns)
                    col.name = col.label;
            }

            std::size_t chunkIdx = 0;
            for (std::size_t startRow = 0; startRow < totalRows && running_; startRow += chunkSize)
            {
                const std::size_t numRows = std::min(chunkSize, totalRows - startRow);

                // Read timestamps
                std::vector<uint32_t> secData(numRows);
                std::vector<uint32_t> nanoData(numRows);
                {
                    hsize_t offset[2] = {startRow, 0};
                    hsize_t count[2] = {numRows, 1};
                    H5::DataSpace fspace = secDs.getSpace();
                    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                    hsize_t mdims[1] = {numRows};
                    H5::DataSpace mspace(1, mdims);
                    secDs.read(secData.data(), H5::PredType::NATIVE_UINT32, mspace, fspace);
                }
                {
                    hsize_t offset[2] = {startRow, 0};
                    hsize_t count[2] = {numRows, 1};
                    H5::DataSpace fspace = nanoDs.getSpace();
                    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                    hsize_t mdims[1] = {numRows};
                    H5::DataSpace mspace(1, mdims);
                    nanoDs.read(nanoData.data(), H5::PredType::NATIVE_UINT32, mspace, fspace);
                }

                std::vector<TimestampEntry> timestamps(numRows);
                for (std::size_t r = 0; r < numRows; ++r)
                {
                    timestamps[r].epoch_seconds = secData[r];
                    timestamps[r].nanoseconds = nanoData[r];
                }

                // Read float64 columns
                std::vector<double> floatData(numRows * numFloatCols);
                for (std::size_t c = 0; c < numFloatCols; ++c)
                {
                    hsize_t offset[2] = {startRow, 0};
                    hsize_t count[2] = {numRows, 1};
                    H5::DataSpace fspace = colDatasets[c].getSpace();
                    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                    hsize_t mdims[1] = {numRows};
                    H5::DataSpace mspace(1, mdims);
                    colDatasets[c].read(floatData.data() + c * numRows,
                                       H5::PredType::NATIVE_DOUBLE, mspace, fspace);
                }

                // Read int16 columns
                std::vector<int16_t> intData(numRows * numIntCols);
                for (std::size_t c = 0; c < numIntCols; ++c)
                {
                    hsize_t offset[2] = {startRow, 0};
                    hsize_t count[2] = {numRows, 1};
                    H5::DataSpace fspace = colDatasets[numFloatCols + c].getSpace();
                    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                    hsize_t mdims[1] = {numRows};
                    H5::DataSpace mspace(1, mdims);
                    colDatasets[numFloatCols + c].read(intData.data() + c * numRows,
                                                      H5::PredType::NATIVE_INT16, mspace, fspace);
                }

                logger_->log(util::log::Level::Trace,
                             "readFile: emitting chunk " + std::to_string(chunkIdx + 1) + "/" +
                                 std::to_string(totalChunks) + " rows=" + std::to_string(numRows));

                emitChunk(sourceName, timestamps, resolvedColumns, floatData, intData,
                          numRows, numFloatCols, numIntCols);
                ++chunkIdx;
            }

            logger_->log(util::log::Level::Info,
                         "readFile: completed " + std::to_string(chunkIdx) +
                             " chunks for " + filePath.string());
        }
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
    const std::vector<ColumnInfo>& columns,
    const std::vector<double>& floatData,
    const std::vector<int16_t>& intData,
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
    for (std::size_t c = 0; c < numFloatCols; ++c)
    {
        DataBatch frame;
        frame.timestamps = timestamps;

        DataColumn col;
        col.name = columns[c].name;
        std::vector<double> values(numRows);
        for (std::size_t r = 0; r < numRows; ++r)
            values[r] = floatData[c * numRows + r];
        col.values = std::move(values);
        frame.columns.push_back(std::move(col));

        tsp.frames.push_back(std::move(frame));
    }

    // Emit one frame per int16 column (as int32)
    for (std::size_t c = 0; c < numIntCols; ++c)
    {
        DataBatch frame;
        frame.timestamps = timestamps;

        DataColumn col;
        col.name = columns[numFloatCols + c].name;
        std::vector<int32_t> values(numRows);
        for (std::size_t r = 0; r < numRows; ++r)
            values[r] = static_cast<int32_t>(intData[c * numRows + r]);
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
