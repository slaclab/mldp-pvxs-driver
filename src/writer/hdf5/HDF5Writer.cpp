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
    infof("HDF5Writer [{}] starting (output_dir={}, max_file_size_mb={}, flush_interval_ms={})",
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

    infof("HDF5Writer [{}] started — writer and flush threads running", config_.name);
}

void HDF5Writer::stop() noexcept
{
    infof("HDF5Writer [{}] stopping", config_.name);

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
    infof("HDF5Writer [{}] stopped", config_.name);
}

// ---------------------------------------------------------------------------
// push() — enqueue into bounded MPSC queue
// ---------------------------------------------------------------------------

bool HDF5Writer::push(util::bus::IDataBus::EventBatch batch) noexcept
{
    if (stopping_.load())
    {
        debugf("HDF5Writer [{}] push rejected — writer is stopping", config_.name);
        return false;
    }
    std::lock_guard<std::mutex> lk(queueMutex_);
    if (queue_.size() >= kQueueCapacity)
    {
        // Back-pressure: drop the batch rather than blocking the caller.
        warnf("HDF5Writer [{}] queue full ({} items) — dropping batch", config_.name, queue_.size());
        return false;
    }
    queue_.push_back(std::move(batch));
    queueCv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// writerLoop() — background writer thread
// ---------------------------------------------------------------------------

void HDF5Writer::writerLoop()
{
    debugf("HDF5Writer [{}] writer thread started", config_.name);
    while (true)
    {
        // --- Phase 1: drain the entire queue under a single lock window -----
        // Swapping into a local deque releases queueMutex_ before any I/O,
        // so push() is never blocked by slow HDF5 operations.
        std::deque<EventBatch> drained;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this]
                          {
                              return !queue_.empty() || stopping_.load();
                          });
            if (queue_.empty())
            {
                // stopping_ is set and queue is drained — safe to exit.
                debugf("HDF5Writer [{}] writer thread exiting — queue drained", config_.name);
                break;
            }
            drained.swap(queue_); // O(1); queue_ is left empty
        }

        if (!pool_)
        {
            warnf("HDF5Writer [{}] pool not initialised — skipping {} batches", config_.name, drained.size());
            continue;
        }

        // --- Phase 2: write each frame to HDF5 ------------------------------
        // Processing all drained batches without re-acquiring queueMutex_
        // means a burst of N pending batches is handled in one pass, which
        // catches up quickly after initial file-creation overhead.
        for (auto& batch : drained)
        {
            for (const auto& frame : batch.frames)
            {
                try
                {
                    // acquire() may create or rotate the file for this source.
                    auto entry = pool_->acquire(batch.root_source);

                    // Measure bytes written to update rotation accounting.
                    // fileMutex serialises access to the H5::H5File object
                    // against the flush thread (see locking discipline above).
                    uint64_t written = 0;
                    {
                        std::lock_guard<std::mutex> fileLk(entry->fileMutex);
                        const std::size_t           sizeBefore = static_cast<std::size_t>(entry->file.getFileSize());
                        appendFrame(batch.root_source, frame, entry->file);
                        const std::size_t sizeAfter = static_cast<std::size_t>(entry->file.getFileSize());
                        if (sizeAfter > sizeBefore)
                        {
                            written = static_cast<uint64_t>(sizeAfter - sizeBefore);
                        }
                    }
                    // IMPORTANT: fileMutex must be released BEFORE calling
                    // recordWrite().  recordWrite() acquires the pool's internal
                    // mutex_ (pool mutex_ → fileMutex order).  Holding fileMutex
                    // here while the flush thread holds pool mutex_ and waits for
                    // fileMutex would cause a deadlock.
                    if (written > 0)
                    {
                        tracef("HDF5Writer [{}] source={} wrote {} bytes",
                               config_.name, batch.root_source, written);
                        pool_->recordWrite(batch.root_source, written);
                    }
                }
                catch (const H5::Exception& ex)
                {
                    // H5::Exception does NOT inherit std::exception — must be
                    // caught explicitly or the thread will terminate silently.
                    errorf("HDF5Writer [{}] source={} appendFrame HDF5 error: {}",
                           config_.name, batch.root_source, ex.getCDetailMsg());
                }
                catch (const std::exception& ex)
                {
                    errorf("HDF5Writer [{}] source={} appendFrame failed: {}",
                           config_.name, batch.root_source, ex.what());
                }
                catch (...)
                {
                    errorf("HDF5Writer [{}] source={} appendFrame failed — unknown exception",
                           config_.name, batch.root_source);
                }
            }
        }
    }
    debugf("HDF5Writer [{}] writer thread exited", config_.name);
}

// ---------------------------------------------------------------------------
// flushLoop() — background flush thread
// ---------------------------------------------------------------------------

void HDF5Writer::flushLoop()
{
    debugf("HDF5Writer [{}] flush thread started (interval={}ms)", config_.name,
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
        debugf("HDF5Writer [{}] final flush on shutdown", config_.name);
        pool_->flushAll();
    }
    debugf("HDF5Writer [{}] flush thread exited", config_.name);
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

H5::DataSet HDF5Writer::ensureDataset(H5::H5File&         file,
                                      const std::string&  name,
                                      const H5::DataType& dtype)
{
    if (file.nameExists(name))
    {
        return file.openDataSet(name);
    }

    // Create a 1-D chunked dataset with unlimited max extent so extend() works.
    tracef("HDF5Writer ensureDataset '{}' — creating new chunked dataset (chunk={})", name, kChunkSize);
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
                             H5::H5File&                           file)
{
    // Frames without a timestamp list carry no time context — skip entirely.
    if (!frame.has_datatimestamps() || !frame.datatimestamps().has_timestamplist())
    {
        debugf("HDF5Writer appendFrame source={} — frame has no timestamps, skipping", sourceName);
        return;
    }
    const auto& tslist = frame.datatimestamps().timestamplist();
    const int   tsCount = tslist.timestamps_size();
    if (tsCount <= 0)
    {
        debugf("HDF5Writer appendFrame source={} — timestamp list empty, skipping", sourceName);
        return;
    }

    // -------------------------------------------------------------------------
    // 1. timestamps dataset — int64 nanoseconds-since-epoch
    // -------------------------------------------------------------------------
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

        auto    ds = ensureDataset(file, "timestamps", H5::PredType::NATIVE_INT64);
        hsize_t curDims[1] = {0};
        hsize_t maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(tsCount);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(tsCount)};
        hsize_t count[1] = {static_cast<hsize_t>(tsCount)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace mspace(1, count);
        ds.write(nsVec.data(), H5::PredType::NATIVE_INT64, mspace, fspace);
    }

    // -------------------------------------------------------------------------
    // 2. Scalar column helper — same extend+hyperslab pattern for every type
    // -------------------------------------------------------------------------
    //
    // Steps:
    //   a) ensureDataset() — open or create 1-D unlimited chunked dataset
    //   b) getSpace() + extend() — grow by n rows
    //   c) selectHyperslab() — target the last n rows in file space
    //   d) write() — copy from contiguous memory buffer

    // -- double columns --
    for (const auto& col : frame.doublecolumns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;

        auto    ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_DOUBLE);
        hsize_t curDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1] = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace       mspace(1, count);
        std::vector<double> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_DOUBLE, mspace, fspace);
    }

    // -- float columns --
    for (const auto& col : frame.floatcolumns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;

        auto    ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_FLOAT);
        hsize_t curDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1] = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace      mspace(1, count);
        std::vector<float> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_FLOAT, mspace, fspace);
    }

    // -- int32 columns --
    for (const auto& col : frame.int32columns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;

        auto    ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_INT32);
        hsize_t curDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1] = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace        mspace(1, count);
        std::vector<int32_t> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_INT32, mspace, fspace);
    }

    // -- int64 columns --
    for (const auto& col : frame.int64columns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;

        auto    ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_INT64);
        hsize_t curDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1] = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace        mspace(1, count);
        std::vector<int64_t> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_INT64, mspace, fspace);
    }

    // -- bool columns --
    // protobuf bool → unsigned int for HDF5 NATIVE_HBOOL (typedef to unsigned int)
    for (const auto& col : frame.boolcolumns())
    {
        if (col.name().empty())
            continue;
        const int n = col.values_size();
        if (n <= 0)
            continue;

        auto    ds = ensureDataset(file, col.name(), H5::PredType::NATIVE_HBOOL);
        hsize_t curDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
        ds.extend(&newSize);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
        hsize_t count[1] = {static_cast<hsize_t>(n)};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace             mspace(1, count);
        std::vector<unsigned int> buf;
        buf.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            buf.push_back(col.values(i) ? 1u : 0u);
        ds.write(buf.data(), H5::PredType::NATIVE_HBOOL, mspace, fspace);
    }

    // -- string columns --
    // HDF5 variable-length strings require an array of const char* pointers
    // (the actual string data is owned by the protobuf message throughout the write).
    {
        const H5::StrType vlStrType(H5::PredType::C_S1, H5T_VARIABLE);
        for (const auto& col : frame.stringcolumns())
        {
            if (col.name().empty())
                continue;
            const int n = col.values_size();
            if (n <= 0)
                continue;

            auto    ds = ensureDataset(file, col.name(), vlStrType);
            hsize_t curDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
            ds.getSpace().getSimpleExtentDims(curDims, maxDims);
            const hsize_t newSize = curDims[0] + static_cast<hsize_t>(n);
            ds.extend(&newSize);
            H5::DataSpace fspace = ds.getSpace();
            fspace.getSimpleExtentDims(curDims, maxDims);
            hsize_t offset[1] = {curDims[0] - static_cast<hsize_t>(n)};
            hsize_t count[1] = {static_cast<hsize_t>(n)};
            fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
            H5::DataSpace            mspace(1, count);
            std::vector<const char*> ptrs;
            ptrs.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
                ptrs.push_back(col.values(i).c_str());
            ds.write(ptrs.data(), vlStrType, mspace, fspace);
        }
    }

    // -------------------------------------------------------------------------
    // 3. Array column helper — 2-D dataset (N_samples × array_len)
    // -------------------------------------------------------------------------
    //
    // Each sample is a fixed-length array (e.g. a waveform snapshot).
    // Layout: rows = samples, columns = array elements.
    //
    // Steps identical to scalar case except dimensions are 2-D:
    //   a) ensureDataset2D() — open or create 2-D unlimited×fixed chunked dataset
    //   b) extend() on row dimension only
    //   c) selectHyperslab() — target the appended rows
    //   d) write() — flat contiguous buffer interpreted as row-major 2-D array

    // -- double array columns --
    for (const auto& col : frame.doublearraycolumns())
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

        auto    ds = ensureDataset2D(file, col.name(), H5::PredType::NATIVE_DOUBLE, arrayLen);
        hsize_t curDims[2] = {0, arrayLen}, maxDims[2] = {H5S_UNLIMITED, arrayLen};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        hsize_t newDims[2] = {curDims[0] + nSamples, arrayLen};
        ds.extend(newDims);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[2] = {curDims[0] - nSamples, 0};
        hsize_t count[2] = {nSamples, arrayLen};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace       mspace(2, count);
        std::vector<double> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_DOUBLE, mspace, fspace);
    }

    // -- float array columns --
    for (const auto& col : frame.floatarraycolumns())
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

        auto    ds = ensureDataset2D(file, col.name(), H5::PredType::NATIVE_FLOAT, arrayLen);
        hsize_t curDims[2] = {0, arrayLen}, maxDims[2] = {H5S_UNLIMITED, arrayLen};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        hsize_t newDims[2] = {curDims[0] + nSamples, arrayLen};
        ds.extend(newDims);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[2] = {curDims[0] - nSamples, 0};
        hsize_t count[2] = {nSamples, arrayLen};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace      mspace(2, count);
        std::vector<float> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_FLOAT, mspace, fspace);
    }

    // -- int32 array columns --
    for (const auto& col : frame.int32arraycolumns())
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

        auto    ds = ensureDataset2D(file, col.name(), H5::PredType::NATIVE_INT32, arrayLen);
        hsize_t curDims[2] = {0, arrayLen}, maxDims[2] = {H5S_UNLIMITED, arrayLen};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        hsize_t newDims[2] = {curDims[0] + nSamples, arrayLen};
        ds.extend(newDims);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[2] = {curDims[0] - nSamples, 0};
        hsize_t count[2] = {nSamples, arrayLen};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace        mspace(2, count);
        std::vector<int32_t> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_INT32, mspace, fspace);
    }

    // -- int64 array columns --
    for (const auto& col : frame.int64arraycolumns())
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

        auto    ds = ensureDataset2D(file, col.name(), H5::PredType::NATIVE_INT64, arrayLen);
        hsize_t curDims[2] = {0, arrayLen}, maxDims[2] = {H5S_UNLIMITED, arrayLen};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        hsize_t newDims[2] = {curDims[0] + nSamples, arrayLen};
        ds.extend(newDims);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[2] = {curDims[0] - nSamples, 0};
        hsize_t count[2] = {nSamples, arrayLen};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace        mspace(2, count);
        std::vector<int64_t> buf(col.values().begin(), col.values().end());
        ds.write(buf.data(), H5::PredType::NATIVE_INT64, mspace, fspace);
    }

    // -- bool array columns --
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

        auto    ds = ensureDataset2D(file, col.name(), H5::PredType::NATIVE_HBOOL, arrayLen);
        hsize_t curDims[2] = {0, arrayLen}, maxDims[2] = {H5S_UNLIMITED, arrayLen};
        ds.getSpace().getSimpleExtentDims(curDims, maxDims);
        hsize_t newDims[2] = {curDims[0] + nSamples, arrayLen};
        ds.extend(newDims);
        H5::DataSpace fspace = ds.getSpace();
        fspace.getSimpleExtentDims(curDims, maxDims);
        hsize_t offset[2] = {curDims[0] - nSamples, 0};
        hsize_t count[2] = {nSamples, arrayLen};
        fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
        H5::DataSpace             mspace(2, count);
        std::vector<unsigned int> buf;
        buf.reserve(static_cast<std::size_t>(col.values_size()));
        for (int i = 0; i < col.values_size(); ++i)
            buf.push_back(col.values(i) ? 1u : 0u);
        ds.write(buf.data(), H5::PredType::NATIVE_HBOOL, mspace, fspace);
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
    tracef("HDF5Writer ensureDataset2D '{}' — creating new 2D chunked dataset (arrayLen={}, chunk={})", name, arrayLen, kChunkSize);
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
