//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#ifdef MLDP_PVXS_HDF5_ENABLED

#include <writer/hdf5/HDF5Writer.h>

#include <util/log/Logger.h>

#include <stdexcept>
#include <vector>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::log;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

HDF5Writer::HDF5Writer(HDF5WriterConfig config)
    : config_(std::move(config))
{
}

HDF5Writer::~HDF5Writer()
{
    if (!stopping_.load())
    {
        stop();
    }
}

// ---------------------------------------------------------------------------
// IWriter lifecycle
// ---------------------------------------------------------------------------

void HDF5Writer::start()
{
    stopping_.store(false);
    pool_ = std::make_unique<HDF5FilePool>(config_);

    writerThread_ = std::thread([this] { writerLoop(); });
    flushThread_  = std::thread([this] { flushLoop(); });
}

void HDF5Writer::stop() noexcept
{
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        stopping_.store(true);
    }
    queueCv_.notify_all();

    if (writerThread_.joinable()) { try { writerThread_.join(); } catch (...) {} }
    if (flushThread_.joinable())  { try { flushThread_.join();  } catch (...) {} }

    if (pool_)
    {
        pool_->closeAll();
        pool_.reset();
    }
}

// ---------------------------------------------------------------------------
// push — enqueue into bounded MPSC queue
// ---------------------------------------------------------------------------

bool HDF5Writer::push(util::bus::IDataBus::EventBatch batch) noexcept
{
    if (stopping_.load())
    {
        return false;
    }
    std::lock_guard<std::mutex> lk(queueMutex_);
    if (queue_.size() >= kQueueCapacity)
    {
        return false;   // back-pressure: queue full
    }
    queue_.push_back(std::move(batch));
    queueCv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Writer thread
// ---------------------------------------------------------------------------

void HDF5Writer::writerLoop()
{
    while (true)
    {
        EventBatch batch;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this] { return !queue_.empty() || stopping_.load(); });
            if (queue_.empty())
            {
                break;   // stopping and queue drained
            }
            batch = std::move(queue_.front());
            queue_.pop_front();
        }

        if (!pool_)
        {
            continue;
        }

        for (const auto& frame : batch.frames)
        {
            try
            {
                auto entry = pool_->acquire(batch.root_source);
                const std::size_t sizeBefore = static_cast<std::size_t>(entry->file.getFileSize());
                appendFrame(batch.root_source, frame, entry->file);
                const std::size_t sizeAfter  = static_cast<std::size_t>(entry->file.getFileSize());
                if (sizeAfter > sizeBefore)
                {
                    pool_->recordWrite(batch.root_source,
                                       static_cast<uint64_t>(sizeAfter - sizeBefore));
                }
            }
            catch (const std::exception& ex)
            {
                // Log but do not abort; HDF5 errors are non-fatal for the bus
                (void)ex; // suppress unused-variable warning when logging is stripped
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Flush thread
// ---------------------------------------------------------------------------

void HDF5Writer::flushLoop()
{
    while (!stopping_.load())
    {
        std::this_thread::sleep_for(config_.flushInterval);
        if (pool_)
        {
            pool_->flushAll();
        }
    }
    // Final flush before exit
    if (pool_)
    {
        pool_->flushAll();
    }
}

// ---------------------------------------------------------------------------
// appendFrame — write one DataFrame into an open H5File
// ---------------------------------------------------------------------------

static constexpr hsize_t kChunkSize = 1024;

H5::DataSet HDF5Writer::ensureDataset(H5::H5File&        file,
                                       const std::string& name,
                                       const H5::DataType& dtype)
{
    if (file.nameExists(name))
    {
        return file.openDataSet(name);
    }

    // Chunked unlimited dataset
    hsize_t      dims[1] = {0};
    hsize_t      maxDims[1] = {H5S_UNLIMITED};
    H5::DataSpace space(1, dims, maxDims);

    hsize_t      chunkDims[1] = {kChunkSize};
    H5::DSetCreatPropList props;
    props.setChunk(1, chunkDims);
    if (config_.compressionLevel > 0)
    {
        props.setDeflate(config_.compressionLevel);
    }

    return file.createDataSet(name, dtype, space, props);
}

void HDF5Writer::appendFrame(const std::string&                    sourceName,
                              const dp::service::common::DataFrame& frame,
                              H5::H5File&                           file)
{
    // -- timestamps dataset --
    if (!frame.has_datatimestamps() ||
        !frame.datatimestamps().has_timestamplist())
    {
        return;
    }
    const auto& tslist = frame.datatimestamps().timestamplist();
    const int tsCount = tslist.timestamps_size();
    if (tsCount <= 0)
    {
        return;
    }

    // Build a nanoseconds-since-epoch vector from the timestamp list
    std::vector<int64_t> nsVec;
    nsVec.reserve(static_cast<std::size_t>(tsCount));
    for (int i = 0; i < tsCount; ++i)
    {
        const auto& ts = tslist.timestamps(i);
        nsVec.push_back(
            static_cast<int64_t>(ts.epochseconds()) * 1'000'000'000LL +
            static_cast<int64_t>(ts.nanoseconds()));
    }

    {
        auto ds = ensureDataset(file, "timestamps", H5::PredType::NATIVE_INT64);
        hsize_t curDims[1] = {0};
        hsize_t maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(tsCount);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(tsCount)};
        hsize_t count[1]  = {static_cast<hsize_t>(tsCount)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        hsize_t mDims[1] = {static_cast<hsize_t>(tsCount)};
        H5::DataSpace mspace(1, mDims);
        ds.write(nsVec.data(), H5::PredType::NATIVE_INT64, mspace, fspace);
    }

    // -- double columns --
    for (const auto& col : frame.doublecolumns())
    {
        if (col.name().empty()) { continue; }
        const int n = col.values_size();
        if (n <= 0) { continue; }
        auto ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_DOUBLE);
        hsize_t curDims[1] = {0};
        hsize_t maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1]  = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        hsize_t mDims[1] = {static_cast<hsize_t>(n)};
        H5::DataSpace mspace(1, mDims);
        std::vector<double> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_DOUBLE, mspace, fspace);
    }

    // -- float columns --
    for (const auto& col : frame.floatcolumns())
    {
        if (col.name().empty()) { continue; }
        const int n = col.values_size();
        if (n <= 0) { continue; }
        auto ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_FLOAT);
        hsize_t curDims[1] = {0};
        hsize_t maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1]  = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        hsize_t mDims[1] = {static_cast<hsize_t>(n)};
        H5::DataSpace mspace(1, mDims);
        std::vector<float> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_FLOAT, mspace, fspace);
    }

    // -- int32 columns --
    for (const auto& col : frame.int32columns())
    {
        if (col.name().empty()) { continue; }
        const int n = col.values_size();
        if (n <= 0) { continue; }
        auto ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_INT32);
        hsize_t curDims[1] = {0};
        hsize_t maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1]  = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        hsize_t mDims[1] = {static_cast<hsize_t>(n)};
        H5::DataSpace mspace(1, mDims);
        std::vector<int32_t> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_INT32, mspace, fspace);
    }

    // -- int64 columns --
    for (const auto& col : frame.int64columns())
    {
        if (col.name().empty()) { continue; }
        const int n = col.values_size();
        if (n <= 0) { continue; }
        auto ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_INT64);
        hsize_t curDims[1] = {0};
        hsize_t maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1]  = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        hsize_t mDims[1] = {static_cast<hsize_t>(n)};
        H5::DataSpace mspace(1, mDims);
        std::vector<int64_t> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_INT64, mspace, fspace);
    }

    // -- bool columns --
    for (const auto& col : frame.boolcolumns())
    {
        if (col.name().empty()) { continue; }
        const int n = col.values_size();
        if (n <= 0) { continue; }
        auto ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_HBOOL);
        hsize_t curDims[1] = {0};
        hsize_t maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1]  = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        hsize_t mDims[1] = {static_cast<hsize_t>(n)};
        H5::DataSpace mspace(1, mDims);
        // protobuf bool is stored as int; HDF5 HBOOL is typedef to unsigned int
        std::vector<unsigned int> buf;
        buf.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) { buf.push_back(col.values(i) ? 1u : 0u); }
        ds.write(buf.data(), H5::PredType::NATIVE_HBOOL, mspace, fspace);
    }
}

#endif // MLDP_PVXS_HDF5_ENABLED
