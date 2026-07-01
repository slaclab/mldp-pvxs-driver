//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file HDF5BsasGen1Reader.cpp
 * @brief Implementation of the BSAS Gen1 HDF5 file reader.
 *
 * ## Reading flow
 *
 * ### Multi-file glob resolution
 * 1. The configured @c file_path glob string is expanded via FSUtil::findFilesByGlob(),
 *    yielding an ordered set of filesystem paths.
 * 2. If no files match, an exception is thrown and an error metric recorded.
 * 3. Each matched file is processed sequentially in path-sorted order within
 *    a single worker thread. Per-file metrics use the **file name** (basename)
 *    as the Prometheus @c source label.
 *
 * ### Single-file processing
 * For each HDF5 file the following steps execute:
 * 1. **Open** — the file is opened read-only via H5::H5File.
 * 2. **Dataset discovery** — all root-level datasets are enumerated.
 *    - @c secondsPastEpoch and @c nanoseconds are identified as timestamp vectors;
 *      the row count is determined from @c secondsPastEpoch dimensions.
 *    - Remaining datasets are classified by HDF5 data type: float64 (8-byte float)
 *      or int16 (2-byte integer). Other types are silently skipped.
 *    - Each qualifying dataset becomes a ColumnInfo entry; the HDF5 "label"
 *      attribute is read and UTF-8–sanitized for use as a human-readable name.
 * 3. **Column sorting** — columns are stable-sorted with float64 first, then int16,
 *    preserving discovery order within each type group.
 * 4. **Name normalization** — dataset names are sanitized (control chars → underscore).
 * 5. **Chunked reading** — the file is read in row-chunks of configurable size:
 *    - Timestamp vectors (sec + nano) are read via hyperslab selection.
 *    - Float64 columns are read into a contiguous column-major buffer.
 *    - Int16 columns are read into a separate contiguous column-major buffer.
 *    - Byte metrics are accumulated per chunk.
 *    - emitChunk() assembles one TimeSeriesPayload per chunk and pushes it
 *      onto the data bus, followed by an end_of_batch_group marker.
 * 6. **File summary** — after all chunks, total bytes, processing time (ms),
 *    and throughput (bytes/s) are recorded as Prometheus metrics.
 * 7. **Completion** — after all files, signalCompleted() notifies downstream.
 */

#include <reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.h>

#include <H5Cpp.h>
#include <metrics/Metrics.h>
#include <util/fs/FSUtil.h>
#include <util/log/ILog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1;
using namespace mldp_pvxs_driver::util::fsutil;
using namespace mldp_pvxs_driver::util::bus;

namespace {

    bool isContinuationByte(unsigned char byte)
    {
        return (byte & 0xC0) == 0x80;
    }

    bool isValidUtf8(const std::string& input)
    {
        const auto* p = reinterpret_cast<const unsigned char*>(input.data());
        const auto* end = p + input.size();
        while (p < end)
        {
            if (*p < 0x80)
            {
                ++p;
            }
            else if ((*p & 0xE0) == 0xC0 && p + 1 < end && isContinuationByte(p[1]))
            {
                if (*p < 0xC2)
                {
                    return false;
                }
                p += 2;
            }
            else if ((*p & 0xF0) == 0xE0 && p + 2 < end &&
                     isContinuationByte(p[1]) && isContinuationByte(p[2]))
            {
                const auto ch = static_cast<uint32_t>((*p & 0x0F) << 12 |
                                                      (p[1] & 0x3F) << 6 |
                                                      (p[2] & 0x3F));
                if (ch < 0x800 || (ch >= 0xD800 && ch <= 0xDFFF))
                {
                    return false;
                }
                p += 3;
            }
            else if ((*p & 0xF8) == 0xF0 && p + 3 < end &&
                     isContinuationByte(p[1]) && isContinuationByte(p[2]) &&
                     isContinuationByte(p[3]))
            {
                const auto ch = static_cast<uint32_t>((*p & 0x07) << 18 |
                                                      (p[1] & 0x3F) << 12 |
                                                      (p[2] & 0x3F) << 6 |
                                                      (p[3] & 0x3F));
                if (ch < 0x10000 || ch > 0x10FFFF)
                {
                    return false;
                }
                p += 4;
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    std::string sanitizeUtf8(const std::string& input)
    {
        if (isValidUtf8(input))
        {
            return input;
        }

        std::string out;
        out.reserve(input.size());
        const auto* p = reinterpret_cast<const unsigned char*>(input.data());
        const auto* end = p + input.size();
        while (p < end)
        {
            if (*p >= 0x20 && *p < 0x7F)
            {
                out.push_back(static_cast<char>(*p));
            }
            else if (*p == ' ')
            {
                out.push_back('_');
            }
            ++p;
        }
        return out;
    }

    std::string normalizeColumnName(const std::string& candidate,
                                    const std::string& fallback)
    {
        std::string name = sanitizeUtf8(candidate);
        if (name.empty())
        {
            name = sanitizeUtf8(fallback);
        }

        for (char& ch : name)
        {
            const auto uch = static_cast<unsigned char>(ch);
            if (std::iscntrl(uch))
            {
                ch = '_';
            }
        }

        if (name.empty())
        {
            name = "unnamed_column";
        }

        return name;
    }

    std::unordered_map<std::string, std::string> readAllStringAttrs(H5::H5Object& obj)
    {
        std::unordered_map<std::string, std::string> result;
        const int numAttrs = obj.getNumAttrs();
        for (int i = 0; i < numAttrs; ++i)
        {
            H5::Attribute attr = obj.openAttribute(static_cast<unsigned int>(i));
            H5::DataType  dtype = attr.getDataType();
            if (dtype.getClass() != H5T_STRING)
                continue;
            std::string attrName = attr.getName();
            H5::StrType strType = attr.getStrType();
            std::string value;
            if (strType.isVariableStr())
            {
                attr.read(strType, value);
            }
            else
            {
                value.resize(strType.getSize());
                attr.read(strType, value.data());
            }
            auto end = value.find('\0');
            if (end != std::string::npos)
                value.resize(end);
            result[attrName] = sanitizeUtf8(value);
        }
        return result;
    }

} // anonymous namespace

HDF5BsasGen1Reader::HDF5BsasGen1Reader(
    std::shared_ptr<IDataBus>         bus,
    std::shared_ptr<metrics::Metrics> metrics,
    const config::Config&             cfg)
    : Reader(std::move(bus), std::move(metrics))
    , config_(cfg)
{
    provenance_ = config_.provenance();
    logger_ = util::log::newLogger("hdf5_bsas_gen1_reader");
    running_ = true;
    worker_ = std::thread([this]()
                          {
                              readFile();
                          });
}

HDF5BsasGen1Reader::~HDF5BsasGen1Reader()
{
    running_ = false;
    if (worker_.joinable())
        worker_.join();
}

// readFile() — Data flow sequence:
//
// 1. Resolve glob → ordered file list.
// 2. For each file:
//    a. Open HDF5 read-only, discover datasets (timestamps + data columns).
//    b. Sort columns: float64 first, then int16.
//    c. Read in row-chunks (configurable size):
//       - Read timestamps (sec + nano) via hyperslab.
//       - Read float64 columns into contiguous column-major buffer.
//       - Read int16 columns into contiguous column-major buffer.
//       - Call emitChunk() → push data + end_of_batch marker onto bus.
//    d. If emitChunk() returns false (stopped or backpressure exhausted),
//       break immediately — no further data is read or pushed.
//    e. After all chunks: record per-file metrics (bytes, time, throughput).
// 3. After all files (or early exit): signalCompleted() notifies downstream.
//
// Backpressure: emitChunk() handles retry; this loop never discards data.
// Fast exit: if running_ becomes false (controller stop), both inner chunk
// loop and outer file loop terminate without pushing buffered data.
void HDF5BsasGen1Reader::readFile()
{
    std::string current_file_name;
    try
    {
        // --- Phase 1: Glob resolution ---
        const auto files = FSUtil::findFilesByGlob(config_.filePath());
        if (files.empty())
        {
            throw std::runtime_error("no files matched configured file-path: " + config_.filePath());
        }

        logger_->log(util::log::Level::Debug,
                     "readFile: matched " + std::to_string(files.size()) + " file(s) for " +
                         config_.filePath());

        // --- Phase 2: Sequential file processing ---
        for (const auto& filePath : files)
        {
            current_file_name = filePath.filename().string();
            logger_->log(util::log::Level::Trace,
                         "readFile: opening " + filePath.string());
            H5::H5File file(filePath.string(), H5F_ACC_RDONLY);

            // --- Phase 3: Dataset discovery ---
            // Enumerate root-level datasets. Identify timestamp vectors
            // (secondsPastEpoch, nanoseconds) and classify data columns
            // by HDF5 type (float64 or int16). Other types are skipped.
            std::vector<ColumnInfo> columns;
            std::size_t             totalRows = 0;
            bool                    hasSeconds = false;
            bool                    hasNanos = false;

            const hsize_t numObjects = file.getNumObjs();
            for (hsize_t i = 0; i < numObjects; ++i)
            {
                if (file.getObjTypeByIdx(i) != H5G_DATASET)
                    continue;

                std::string dsName = file.getObjnameByIdx(i);

                if (dsName == "secondsPastEpoch")
                {
                    hasSeconds = true;
                    H5::DataSet   ds = file.openDataSet(dsName);
                    H5::DataSpace sp = ds.getSpace();
                    hsize_t       dims[2]{0, 0};
                    sp.getSimpleExtentDims(dims);
                    totalRows = dims[0];
                    continue;
                }
                if (dsName == "nanoseconds")
                {
                    hasNanos = true;
                    continue;
                }

                H5::DataSet  ds = file.openDataSet(dsName);
                H5::DataType dtype = ds.getDataType();

                ColumnInfo col;
                col.name = dsName;
                col.metadata = readAllStringAttrs(ds);

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

            // --- Phase 4: Column sorting ---
            // Float64 columns ordered before int16; discovery order preserved within each group.
            std::stable_sort(columns.begin(), columns.end(),
                             [](const ColumnInfo& a, const ColumnInfo& b)
                             {
                                 return static_cast<int>(a.type) < static_cast<int>(b.type);
                             });

            std::size_t numFloatCols = 0;
            std::size_t numIntCols = 0;
            for (const auto& col : columns)
            {
                if (col.type == ColumnInfo::Type::Float64)
                    ++numFloatCols;
                else
                    ++numIntCols;
            }

            const std::size_t chunkSize = config_.chunkSize();
            const std::string sourceName = config_.name();
            
            const std::string fileName = filePath.filename().string();
            const prometheus::Labels source_tag{{"source", fileName}};
            const auto file_start = std::chrono::steady_clock::now();
            std::size_t file_total_bytes = 0;
            const std::size_t totalChunks = (totalRows + chunkSize - 1) / chunkSize;

            logger_->log(util::log::Level::Info,
                         "readFile: totalRows=" + std::to_string(totalRows) +
                             " chunkSize=" + std::to_string(chunkSize) +
                             " totalChunks=" + std::to_string(totalChunks) +
                             " floatCols=" + std::to_string(numFloatCols) +
                             " intCols=" + std::to_string(numIntCols));

            // --- Phase 5: Dataset handle caching & name normalization ---
            // Open all HDF5 datasets once (avoids repeated open/close per chunk).
            // Normalize column names (replace control chars with underscore).
            H5::DataSet secDs = file.openDataSet("secondsPastEpoch");
            H5::DataSet nanoDs = file.openDataSet("nanoseconds");

            std::vector<H5::DataSet> colDatasets;
            colDatasets.reserve(columns.size());
            for (const auto& col : columns)
                colDatasets.push_back(file.openDataSet(col.name));

            for (auto& col : columns)
            {
                col.name = normalizeColumnName(col.name, col.name);
            }

            // --- Phase 6: Chunked reading loop ---
            // Iterate row-chunks of size chunkSize. Each iteration:
            //   a) Hyperslab-read timestamp vectors (sec + nano) for this row range.
            //   b) Hyperslab-read all float64 columns into one contiguous buffer (column-major).
            //   c) Hyperslab-read all int16 columns into a separate buffer (column-major).
            //   d) emitChunk() packages data into TimeSeriesPayload and pushes to bus.
            std::size_t chunkIdx = 0;
            auto lastProgressLog = std::chrono::steady_clock::time_point{};
            const auto logInterval = std::chrono::seconds(config_.logIntervalSec());
            for (std::size_t startRow = 0; startRow < totalRows && running_; startRow += chunkSize)
            {
                const std::size_t numRows = std::min(chunkSize, totalRows - startRow);

                // Read timestamps
                std::vector<uint32_t> secData(numRows);
                std::vector<uint32_t> nanoData(numRows);
                {
                    hsize_t       offset[2] = {startRow, 0};
                    hsize_t       count[2] = {numRows, 1};
                    H5::DataSpace fspace = secDs.getSpace();
                    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                    hsize_t       mdims[1] = {numRows};
                    H5::DataSpace mspace(1, mdims);
                    secDs.read(secData.data(), H5::PredType::NATIVE_UINT32, mspace, fspace);
                }
                {
                    hsize_t       offset[2] = {startRow, 0};
                    hsize_t       count[2] = {numRows, 1};
                    H5::DataSpace fspace = nanoDs.getSpace();
                    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                    hsize_t       mdims[1] = {numRows};
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
                    hsize_t       offset[2] = {startRow, 0};
                    hsize_t       count[2] = {numRows, 1};
                    H5::DataSpace fspace = colDatasets[c].getSpace();
                    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                    hsize_t       mdims[1] = {numRows};
                    H5::DataSpace mspace(1, mdims);
                    colDatasets[c].read(floatData.data() + c * numRows,
                                        H5::PredType::NATIVE_DOUBLE, mspace, fspace);
                }

                // Read int16 columns
                std::vector<int16_t> intData(numRows * numIntCols);
                for (std::size_t c = 0; c < numIntCols; ++c)
                {
                    hsize_t       offset[2] = {startRow, 0};
                    hsize_t       count[2] = {numRows, 1};
                    H5::DataSpace fspace = colDatasets[numFloatCols + c].getSpace();
                    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
                    hsize_t       mdims[1] = {numRows};
                    H5::DataSpace mspace(1, mdims);
                    colDatasets[numFloatCols + c].read(intData.data() + c * numRows,
                                                       H5::PredType::NATIVE_INT16, mspace, fspace);
                }

                logger_->log(util::log::Level::Trace,
                             "readFile: emitting chunk " + std::to_string(chunkIdx + 1) + "/" +
                                 std::to_string(totalChunks) + " rows=" + std::to_string(numRows));

                const std::size_t chunk_bytes =
                    numRows * numFloatCols * sizeof(double) +
                    numRows * numIntCols * sizeof(int16_t) +
                    numRows * 2 * sizeof(uint32_t);
                file_total_bytes += chunk_bytes;

                if (!emitChunk(sourceName, filePath.string(), timestamps, columns, floatData, intData,
                               numRows, numFloatCols, numIntCols))
                {
                    logger_->log(util::log::Level::Debug,
                                 "readFile: emitChunk returned false at chunk " + std::to_string(chunkIdx + 1) +
                                     "/" + std::to_string(totalChunks) + " for " + fileName +
                                     " — running_=" + (running_ ? "true" : "false"));
                    break;
                }

                metric_call(metrics_, [&](auto& m)
                            {
                                m.incrementReaderEvents(1.0, source_tag);
                                m.incrementReaderEventsReceived(static_cast<double>(numRows), source_tag);
                            });
                ++chunkIdx;

                const auto now = std::chrono::steady_clock::now();
                const std::size_t processedRows = startRow + numRows;
                if (chunkIdx == 1 || now - lastProgressLog >= logInterval)
                {
                    logger_->log(util::log::Level::Info,
                                 "readFile: " + fileName +
                                     " — processed " + std::to_string(processedRows) +
                                     " row(s), " + std::to_string(totalRows - processedRows) + " remaining");
                    lastProgressLog = now;
                }
            }

            if (!running_)
            {
                logger_->log(util::log::Level::Debug,
                             "readFile: running_ false after chunk loop — aborting file scan at " + fileName);
                break;
            }

            if (chunkIdx > 0)
            {
                logger_->log(util::log::Level::Info,
                             "readFile: " + fileName +
                                 " — processed " + std::to_string(totalRows) +
                                 " row(s), 0 remaining");
            }

            // --- Phase 7: Per-file metrics ---
            // Record total bytes read, wall-clock processing time, and throughput.
            const auto file_end = std::chrono::steady_clock::now();
            const double file_ms = std::chrono::duration<double, std::milli>(file_end - file_start).count();

            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementReaderDataBytesTotal(static_cast<double>(file_total_bytes), source_tag);
                            m.observeReaderProcessingTimeMs(file_ms, source_tag);
                        });
            if (file_ms > 0.0)
            {
                const double bps = (static_cast<double>(file_total_bytes) * 1000.0) / file_ms;
                metric_call(metrics_, [&](auto& m)
                            {
                                m.setReaderDataBytesPerSecond(bps, source_tag);
                            });
            }

            logger_->log(util::log::Level::Info,
                         "readFile: completed " + std::to_string(chunkIdx) +
                             " chunks for " + filePath.string());
        }
    }
    catch (const H5::Exception& e)
    {
        const std::string err_source = current_file_name.empty() ? std::string("unknown") : current_file_name;
        const prometheus::Labels err_tag{{"source", err_source}};
        metric_call(metrics_, [&](auto& m) { m.incrementReaderErrors(1.0, err_tag); });
        if (logger_)
            logger_->log(util::log::Level::Error,
                         std::string("HDF5BsasGen1Reader: HDF5 error: ") + e.getCDetailMsg());
    }
    catch (const std::exception& e)
    {
        const std::string err_source = current_file_name.empty() ? std::string("unknown") : current_file_name;
        const prometheus::Labels err_tag{{"source", err_source}};
        metric_call(metrics_, [&](auto& m) { m.incrementReaderErrors(1.0, err_tag); });
        if (logger_)
            logger_->log(util::log::Level::Error,
                         std::string("HDF5BsasGen1Reader: ") + e.what());
    }

    logger_->log(util::log::Level::Debug, "readFile: all files processed — signaling completed");
    signalCompleted();
}

// emitChunk() — Push sequence:
//
// 1. Build data batch (all float64 + int16 frames for this chunk).
// 2. Push data batch onto bus via pushWithRetry():
//    a. Check running_ — bail immediately if controller stopped us.
//    b. Attempt bus_->push(). If accepted → done.
//    c. If rejected (queue full): sleep 10 ms, check running_ again,
//       rebuild batch, retry once.
//    d. If retry also fails → return false (caller breaks read loop).
// 3. Build end_of_batch_group marker.
// 4. Push marker via same pushWithRetry() logic.
// 5. Return true only if both pushes succeeded.
//
// Guarantee: while running_ is true, data is never discarded — the method
// waits for space rather than dropping. Only a controller-initiated stop
// causes an early return without delivery.
bool HDF5BsasGen1Reader::emitChunk(
    const std::string&                 sourceName,
    const std::string&                 currentFile,
    const std::vector<TimestampEntry>& timestamps,
    const std::vector<ColumnInfo>&     columns,
    const std::vector<double>&         floatData,
    const std::vector<int16_t>&        intData,
    std::size_t                        numRows,
    std::size_t                        numFloatCols,
    std::size_t                        numIntCols)
{
    auto buildDataBatch = [&]()
    {
        IDataBus::EventBatch batch;
        batch.reader_name = name();
        batch.metadata = config_.staticMetadata();
        batch.metadata.insert(provenance().begin(), provenance().end());
        batch.metadata["source"] = sourceName;
        batch.metadata["file"] = currentFile;
        for (const auto& [k, v] : provenance())
            batch.metadata["provenance." + k] = v;
        batch.payload = TimeSeriesPayload{
            .root_source_name = sourceName,
            .is_tabular = true};

        auto& tsp = std::get<TimeSeriesPayload>(batch.payload);

        for (std::size_t c = 0; c < numFloatCols; ++c)
        {
            DataBatch frame;
            frame.timestamps = timestamps;
            DataColumn col;
            col.name = columns[c].name;
            col.metadata = columns[c].metadata;
            std::vector<double> values(numRows);
            for (std::size_t r = 0; r < numRows; ++r)
                values[r] = floatData[c * numRows + r];
            col.values = std::move(values);
            frame.columns.push_back(std::move(col));
            tsp.frames.push_back(std::move(frame));
        }

        for (std::size_t c = 0; c < numIntCols; ++c)
        {
            DataBatch frame;
            frame.timestamps = timestamps;
            DataColumn col;
            col.name = columns[numFloatCols + c].name;
            col.metadata = columns[numFloatCols + c].metadata;
            std::vector<int32_t> values(numRows);
            for (std::size_t r = 0; r < numRows; ++r)
                values[r] = static_cast<int32_t>(intData[c * numRows + r]);
            col.values = std::move(values);
            frame.columns.push_back(std::move(col));
            tsp.frames.push_back(std::move(frame));
        }

        return batch;
    };

    auto buildMarker = [&]()
    {
        IDataBus::EventBatch marker;
        marker.reader_name = name();
        marker.metadata.insert(provenance().begin(), provenance().end());
        marker.metadata["source"] = sourceName;
        for (const auto& [k, v] : provenance())
            marker.metadata["provenance." + k] = v;
        marker.payload = TimeSeriesPayload{
            .root_source_name = sourceName,
            .end_of_batch_group = true,
            .is_tabular = true};
        return marker;
    };

    auto pushWithRetry = [&](const char* label, auto&& builder) -> bool
    {
        bool blocked = false;
        while (running_)
        {
            auto event = builder();
            if (bus_->push(std::move(event)))
            {
                if (blocked)
                    logger_->log(util::log::Level::Debug,
                                 std::string("emitChunk: ") + label + " push unblocked for source '" + sourceName + "'");
                return true;
            }
            if (!blocked)
            {
                blocked = true;
                logger_->log(util::log::Level::Debug,
                             std::string("emitChunk: ") + label + " push blocked (bus full) for source '" + sourceName + "'");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        logger_->log(util::log::Level::Debug,
                     std::string("emitChunk: ") + label + " push aborted — running_ false for source '" + sourceName + "'");
        return false;
    };

    if (!pushWithRetry("data_batch", buildDataBatch))
        return false;
    if (!pushWithRetry("marker", buildMarker))
        return false;
    return true;
}
