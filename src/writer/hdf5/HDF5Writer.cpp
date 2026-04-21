//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/*
 * HDF5Writer — asynchronous, thread-safe HDF5 file writer
 * =========================================================
 *
 * Overview
 * --------
 * HDF5Writer receives EventBatches from the controller (via push()) and
 * persists them to HDF5 files managed by HDF5FilePool.  The design uses two
 * dedicated background threads to decouple slow disk I/O from the fast data
 * path:
 *
 *   ┌─────────────┐   push()    ┌──────────────┐
 *   │  Controller │ ──────────► │  queue_      │  bounded deque
 *   └─────────────┘             └──────┬───────┘
 *                                      │ (writerThread_)
 *                               writerLoop() drains entire queue per
 *                               wake-up, calls appendFrame() per frame
 *                                      │
 *                               ┌──────▼──────────┐
 *                               │  HDF5FilePool   │  one .hdf5 file per source
 *                               └──────┬──────────┘
 *                                      │ (flushThread_)
 *                               flushLoop() calls flushAll() periodically
 *
 * Thread model
 * ------------
 * Three threads touch shared state:
 *
 *   1. Caller thread(s) — calls push(); holds queueMutex_ briefly to enqueue.
 *   2. writerThread_    — calls writerLoop(); drains queue, calls appendFrame().
 *   3. flushThread_     — calls flushLoop(); periodically flushes open HDF5 files.
 *
 * Locking discipline (must be respected to avoid deadlock):
 *
 *   - queueMutex_     : guards queue_ and stopping_. Held briefly; no HDF5
 *                       calls made while holding it.
 *   - pool mutex_     : internal to HDF5FilePool; guards the source→FileEntry map.
 *   - entry->fileMutex: per-FileEntry; guards the H5::H5File object.
 *
 *   Lock-order rule: pool mutex_ → entry->fileMutex.
 *   NEVER hold fileMutex while calling pool methods (acquire / recordWrite),
 *   as pool methods acquire pool mutex_ internally.
 *
 * Why per-entry fileMutex?
 * ------------------------
 * HDF5 (without the thread-safe library build) is NOT thread-safe.  The
 * writerThread_ and flushThread_ can both access the same H5::H5File
 * simultaneously — writerThread_ via appendFrame() and flushThread_ via
 * file.flush().  Without serialisation this corrupts the HDF5 metadata cache
 * and triggers assertion failures deep in libhdf5 (H5C__flush_ring).
 * The per-entry mutex serialises all access to a single file while still
 * allowing concurrent I/O on files belonging to different sources.
 *
 * Queue drain strategy
 * --------------------
 * writerLoop() swaps the entire queue_ into a local deque under queueMutex_,
 * then releases the lock before doing any I/O.  This means:
 *   - push() is never blocked by slow HDF5 writes.
 *   - A single wake-up processes all accumulated batches, catching up quickly
 *     when the writer falls behind (e.g. after the initial dataset creation
 *     overhead on a new file).
 *
 * appendFrame() per-column write pattern
 * ----------------------------------------
 * For every column type the pattern is identical:
 *   1. ensureDataset[2D]() — open existing or create new chunked dataset.
 *   2. getSpace() + extend() — grow the dataset by N new rows.
 *   3. selectHyperslab()    — select only the newly appended rows in file space.
 *   4. write()              — copy data from memory buffer into that slab.
 *
 * Scalar columns  → 1-D dataset shape (N_total_samples,)
 * Array  columns  → 2-D dataset shape (N_total_samples, array_len)
 *
 * Exception handling
 * ------------------
 * H5::Exception does NOT inherit from std::exception.  All catch blocks
 * therefore explicitly catch H5::Exception first, then std::exception, then
 * (...) to prevent uncaught exceptions from silently killing background threads.
 */

#include <writer/hdf5/HDF5Writer.h>

#include <BS_thread_pool.hpp>
#include <util/log/Logger.h>

#include <cstring>
#include <limits>
#include <vector>

using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::writer;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

HDF5Writer::HDF5Writer(const config::Config& node,
                       std::shared_ptr<metrics::Metrics> /*metrics*/)
    : HDF5Writer(HDF5WriterConfig::parse(node))
{
}

HDF5Writer::HDF5Writer(HDF5WriterConfig config)
    : config_(std::move(config))
    , logger_(util::log::newLogger("hdf5_writer:" + config_.name))
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
    infof(*logger_, "HDF5Writer [{}] starting (output_dir={}, max_file_size_mb={}, flush_interval_ms={})",
          config_.name,
          config_.basePath,
          config_.maxFileSizeMB,
          std::chrono::duration_cast<std::chrono::milliseconds>(config_.flushInterval).count());

    stopping_.store(false);
    pool_ = std::make_unique<HDF5FilePool>(config_);

    writerThread_ = std::thread([this]
                                {
                                    BS::this_thread::set_os_thread_name("hdf5-writer");
                                    writerLoop();
                                });
    flushThread_ = std::thread([this]
                               {
                                   BS::this_thread::set_os_thread_name("hdf5-flush");
                                   flushLoop();
                               });

    infof(*logger_, "HDF5Writer [{}] started — writer and flush threads running", config_.name);
}

void HDF5Writer::stop() noexcept
{
    infof(*logger_, "HDF5Writer [{}] stopping", config_.name);

    // Signal both threads to exit.  The lock ensures stopping_ is visible to
    // writerLoop() when it re-evaluates the condition variable predicate.
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        stopping_.store(true);
    }
    queueCv_.notify_all();

    if (writerThread_.joinable())
    {
        try
        {
            writerThread_.join();
        }
        catch (...)
        {
        }
    }
    if (flushThread_.joinable())
    {
        try
        {
            flushThread_.join();
        }
        catch (...)
        {
        }
    }

    // Close all open HDF5 files after both threads have exited so there is no
    // concurrent access on the pool during shutdown.
    if (pool_)
    {
        pool_->closeAll();
        pool_.reset();
    }
    infof(*logger_, "HDF5Writer [{}] stopped", config_.name);
}

// ---------------------------------------------------------------------------
// push() — enqueue into bounded MPSC queue
// ---------------------------------------------------------------------------

bool HDF5Writer::push(util::bus::IDataBus::EventBatch batch) noexcept
{
    if (stopping_.load())
    {
        debugf(*logger_, "HDF5Writer [{}] push rejected — writer is stopping", config_.name);
        return false;
    }
    const uint64_t              seq = nextBatchSeq_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(queueMutex_);
    if (queue_.size() >= kQueueCapacity)
    {
        // Back-pressure: drop the batch rather than blocking the caller.
        warnf(*logger_, "HDF5Writer [{}] queue full ({} items) — dropping batch", config_.name, queue_.size());
        return false;
    }
    queue_.push_back({seq, std::move(batch)});
    queueCv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// writerLoop() — background writer thread
// ---------------------------------------------------------------------------

void HDF5Writer::writerLoop()
{
    debugf(*logger_, "HDF5Writer [{}] writer thread started", config_.name);
    while (true)
    {
        // --- Phase 1: drain the entire queue under a single lock window -----
        // Swapping into a local deque releases queueMutex_ before any I/O,
        // so push() is never blocked by slow HDF5 operations.
        std::deque<QueueEntry> drained;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this]
                          {
                              return !queue_.empty() || stopping_.load();
                          });
            if (queue_.empty())
            {
                // stopping_ is set and queue is drained — safe to exit.
                debugf(*logger_, "HDF5Writer [{}] writer thread exiting — queue drained", config_.name);
                break;
            }
            drained.swap(queue_); // O(1); queue_ is left empty
        }

        if (!pool_)
        {
            warnf(*logger_, "HDF5Writer [{}] pool not initialised — skipping {} batches", config_.name, drained.size());
            continue;
        }

        // --- Phase 2: write each frame to HDF5 ------------------------------
        // end_of_batch_group marker → flush tabular buffer for that source.
        // Tabular column batches (tags[0] == root_source) → accumulate.
        // All other batches → existing columnar path.
        for (auto& entry : drained)
        {
            try
            {
                if (entry.batch.end_of_batch_group)
                {
                    // Marker: flush whatever is accumulated for this source.
                    const auto& source = entry.batch.root_source;
                    auto        it = tabularBuffers_.find(source);
                    if (it != tabularBuffers_.end() && it->second.rowCount > 0)
                    {
                        auto                        ev = pool_->acquire(source);
                        std::lock_guard<std::mutex> fileLk(ev->fileMutex);
                        flushTabularBuffer(source, it->second, ev->file);
                    }
                }
                else if (isTabularBatch(entry.batch))
                {
                    processTabularBatch(entry);
                }
                else
                {
                    for (const auto& frame : entry.batch.frames)
                    {
                        auto           ev = pool_->acquire(entry.batch.root_source);
                        const uint64_t written = static_cast<uint64_t>(frame.ByteSizeLong());
                        {
                            std::lock_guard<std::mutex> fileLk(ev->fileMutex);
                            appendFrame(entry.batch.root_source, frame, ev->file, entry.batchSeq);
                        }
                        tracef(*logger_, "HDF5Writer [{}] source={} wrote {} bytes",
                               config_.name, entry.batch.root_source, written);
                        if (written > 0)
                        {
                            pool_->recordWrite(entry.batch.root_source, written);
                        }
                    }
                }
            }
            catch (const H5::Exception& ex)
            {
                errorf(*logger_, "HDF5Writer [{}] source={} write HDF5 error: {}",
                       config_.name, entry.batch.root_source, ex.getCDetailMsg());
            }
            catch (const std::exception& ex)
            {
                errorf(*logger_, "HDF5Writer [{}] source={} write failed: {}",
                       config_.name, entry.batch.root_source, ex.what());
            }
            catch (...)
            {
                errorf(*logger_, "HDF5Writer [{}] source={} write failed — unknown exception",
                       config_.name, entry.batch.root_source);
            }
        }
    }
    debugf(*logger_, "HDF5Writer [{}] writer thread exited", config_.name);
}

// ---------------------------------------------------------------------------
// flushLoop() — background flush thread
// ---------------------------------------------------------------------------

void HDF5Writer::flushLoop()
{
    debugf(*logger_, "HDF5Writer [{}] flush thread started (interval={}ms)", config_.name,
           std::chrono::duration_cast<std::chrono::milliseconds>(config_.flushInterval).count());

    // Periodically flush OS/HDF5 buffers to disk so data is visible to readers
    // even if the file is still open (not yet closed/rotated).
    while (!stopping_.load())
    {
        std::this_thread::sleep_for(config_.flushInterval);
        if (pool_)
        {
            pool_->flushAll();
        }
    }

    // One final flush after stopping_ is set to ensure the last batch written
    // by writerLoop() reaches disk before closeAll() is called in stop().
    if (pool_)
    {
        debugf(*logger_, "HDF5Writer [{}] final flush on shutdown", config_.name);
        pool_->flushAll();
    }
    debugf(*logger_, "HDF5Writer [{}] flush thread exited", config_.name);
}

// ---------------------------------------------------------------------------
// appendFrame() — write one protobuf DataFrame into an open HDF5 file
// ---------------------------------------------------------------------------
//
// Each DataFrame carries:
//   - A TimestampList (one timestamp per sample row)
//   - Zero or more typed columns (double, float, int32, int64, bool, string)
//   - Zero or more typed array columns (same types, but each value is an array)
//
// HDF5 layout produced per source file:
//
//   /timestamps          int64[N]         nanoseconds since UNIX epoch
//   /<col_name>          <type>[N]        one entry per scalar column
//   /<col_name>          <type>[N, L]     one row per array column sample (L = array length)
//
// Write pattern for every column (scalar and array):
//   1. ensureDataset[2D]()  open or create chunked unlimited dataset
//   2. extend()             grow dataset by N new rows
//   3. selectHyperslab()    target only the appended rows in file space
//   4. write()              copy from in-memory buffer
//
// CALLER MUST HOLD entry->fileMutex for the entire duration of this call.

static constexpr hsize_t kChunkSize = 64; // rows per HDF5 chunk; small to minimise
                                          // initial allocation overhead on new files

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers — extend+hyperslab+write, templated on C type.
// These are free functions to keep proto types out of the class interface.
// ---------------------------------------------------------------------------

namespace {

// Append n rows to a 1-D dataset: extend → hyperslab → write.
template <typename CType>
void append1D(H5::DataSet& ds, const H5::DataType& h5type, const CType* data, hsize_t n)
{
    hsize_t curDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
    ds.getSpace().getSimpleExtentDims(curDims, maxDims);
    const hsize_t newSize = curDims[0] + n;
    ds.extend(&newSize);
    H5::DataSpace fspace = ds.getSpace();
    fspace.getSimpleExtentDims(curDims, maxDims);
    hsize_t offset[1] = {curDims[0] - n};
    hsize_t count[1] = {n};
    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
    H5::DataSpace mspace(1, count);
    ds.write(data, h5type, mspace, fspace);
}

// Append nSamples rows to a 2-D dataset (nSamples × arrayLen): extend → hyperslab → write.
template <typename CType>
void append2D(H5::DataSet& ds, const H5::DataType& h5type, const CType* data, hsize_t nSamples, hsize_t arrayLen)
{
    hsize_t curDims[2] = {0, arrayLen}, maxDims[2] = {H5S_UNLIMITED, arrayLen};
    ds.getSpace().getSimpleExtentDims(curDims, maxDims);
    hsize_t newDims[2] = {curDims[0] + nSamples, arrayLen};
    ds.extend(newDims);
    H5::DataSpace fspace = ds.getSpace();
    fspace.getSimpleExtentDims(curDims, maxDims);
    hsize_t offset[2] = {curDims[0] - nSamples, 0};
    hsize_t count[2] = {nSamples, arrayLen};
    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
    H5::DataSpace mspace(2, count);
    ds.write(data, h5type, mspace, fspace);
}

// Write all numeric scalar columns of one proto repeated field.
// ProtoCol must expose: name(), values_size(), values() (proto repeated numeric).
template <typename CType, typename ProtoCol, typename EnsureFn>
void writeScalarColumns(const google::protobuf::RepeatedPtrField<ProtoCol>& cols,
                        const H5::DataType&                                 h5type,
                        EnsureFn                                            ensureDataset)
{
    for (const auto& col : cols)
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;
        std::vector<CType> buf(col.values().begin(), col.values().end());
        auto               ds = ensureDataset(col.name(), h5type);
        append1D(ds, h5type, buf.data(), static_cast<hsize_t>(n));
    }
}

// Write all numeric array columns of one proto repeated field.
// ProtoCol must expose: name(), has_dimensions(), dimensions().dims(0), values_size(), values().
template <typename CType, typename ProtoCol, typename EnsureFn2D>
void writeArrayColumns(const google::protobuf::RepeatedPtrField<ProtoCol>& cols,
                       const H5::DataType&                                 h5type,
                       EnsureFn2D                                          ensureDataset2D)
{
    for (const auto& col : cols)
    {
        if (col.name().empty() || !col.has_dimensions())
            continue;
        const auto& dims = col.dimensions();
        if (dims.dims_size() == 0)
            continue;
        const hsize_t arrayLen = static_cast<hsize_t>(dims.dims(0));
        if (arrayLen == 0)
            continue;
        const hsize_t nSamples = static_cast<hsize_t>(col.values_size()) / arrayLen;
        if (nSamples == 0)
            continue;
        std::vector<CType> buf(col.values().begin(), col.values().end());
        auto               ds = ensureDataset2D(col.name(), h5type, arrayLen);
        append2D(ds, h5type, buf.data(), nSamples, arrayLen);
    }
}

// ---------------------------------------------------------------------------
// Type-to-HDF5-predicate mapping — used by the NTTable per-column writer.
// ---------------------------------------------------------------------------

template <typename T>
const H5::PredType& mapNativeType();

template <>
const H5::PredType& mapNativeType<double>()
{
    return H5::PredType::NATIVE_DOUBLE;
}

template <>
const H5::PredType& mapNativeType<float>()
{
    return H5::PredType::NATIVE_FLOAT;
}

template <>
const H5::PredType& mapNativeType<int32_t>()
{
    return H5::PredType::NATIVE_INT32;
}

template <>
const H5::PredType& mapNativeType<int64_t>()
{
    return H5::PredType::NATIVE_INT64;
}

template <>
const H5::PredType& mapNativeType<uint8_t>()
{
    return H5::PredType::NATIVE_UINT8;
}

// Fill value used when padding a column shorter than the timestamp vector.
template <typename T>
T fillValue()
{
    return T{0};
}

template <>
double fillValue<double>()
{
    return std::numeric_limits<double>::quiet_NaN();
}

template <>
float fillValue<float>()
{
    return std::numeric_limits<float>::quiet_NaN();
}

} // namespace

H5::DataSet HDF5Writer::ensureDataset(H5::H5File&         file,
                                      const std::string&  name,
                                      const H5::DataType& dtype)
{
    if (file.nameExists(name))
    {
        return file.openDataSet(name);
    }

    // Create a 1-D chunked dataset with unlimited max extent so extend() works.
    tracef(*logger_, "HDF5Writer ensureDataset '{}' — creating new chunked dataset (chunk={})", name, kChunkSize);
    hsize_t       dims[1] = {0};
    hsize_t       maxDims[1] = {H5S_UNLIMITED};
    H5::DataSpace space(1, dims, maxDims);

    hsize_t               chunkDims[1] = {kChunkSize};
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
                             H5::H5File&                           file,
                             uint64_t                              batchSeq)
{
    // Frames without a timestamp list carry no time context — skip entirely.
    if (!frame.has_datatimestamps() || !frame.datatimestamps().has_timestamplist())
    {
        debugf(*logger_, "HDF5Writer appendFrame source={} — frame has no timestamps, skipping", sourceName);
        return;
    }
    const auto& tslist = frame.datatimestamps().timestamplist();
    const int   tsCount = tslist.timestamps_size();
    if (tsCount <= 0)
    {
        debugf(*logger_, "HDF5Writer appendFrame source={} — timestamp list empty, skipping", sourceName);
        return;
    }

    // Lambdas capture `file` and `this` so helpers below don't need to know about HDF5Writer.
    auto ensure1D = [&](const std::string& name, const H5::DataType& dtype)
    {
        return ensureDataset(file, name, dtype);
    };
    auto ensure2D = [&](const std::string& name, const H5::DataType& dtype, hsize_t arrayLen)
    {
        return ensureDataset2D(file, name, dtype, arrayLen);
    };

    // -------------------------------------------------------------------------
    // 1. timestamps dataset — int64 nanoseconds-since-epoch
    //    Skip if this batchSeq has already written timestamps for this source
    //    (split-column NTTable frames share the same batchSeq and timestamps).
    // -------------------------------------------------------------------------
    {
        auto it = lastTsBatchSeq_.find(sourceName);
        if (it != lastTsBatchSeq_.end() && it->second == batchSeq)
        {
            tracef(*logger_,
                   "HDF5Writer appendFrame source={} batchSeq={} — " "timestamps already written (split-column frame), skipping",
                   sourceName, batchSeq);
        }
        else
        {
            std::vector<int64_t> nsVec;
            nsVec.reserve(static_cast<std::size_t>(tsCount));
            for (int i = 0; i < tsCount; ++i)
            {
                const auto& ts = tslist.timestamps(i);
                nsVec.push_back(
                    static_cast<int64_t>(ts.epochseconds()) * 1'000'000'000LL +
                    static_cast<int64_t>(ts.nanoseconds()));
            }
            auto ds = ensure1D("timestamps", H5::PredType::NATIVE_INT64);
            append1D(ds, H5::PredType::NATIVE_INT64, nsVec.data(), static_cast<hsize_t>(tsCount));
            lastTsBatchSeq_[sourceName] = batchSeq;
        }
    }

    // -------------------------------------------------------------------------
    // 2. Scalar columns
    // -------------------------------------------------------------------------
    writeScalarColumns<double>(frame.doublecolumns(), H5::PredType::NATIVE_DOUBLE, ensure1D);
    writeScalarColumns<float>(frame.floatcolumns(), H5::PredType::NATIVE_FLOAT, ensure1D);
    writeScalarColumns<int32_t>(frame.int32columns(), H5::PredType::NATIVE_INT32, ensure1D);
    writeScalarColumns<int64_t>(frame.int64columns(), H5::PredType::NATIVE_INT64, ensure1D);

    // bool: protobuf bool → unsigned int for HDF5 NATIVE_HBOOL
    for (const auto& col : frame.boolcolumns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;
        std::vector<unsigned int> buf;
        buf.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            buf.push_back(col.values(i) ? 1u : 0u);
        auto ds = ensure1D(col.name(), H5::PredType::NATIVE_HBOOL);
        append1D(ds, H5::PredType::NATIVE_HBOOL, buf.data(), static_cast<hsize_t>(n));
    }

    // string: variable-length HDF5 strings via const char* ptrs
    {
        const H5::StrType vlStrType(H5::PredType::C_S1, H5T_VARIABLE);
        for (const auto& col : frame.stringcolumns())
        {
            if (col.name().empty())
                continue;
            const int n = col.values_size();
            if (n <= 0)
                continue;
            std::vector<const char*> ptrs;
            ptrs.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
                ptrs.push_back(col.values(i).c_str());
            auto ds = ensure1D(col.name(), vlStrType);
            append1D(ds, vlStrType, ptrs.data(), static_cast<hsize_t>(n));
        }
    }

    // -------------------------------------------------------------------------
    // 3. Array columns (2-D datasets: N_samples × array_len)
    // -------------------------------------------------------------------------
    writeArrayColumns<double>(frame.doublearraycolumns(), H5::PredType::NATIVE_DOUBLE, ensure2D);
    writeArrayColumns<float>(frame.floatarraycolumns(), H5::PredType::NATIVE_FLOAT, ensure2D);
    writeArrayColumns<int32_t>(frame.int32arraycolumns(), H5::PredType::NATIVE_INT32, ensure2D);
    writeArrayColumns<int64_t>(frame.int64arraycolumns(), H5::PredType::NATIVE_INT64, ensure2D);

    // bool array: bool → unsigned int conversion
    for (const auto& col : frame.boolarraycolumns())
    {
        if (col.name().empty() || !col.has_dimensions())
            continue;
        const auto& dims = col.dimensions();
        if (dims.dims_size() == 0)
            continue;
        const hsize_t arrayLen = static_cast<hsize_t>(dims.dims(0));
        if (arrayLen == 0)
            continue;
        const hsize_t nSamples = static_cast<hsize_t>(col.values_size()) / arrayLen;
        if (nSamples == 0)
            continue;
        std::vector<unsigned int> buf;
        buf.reserve(static_cast<std::size_t>(col.values_size()));
        for (int i = 0; i < col.values_size(); ++i)
            buf.push_back(col.values(i) ? 1u : 0u);
        auto ds = ensure2D(col.name(), H5::PredType::NATIVE_HBOOL, arrayLen);
        append2D(ds, H5::PredType::NATIVE_HBOOL, buf.data(), nSamples, arrayLen);
    }
}

// ---------------------------------------------------------------------------
// ensureDataset2D() — open or create a 2-D chunked dataset
// ---------------------------------------------------------------------------

H5::DataSet HDF5Writer::ensureDataset2D(H5::H5File&         file,
                                        const std::string&  name,
                                        const H5::DataType& dtype,
                                        hsize_t             arrayLen)
{
    if (file.nameExists(name))
    {
        return file.openDataSet(name);
    }

    // Row dimension is unlimited (grows with each appendFrame call).
    // Column dimension is fixed to arrayLen (waveform length must not change).
    tracef(*logger_, "HDF5Writer ensureDataset2D '{}' — creating new 2D chunked dataset (arrayLen={}, chunk={})", name, arrayLen, kChunkSize);
    hsize_t       dims[2] = {0, arrayLen};
    hsize_t       maxDims[2] = {H5S_UNLIMITED, arrayLen};
    H5::DataSpace space(2, dims, maxDims);

    hsize_t               chunkDims[2] = {kChunkSize, arrayLen};
    H5::DSetCreatPropList props;
    props.setChunk(2, chunkDims);
    if (config_.compressionLevel > 0)
    {
        props.setDeflate(config_.compressionLevel);
    }

    return file.createDataSet(name, dtype, space, props);
}

// ---------------------------------------------------------------------------
// isTabularBatch() — detect tabular batches by tag convention
// ---------------------------------------------------------------------------

bool HDF5Writer::isTabularBatch(const EventBatch& batch)
{
    return batch.is_tabular;
}

// ---------------------------------------------------------------------------
// processTabularBatch() — accumulate all column frames, then flush compound
// ---------------------------------------------------------------------------

void HDF5Writer::processTabularBatch(const QueueEntry& entry)
{
    const auto& batch = entry.batch;
    const auto& source = batch.root_source;
    auto&       buf = tabularBuffers_[source];

    for (const auto& frame : batch.frames)
        accumulateTabularFrame(source, frame, buf);

    // Don't flush here — accumulate across batches until timestamp changes.
    // Flushing is triggered by accumulateTabularFrame() when it detects a
    // new update round (different first-row timestamp), or by writerLoop()
    // draining the entire queue (handled in writerLoop after all drained batches).
}

// ---------------------------------------------------------------------------
// accumulateTabularFrame() — extract timestamps + typed columns into buffer
// ---------------------------------------------------------------------------

void HDF5Writer::accumulateTabularFrame(const std::string&                    sourceName,
                                        const dp::service::common::DataFrame& frame,
                                        TabularBuffer&                        buf)
{
    // Determine the first-row timestamp of this frame to detect round changes.
    int64_t frameFirstTs = -1;
    if (frame.has_datatimestamps() && frame.datatimestamps().has_timestamplist() &&
        frame.datatimestamps().timestamplist().timestamps_size() > 0)
    {
        const auto& ts0 = frame.datatimestamps().timestamplist().timestamps(0);
        frameFirstTs = static_cast<int64_t>(ts0.epochseconds()) * 1'000'000'000LL +
                       static_cast<int64_t>(ts0.nanoseconds());
    }

    // If this frame belongs to a new update round (timestamp changed) and we
    // already have buffered rows, clear the stale data.  The marker protocol
    // is the authoritative flush trigger; stale data from a missed marker is
    // simply discarded here.
    if (buf.rowCount > 0 && frameFirstTs != -1 && frameFirstTs != buf.roundFirstTs)
    {
        buf.columns.clear();
        if (!buf.schemaFixed)
        {
            buf.colIndex.clear();
            buf.colNames.clear();
            buf.colTypes.clear();
        }
        buf.tsSeconds.clear();
        buf.tsNanos.clear();
        buf.rowCount = 0;
        buf.roundFirstTs = -1;
    }

    // Populate timestamps for a new round.
    if (buf.rowCount == 0 && frame.has_datatimestamps() &&
        frame.datatimestamps().has_timestamplist())
    {
        const auto& tsList = frame.datatimestamps().timestamplist();
        const int   n = tsList.timestamps_size();
        buf.tsSeconds.reserve(static_cast<std::size_t>(n));
        buf.tsNanos.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            buf.tsSeconds.push_back(static_cast<int64_t>(tsList.timestamps(i).epochseconds()));
            buf.tsNanos.push_back(static_cast<int64_t>(tsList.timestamps(i).nanoseconds()));
        }
        buf.rowCount = static_cast<std::size_t>(n);
        buf.roundFirstTs = frameFirstTs;
    }

    // Typed per-column accumulation — one block per proto column type.

    // double columns
    for (const auto& col : frame.doublecolumns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;
        if (buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            if (buf.warnedUnknown.insert(col.name()).second)
                warnf(*logger_,
                      "HDF5Writer tabular source={} unknown column '{}' after schema lock, skipping",
                      sourceName, col.name());
            continue;
        }
        if (!buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            buf.colIndex[col.name()] = buf.colNames.size();
            buf.colNames.push_back(col.name());
            buf.colTypes[col.name()] = FieldType::Float64;
            buf.columns.emplace_back(std::vector<double>{});
        }
        const std::size_t colIdx = buf.colIndex.at(col.name());
        auto&             vec = std::get<std::vector<double>>(buf.columns[colIdx]);
        while (vec.size() + static_cast<std::size_t>(n) < buf.rowCount)
            vec.push_back(fillValue<double>());
        for (int i = 0; i < n; ++i)
            vec.push_back(static_cast<double>(col.values(i)));
    }

    // float columns
    for (const auto& col : frame.floatcolumns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;
        if (buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            if (buf.warnedUnknown.insert(col.name()).second)
                warnf(*logger_,
                      "HDF5Writer tabular source={} unknown column '{}' after schema lock, skipping",
                      sourceName, col.name());
            continue;
        }
        if (!buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            buf.colIndex[col.name()] = buf.colNames.size();
            buf.colNames.push_back(col.name());
            buf.colTypes[col.name()] = FieldType::Float32;
            buf.columns.emplace_back(std::vector<float>{});
        }
        const std::size_t colIdx = buf.colIndex.at(col.name());
        auto&             vec = std::get<std::vector<float>>(buf.columns[colIdx]);
        while (vec.size() + static_cast<std::size_t>(n) < buf.rowCount)
            vec.push_back(fillValue<float>());
        for (int i = 0; i < n; ++i)
            vec.push_back(static_cast<float>(col.values(i)));
    }

    // int32 columns
    for (const auto& col : frame.int32columns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;
        if (buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            if (buf.warnedUnknown.insert(col.name()).second)
                warnf(*logger_,
                      "HDF5Writer tabular source={} unknown column '{}' after schema lock, skipping",
                      sourceName, col.name());
            continue;
        }
        if (!buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            buf.colIndex[col.name()] = buf.colNames.size();
            buf.colNames.push_back(col.name());
            buf.colTypes[col.name()] = FieldType::Int32;
            buf.columns.emplace_back(std::vector<int32_t>{});
        }
        const std::size_t colIdx = buf.colIndex.at(col.name());
        auto&             vec = std::get<std::vector<int32_t>>(buf.columns[colIdx]);
        while (vec.size() + static_cast<std::size_t>(n) < buf.rowCount)
            vec.push_back(fillValue<int32_t>());
        for (int i = 0; i < n; ++i)
            vec.push_back(static_cast<int32_t>(col.values(i)));
    }

    // int64 columns
    for (const auto& col : frame.int64columns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;
        if (buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            if (buf.warnedUnknown.insert(col.name()).second)
                warnf(*logger_,
                      "HDF5Writer tabular source={} unknown column '{}' after schema lock, skipping",
                      sourceName, col.name());
            continue;
        }
        if (!buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            buf.colIndex[col.name()] = buf.colNames.size();
            buf.colNames.push_back(col.name());
            buf.colTypes[col.name()] = FieldType::Int64;
            buf.columns.emplace_back(std::vector<int64_t>{});
        }
        const std::size_t colIdx = buf.colIndex.at(col.name());
        auto&             vec = std::get<std::vector<int64_t>>(buf.columns[colIdx]);
        while (vec.size() + static_cast<std::size_t>(n) < buf.rowCount)
            vec.push_back(fillValue<int64_t>());
        for (int i = 0; i < n; ++i)
            vec.push_back(static_cast<int64_t>(col.values(i)));
    }

    // bool columns (stored as uint8_t)
    for (const auto& col : frame.boolcolumns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;
        if (buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            if (buf.warnedUnknown.insert(col.name()).second)
                warnf(*logger_,
                      "HDF5Writer tabular source={} unknown column '{}' after schema lock, skipping",
                      sourceName, col.name());
            continue;
        }
        if (!buf.schemaFixed && buf.colIndex.find(col.name()) == buf.colIndex.end())
        {
            buf.colIndex[col.name()] = buf.colNames.size();
            buf.colNames.push_back(col.name());
            buf.colTypes[col.name()] = FieldType::Bool;
            buf.columns.emplace_back(std::vector<uint8_t>{});
        }
        const std::size_t colIdx = buf.colIndex.at(col.name());
        auto&             vec = std::get<std::vector<uint8_t>>(buf.columns[colIdx]);
        while (vec.size() + static_cast<std::size_t>(n) < buf.rowCount)
            vec.push_back(fillValue<uint8_t>());
        for (int i = 0; i < n; ++i)
            vec.push_back(static_cast<uint8_t>(col.values(i) ? 1 : 0));
    }
}

// ---------------------------------------------------------------------------
// flushTabularBuffer() — write per-column 1-D datasets under a group
// ---------------------------------------------------------------------------

void HDF5Writer::flushTabularBuffer(const std::string& sourceName,
                                    TabularBuffer&     buf,
                                    H5::H5File&        file)
{
    const std::size_t nRows = buf.rowCount;
    if (nRows == 0)
        return;

    // Lock schema on first flush.
    if (!buf.schemaFixed)
    {
        buf.schemaFixed = true;
        infof(*logger_,
              "HDF5Writer tabular source={} schema locked ({} columns)",
              sourceName, buf.colNames.size());
    }

    const std::size_t nCols = buf.colNames.size();
    if (nCols == 0)
    {
        buf.rowCount = 0;
        return;
    }

    // Create HDF5 group for this source if it does not exist yet.
    if (!file.nameExists(sourceName))
        file.createGroup(sourceName);

    // ---- timestamp datasets: <source>/secondsPastEpoch and <source>/nanoseconds ----
    const std::string secPath = sourceName + "/secondsPastEpoch";
    const std::string nanoPath = sourceName + "/nanoseconds";

    buf.tsSeconds.resize(nRows, 0LL);
    buf.tsNanos.resize(nRows, 0LL);

    {
        H5::DataSet ds = ensureDataset(file, secPath, H5::PredType::NATIVE_INT64);
        append1D(ds, H5::PredType::NATIVE_INT64, buf.tsSeconds.data(),
                 static_cast<hsize_t>(nRows));
    }
    {
        H5::DataSet ds = ensureDataset(file, nanoPath, H5::PredType::NATIVE_INT64);
        append1D(ds, H5::PredType::NATIVE_INT64, buf.tsNanos.data(),
                 static_cast<hsize_t>(nRows));
    }

    // ---- per-column 1-D datasets: <source>/<colName> ----------------------
    for (std::size_t i = 0; i < nCols; ++i)
    {
        const std::string dsPath = sourceName + "/" + buf.colNames[i];
        std::visit([&](auto& vec)
                   {
                       using T = typename std::decay_t<decltype(vec)>::value_type;
                       while (vec.size() < nRows)
                           vec.push_back(fillValue<T>());
                       const H5::PredType& h5type = mapNativeType<T>();
                       H5::DataSet         ds = ensureDataset(file, dsPath, h5type);
                       append1D(ds, h5type, vec.data(), static_cast<hsize_t>(nRows));
                   },
                   buf.columns[i]);
    }

    // Approximate bytes written: nRows × (nCols × sizeof(double) + 2 × sizeof(int64_t))
    const uint64_t approxBytes =
        static_cast<uint64_t>(nRows) *
        (static_cast<uint64_t>(nCols) * sizeof(double) +
         2ULL * sizeof(int64_t));

    tracef(*logger_, "HDF5Writer tabular source={} flushed {} rows × {} cols (~{} bytes)",
           sourceName, nRows, nCols, approxBytes);

    // Clear buffer, preserving typed column slots so accumulation can resume.
    buf.tsSeconds.clear();
    buf.tsNanos.clear();
    buf.rowCount = 0;
    buf.columns.clear();
    if (buf.schemaFixed)
    {
        buf.columns.resize(nCols);
        for (std::size_t i = 0; i < nCols; ++i)
        {
            const auto ft = buf.colTypes.at(buf.colNames[i]);
            switch (ft)
            {
            case FieldType::Float64: buf.columns[i] = std::vector<double>{}; break;
            case FieldType::Float32: buf.columns[i] = std::vector<float>{}; break;
            case FieldType::Int32: buf.columns[i] = std::vector<int32_t>{}; break;
            case FieldType::Int64: buf.columns[i] = std::vector<int64_t>{}; break;
            case FieldType::Bool: buf.columns[i] = std::vector<uint8_t>{}; break;
            }
        }
    }
}
