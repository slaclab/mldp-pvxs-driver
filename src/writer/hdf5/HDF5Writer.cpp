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
 *                               wake-up, routes to appendFrame() /
 *                               appendFrameMerge() / tabular path
 *                                      │
 *                               ┌──────▼──────────┐
 *                               │  HDF5FilePool   │  one .hdf5 file per source
 *                               └──────┬──────────┘      (non-merge mode)
 *                                      │ (flushThread_)
 *                               flushLoop() calls flushAll() periodically
 *
 * Two write modes
 * ---------------
 * Non-merge mode (default, mergeRootSources=false):
 *   One HDF5 file per root_source, managed by HDF5FilePool.  Each file is
 *   rotated independently on age/size threshold.
 *
 * Merge mode (mergeRootSources=true):
 *   All root_sources share a single HDF5 file.  Each source gets its own
 *   HDF5 group (/<source_name>/).  The file is rotated via rotateMergeFile()
 *   when age or size threshold is crossed.  Filenames include a UTC timestamp
 *   plus a monotonic sequence counter to prevent sub-second collision:
 *     merged_YYYYMMDDTHHMMSSz_<seq>.hdf5
 *   Files are written to a hidden temp path (prefix ".") and atomically
 *   renamed on close for crash-safe visibility.
 *
 * Two batch routes
 * ----------------
 * Non-tabular (columnar) batches:
 *   appendFrame() / appendFrameMerge() — write one DataBatch directly to
 *   per-column HDF5 datasets at the file root (non-merge) or under the
 *   source group (merge).  The per-batch batchSeq deduplicates the shared
 *   timestamps dataset across split-column NTTable frames.
 *
 * Tabular (NTTable) batches (EventBatch::is_tabular == true):
 *   accumulateTabularFrame() — buffers column data in TabularBuffer until the
 *   end_of_batch_group marker arrives, then flushTabularBuffer[Merge]() writes
 *   per-column 1-D datasets under a /<source_name>/ group.  Schema is locked
 *   on first flush; subsequent rounds must conform (unknown columns warned and
 *   skipped; missing columns filled with NaN/0).
 *
 *   If a new round's first-row timestamp differs from the buffered round's,
 *   the stale data is flushed before the new round is started — no data is
 *   silently discarded.
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
 *   - queueMutex_      : guards queue_ and stopping_. Held briefly; no HDF5
 *                        calls made while holding it.
 *   - pool mutex_      : internal to HDF5FilePool; guards source→FileEntry map.
 *   - entry->fileMutex : per-FileEntry; guards the H5::H5File object.
 *   - mergeFileMutex_  : guards mergeFile_, mergeOpenGroups_, mergeBytesWritten_,
 *                        and mergeFileOpenedAt_. Held during all merge-file I/O.
 *
 *   Lock-order rule: pool mutex_ → entry->fileMutex.
 *   NEVER hold fileMutex while calling pool methods (acquire / recordWrite),
 *   as pool methods acquire pool mutex_ internally.
 *
 *   Thread-exclusive maps (no mutex needed — writerThread_ only):
 *   - lastTsBatchSeq_ : tracks last batchSeq for which timestamps were written.
 *   - tabularBuffers_ : per-source accumulation buffers for NTTable writes.
 *   IMPORTANT: never access these from flushThread_ or caller threads without
 *   adding explicit synchronisation.
 *
 *   Merge-file rotation guard:
 *   - mergeRotating_ (atomic bool, CAS): prevents concurrent double-rotation
 *     when checkMergeRotation() is called concurrently (e.g. rapid batches).
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
 * writerLoop() captures queue_.size() (depthAtDrain) then swaps the entire
 * queue_ into a local deque under queueMutex_, releasing the lock before any
 * I/O.  This means:
 *   - push() is never blocked by slow HDF5 writes.
 *   - A single wake-up processes all accumulated batches, catching up quickly
 *     when the writer falls behind (e.g. after dataset creation overhead).
 *   - The queue-depth metric is set from depthAtDrain (pre-swap), so it
 *     reflects the actual backlog seen at each drain, not always 0.
 *
 * pool_->acquire() is hoisted outside the per-frame loop — all frames in one
 * EventBatch share the same root_source and therefore the same file handle.
 *
 * writeColumnsImpl() — shared column-write template
 * --------------------------------------------------
 * Both appendFrame() and appendFrameMerge() delegate column I/O to the
 * writeColumnsImpl<EnsureFn1D, EnsureFn2D, PostWriteFn>() template in the
 * anonymous namespace.  Callers supply:
 *   - ensure1D / ensure2D : lambdas that open or create the target dataset
 *                           in either the per-source file or the merge file.
 *   - postWrite           : lambda called with the byte count after each
 *                           column write; used by appendFrameMerge() to
 *                           update mergeBytesWritten_ for rotation tracking.
 *
 * append1D / append2D write pattern
 * ----------------------------------
 * For every column type the pattern is identical:
 *   1. ensureDataset[2D]() — open existing or create new chunked dataset.
 *   2. getSpace() → save preDims (pre-extend row count).
 *   3. extend()            — grow dataset by N new rows.
 *   4. getSpace() again    — get updated file dataspace.
 *   5. selectHyperslab()   — select only rows [preDims[0], preDims[0]+N).
 *   6. write()             — copy from in-memory buffer.
 * preDims are captured before extend() and used directly as the hyperslab
 * offset, avoiding a redundant second getSimpleExtentDims() call.
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
#include <writer/hdf5/HDF5WriterMetrics.h>
#include <util/log/Logger.h>

#include <cstring>
#include <ctime>
#include <limits>
#include <vector>

using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::writer;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

HDF5Writer::HDF5Writer(const config::Config& node,
                       std::shared_ptr<metrics::Metrics> metrics)
    : HDF5Writer(HDF5WriterConfig::parse(node))
{
    if (metrics)
    {
        writerMetrics_ = std::make_unique<metrics::HDF5WriterMetrics>(
            *metrics->registry(), metrics->controllerName(), config_.name);
    }
}

HDF5Writer::HDF5Writer(HDF5WriterConfig config)
    : config_(std::move(config))
    , logger_(util::log::newLogger("hdf5_writer:" + config_.name))
{
    if (config_.mergeRootSources && !supports_multi_root_source())
    {
        throw HDF5WriterConfig::Error(
            "writer.hdf5." + config_.name +
            ": merge-root-sources=true is not supported by this writer");
    }
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
    if (writerMetrics_)
    {
        pool_->setMetrics(writerMetrics_.get());
    }

    if (config_.mergeRootSources)
    {
        openMergeFile();
    }

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
    if (config_.mergeRootSources)
    {
        closeMergeFile();
    }
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
        if (writerMetrics_)
        {
            writerMetrics_->incrementQueueDrops();
        }
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
        std::size_t depthAtDrain = 0;
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
            depthAtDrain = queue_.size();
            drained.swap(queue_); // O(1); queue_ is left empty
        }

        // Update queue-depth metric with the depth captured before the drain.
        if (writerMetrics_)
        {
            writerMetrics_->setQueueDepth(static_cast<double>(depthAtDrain));
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
                        const auto t0 = std::chrono::steady_clock::now();
                        if (config_.mergeRootSources)
                        {
                            flushTabularBufferMerge(source, it->second);
                        }
                        else
                        {
                            auto                        ev = pool_->acquire(source);
                            std::lock_guard<std::mutex> fileLk(ev->fileMutex);
                            flushTabularBuffer(source, it->second, ev->file);
                        }
                        if (writerMetrics_)
                        {
                            const double ms = std::chrono::duration<double, std::milli>(
                                                  std::chrono::steady_clock::now() - t0)
                                                  .count();
                            writerMetrics_->observeWriteLatencyMs(ms);
                            writerMetrics_->incrementBatchesWritten();
                        }
                    }
                }
                else if (isTabularBatch(entry.batch))
                {
                    processTabularBatch(entry);
                }
                else
                {
                    if (!config_.mergeRootSources)
                    {
                        // Acquire file handle once for all frames in this batch — they all
                        // share the same root_source and therefore the same file.
                        auto ev = pool_->acquire(entry.batch.root_source);
                        for (const auto& frame : entry.batch.frames)
                        {
                            const uint64_t written = static_cast<uint64_t>(
                                frame.timestamps.size() * frame.columns.size() * sizeof(double));
                            {
                                std::lock_guard<std::mutex> fileLk(ev->fileMutex);
                                const auto t0 = std::chrono::steady_clock::now();
                                appendFrame(entry.batch.root_source, frame, ev->file, entry.batchSeq);
                                if (writerMetrics_)
                                {
                                    const double ms = std::chrono::duration<double, std::milli>(
                                                          std::chrono::steady_clock::now() - t0)
                                                          .count();
                                    writerMetrics_->observeWriteLatencyMs(ms);
                                }
                            }
                            tracef(*logger_, "HDF5Writer [{}] source={} wrote ~{} bytes",
                                   config_.name, entry.batch.root_source, written);
                            if (written > 0)
                            {
                                pool_->recordWrite(entry.batch.root_source, written);
                                if (writerMetrics_)
                                {
                                    writerMetrics_->incrementBytesWritten(entry.batch.root_source,
                                                                           static_cast<double>(written));
                                    writerMetrics_->incrementRowsWritten(
                                        entry.batch.root_source,
                                        static_cast<double>(frame.timestamps.size()));
                                }
                            }
                        }
                    }
                    else
                    {
                        for (const auto& frame : entry.batch.frames)
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            appendFrameMerge(entry.batch.root_source, frame, entry.batchSeq);
                            if (writerMetrics_)
                            {
                                const double ms = std::chrono::duration<double, std::milli>(
                                                      std::chrono::steady_clock::now() - t0)
                                                      .count();
                                writerMetrics_->observeWriteLatencyMs(ms);
                            }
                        }
                    }
                    if (writerMetrics_)
                    {
                        writerMetrics_->incrementBatchesWritten();
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
        if (config_.mergeRootSources && mergeFile_)
        {
            std::lock_guard<std::mutex> lk(mergeFileMutex_);
            try { mergeFile_->flush(H5F_SCOPE_GLOBAL); } catch (...) {}
        }
    }

    // One final flush after stopping_ is set to ensure the last batch written
    // by writerLoop() reaches disk before closeAll() is called in stop().
    if (pool_)
    {
        debugf(*logger_, "HDF5Writer [{}] final flush on shutdown", config_.name);
        pool_->flushAll();
    }
    if (config_.mergeRootSources && mergeFile_)
    {
        std::lock_guard<std::mutex> lk(mergeFileMutex_);
        try { mergeFile_->flush(H5F_SCOPE_GLOBAL); } catch (...) {}
    }
    debugf(*logger_, "HDF5Writer [{}] flush thread exited", config_.name);
}

// ---------------------------------------------------------------------------
// appendFrame() — write one DataBatch into an open HDF5 file
// ---------------------------------------------------------------------------
//
// Each DataBatch carries:
//   - A vector of TimestampEntry (one per sample row)
//   - Zero or more typed DataColumn entries (double, float, int32, int64, bool, string, bytes, arrays)
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
// ---------------------------------------------------------------------------

namespace {

// Append n rows to a 1-D dataset: extend → hyperslab → write.
template <typename CType>
void append1D(H5::DataSet& ds, const H5::DataType& h5type, const CType* data, hsize_t n)
{
    hsize_t preDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
    ds.getSpace().getSimpleExtentDims(preDims, maxDims);
    const hsize_t newSize = preDims[0] + n;
    ds.extend(&newSize);
    H5::DataSpace fspace = ds.getSpace();
    hsize_t offset[1] = {preDims[0]};
    hsize_t count[1]  = {n};
    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
    H5::DataSpace mspace(1, count);
    ds.write(data, h5type, mspace, fspace);
}

// Append nSamples rows to a 2-D dataset (nSamples × arrayLen): extend → hyperslab → write.
template <typename CType>
void append2D(H5::DataSet& ds, const H5::DataType& h5type, const CType* data, hsize_t nSamples, hsize_t arrayLen)
{
    hsize_t preDims[2] = {0, arrayLen}, maxDims[2] = {H5S_UNLIMITED, arrayLen};
    ds.getSpace().getSimpleExtentDims(preDims, maxDims);
    hsize_t newDims[2] = {preDims[0] + nSamples, arrayLen};
    ds.extend(newDims);
    H5::DataSpace fspace = ds.getSpace();
    hsize_t offset[2] = {preDims[0], 0};
    hsize_t count[2]  = {nSamples, arrayLen};
    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
    H5::DataSpace mspace(2, count);
    ds.write(data, h5type, mspace, fspace);
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

// ---------------------------------------------------------------------------
// writeColumnsImpl — shared column-write logic for appendFrame / appendFrameMerge
// ---------------------------------------------------------------------------
// EnsureFn1D:  (const std::string& name, const H5::DataType&) -> H5::DataSet
// EnsureFn2D:  (const std::string& name, const H5::DataType&, hsize_t arrayLen) -> H5::DataSet
// PostWriteFn: (uint64_t bytes) -> void   — called after each column write with byte count

namespace {

template <typename EnsureFn1D, typename EnsureFn2D, typename PostWriteFn>
void writeColumnsImpl(const mldp_pvxs_driver::util::bus::DataBatch& batch,
                      EnsureFn1D&&  ensure1D,
                      EnsureFn2D&&  ensure2D,
                      PostWriteFn&& postWrite)
{
    using namespace mldp_pvxs_driver::util::bus;

    for (const auto& col : batch.columns)
    {
        if (col.name.empty())
            continue;

        std::visit([&](const auto& vals)
                   {
                       using VecT  = std::decay_t<decltype(vals)>;
                       using ElemT = typename VecT::value_type;

                       if constexpr (std::is_same_v<ElemT, double>   ||
                                     std::is_same_v<ElemT, float>    ||
                                     std::is_same_v<ElemT, int64_t>  ||
                                     std::is_same_v<ElemT, int32_t>)
                       {
                           // Scalar 1-D column
                           const hsize_t n = static_cast<hsize_t>(vals.size());
                           if (n == 0)
                               return;
                           const H5::PredType& h5type = mapNativeType<ElemT>();
                           auto ds = ensure1D(col.name, h5type);
                           append1D(ds, h5type, vals.data(), n);
                           postWrite(static_cast<uint64_t>(n * sizeof(ElemT)));
                       }
                       else if constexpr (std::is_same_v<ElemT, bool>)
                       {
                           // bool → NATIVE_HBOOL (unsigned int)
                           const hsize_t n = static_cast<hsize_t>(vals.size());
                           if (n == 0)
                               return;
                           std::vector<unsigned int> buf;
                           buf.reserve(n);
                           for (bool v : vals)
                               buf.push_back(v ? 1u : 0u);
                           auto ds = ensure1D(col.name, H5::PredType::NATIVE_HBOOL);
                           append1D(ds, H5::PredType::NATIVE_HBOOL, buf.data(), n);
                           postWrite(static_cast<uint64_t>(n * sizeof(unsigned int)));
                       }
                       else if constexpr (std::is_same_v<ElemT, std::string>)
                       {
                           // Variable-length HDF5 strings
                           const hsize_t n = static_cast<hsize_t>(vals.size());
                           if (n == 0)
                               return;
                           const H5::StrType vlStrType(H5::PredType::C_S1, H5T_VARIABLE);
                           // Keep local copies so c_str() pointers stay valid
                           std::vector<std::string> strCopies(vals.begin(), vals.end());
                           std::vector<const char*> ptrs;
                           ptrs.reserve(n);
                           for (const auto& s : strCopies)
                               ptrs.push_back(s.c_str());
                           auto ds = ensure1D(col.name, vlStrType);
                           append1D(ds, vlStrType, ptrs.data(), n);
                           postWrite(0); // byte count for VL strings is indeterminate
                       }
                       else if constexpr (std::is_same_v<ElemT, std::vector<uint8_t>>)
                       {
                           // bytes/blob: stored as 2-D uint8 dataset (nSamples × blobLen)
                           if (vals.empty() || vals[0].empty())
                               return;
                           const hsize_t arrayLen  = static_cast<hsize_t>(vals[0].size());
                           const hsize_t nSamples  = static_cast<hsize_t>(vals.size());
                           std::vector<uint8_t> flat;
                           flat.reserve(nSamples * arrayLen);
                           for (const auto& row : vals)
                               flat.insert(flat.end(), row.begin(), row.end());
                           auto ds = ensure2D(col.name, H5::PredType::NATIVE_UINT8, arrayLen);
                           append2D(ds, H5::PredType::NATIVE_UINT8, flat.data(), nSamples, arrayLen);
                           postWrite(static_cast<uint64_t>(nSamples * arrayLen));
                       }
                       else if constexpr (std::is_same_v<ElemT, std::vector<double>>  ||
                                          std::is_same_v<ElemT, std::vector<float>>   ||
                                          std::is_same_v<ElemT, std::vector<int64_t>> ||
                                          std::is_same_v<ElemT, std::vector<int32_t>>)
                       {
                           // Numeric array column → 2-D dataset (nSamples × arrayLen)
                           using InnerT = typename ElemT::value_type;
                           if (vals.empty() || vals[0].empty())
                               return;
                           const hsize_t arrayLen = static_cast<hsize_t>(vals[0].size());
                           const hsize_t nSamples = static_cast<hsize_t>(vals.size());
                           std::vector<InnerT> flat;
                           flat.reserve(nSamples * arrayLen);
                           for (const auto& row : vals)
                               flat.insert(flat.end(), row.begin(), row.end());
                           const H5::PredType& h5type = mapNativeType<InnerT>();
                           auto ds = ensure2D(col.name, h5type, arrayLen);
                           append2D(ds, h5type, flat.data(), nSamples, arrayLen);
                           postWrite(static_cast<uint64_t>(nSamples * arrayLen * sizeof(InnerT)));
                       }
                       else if constexpr (std::is_same_v<ElemT, std::vector<bool>>)
                       {
                           // bool array → 2-D NATIVE_HBOOL dataset
                           if (vals.empty() || vals[0].empty())
                               return;
                           const hsize_t arrayLen = static_cast<hsize_t>(vals[0].size());
                           const hsize_t nSamples = static_cast<hsize_t>(vals.size());
                           std::vector<unsigned int> flat;
                           flat.reserve(nSamples * arrayLen);
                           for (const auto& row : vals)
                               for (bool v : row)
                                   flat.push_back(v ? 1u : 0u);
                           auto ds = ensure2D(col.name, H5::PredType::NATIVE_HBOOL, arrayLen);
                           append2D(ds, H5::PredType::NATIVE_HBOOL, flat.data(), nSamples, arrayLen);
                           postWrite(static_cast<uint64_t>(nSamples * arrayLen * sizeof(unsigned int)));
                       }
                   },
                   col.values);
    }
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

void HDF5Writer::appendFrame(const std::string&          sourceName,
                             const util::bus::DataBatch& batch,
                             H5::H5File&                 file,
                             uint64_t                    batchSeq)
{
    using namespace mldp_pvxs_driver::util::bus;

    // Frames without timestamps carry no time context — skip entirely.
    if (batch.timestamps.empty())
    {
        debugf(*logger_, "HDF5Writer appendFrame source={} — batch has no timestamps, skipping", sourceName);
        return;
    }
    const std::size_t tsCount = batch.timestamps.size();

    // -------------------------------------------------------------------------
    // 1. timestamps dataset — int64 nanoseconds-since-epoch
    // -------------------------------------------------------------------------
    {
        auto it = lastTsBatchSeq_.find(sourceName);
        if (it != lastTsBatchSeq_.end() && it->second == batchSeq)
        {
            tracef(*logger_,
                   "HDF5Writer appendFrame source={} batchSeq={} — "
                   "timestamps already written (split-column frame), skipping",
                   sourceName, batchSeq);
        }
        else
        {
            std::vector<int64_t> nsVec;
            nsVec.reserve(tsCount);
            for (const auto& ts : batch.timestamps)
            {
                nsVec.push_back(
                    static_cast<int64_t>(ts.epoch_seconds) * 1'000'000'000LL +
                    static_cast<int64_t>(ts.nanoseconds));
            }
            auto ds = ensureDataset(file, "timestamps", H5::PredType::NATIVE_INT64);
            append1D(ds, H5::PredType::NATIVE_INT64, nsVec.data(), static_cast<hsize_t>(tsCount));
            lastTsBatchSeq_[sourceName] = batchSeq;
        }
    }

    // -------------------------------------------------------------------------
    // 2. Scalar and array columns via writeColumnsImpl
    // -------------------------------------------------------------------------
    writeColumnsImpl(batch,
        [&](const std::string& n, const H5::DataType& t)
            { return ensureDataset(file, n, t); },
        [&](const std::string& n, const H5::DataType& t, hsize_t l)
            { return ensureDataset2D(file, n, t, l); },
        [](uint64_t) {} // no-op: byte accounting done by pool_->recordWrite in writerLoop
    );
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

void HDF5Writer::accumulateTabularFrame(const std::string&          sourceName,
                                        const util::bus::DataBatch& batch,
                                        TabularBuffer&              buf)
{
    using namespace mldp_pvxs_driver::util::bus;

    // Determine the first-row timestamp of this batch to detect round changes.
    int64_t frameFirstTs = -1;
    if (!batch.timestamps.empty())
    {
        const auto& ts0 = batch.timestamps[0];
        frameFirstTs = static_cast<int64_t>(ts0.epoch_seconds) * 1'000'000'000LL +
                       static_cast<int64_t>(ts0.nanoseconds);
    }

    // If this batch belongs to a new update round (timestamp changed) and we
    // already have buffered rows, flush the stale data rather than discarding it.
    if (buf.rowCount > 0 && frameFirstTs != -1 && frameFirstTs != buf.roundFirstTs)
    {
        // Flush stale partial round before starting new one — do NOT silently discard.
        if (config_.mergeRootSources)
        {
            flushTabularBufferMerge(sourceName, buf);
        }
        else
        {
            auto ev = pool_->acquire(sourceName);
            std::lock_guard<std::mutex> fileLk(ev->fileMutex);
            flushTabularBuffer(sourceName, buf, ev->file);
        }
        // flushTabularBuffer resets rowCount, tsSeconds, tsNanos, columns, roundFirstTs.
        // Fall through to populate timestamps and columns for the new round below.
    }

    // Populate timestamps for a new round.
    if (buf.rowCount == 0 && !batch.timestamps.empty())
    {
        const std::size_t n = batch.timestamps.size();
        buf.tsSeconds.reserve(n);
        buf.tsNanos.reserve(n);
        for (const auto& ts : batch.timestamps)
        {
            buf.tsSeconds.push_back(static_cast<int64_t>(ts.epoch_seconds));
            buf.tsNanos.push_back(static_cast<int64_t>(ts.nanoseconds));
        }
        buf.rowCount = n;
        buf.roundFirstTs = frameFirstTs;
    }

    // Typed per-column accumulation using std::visit.
    for (const auto& col : batch.columns)
    {
        if (col.name.empty())
            continue;

        if (buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
        {
            if (buf.warnedUnknown.size() < TabularBuffer::kMaxWarnedUnknown &&
                buf.warnedUnknown.insert(col.name).second)
            {
                warnf(*logger_,
                      "HDF5Writer tabular source={} unknown column '{}' after schema lock, skipping",
                      sourceName, col.name);
            }
            else if (buf.warnedUnknown.size() == TabularBuffer::kMaxWarnedUnknown &&
                     buf.warnedUnknown.find("__FULL__") == buf.warnedUnknown.end())
            {
                buf.warnedUnknown.insert("__FULL__");
                warnf(*logger_,
                      "HDF5Writer tabular source={} warnedUnknown set full ({} entries), suppressing further warnings",
                      sourceName, TabularBuffer::kMaxWarnedUnknown);
            }
            continue;
        }

        std::visit([&](const auto& vals)
                   {
                       using VecT  = std::decay_t<decltype(vals)>;
                       using ElemT = typename VecT::value_type;

                       const std::size_t n = vals.size();
                       if (n == 0)
                           return;

                       // Determine FieldType and TabularBuffer::ColumnData type for this variant.
                       // Only scalar numeric types are supported in the tabular path.
                       if constexpr (std::is_same_v<ElemT, double>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Float64;
                               buf.columns.emplace_back(std::vector<double>{});
                           }
                           const std::size_t colIdx = buf.colIndex.at(col.name);
                           auto& vec = std::get<std::vector<double>>(buf.columns[colIdx]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<double>());
                           for (const auto& v : vals)
                               vec.push_back(v);
                       }
                       else if constexpr (std::is_same_v<ElemT, float>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Float32;
                               buf.columns.emplace_back(std::vector<float>{});
                           }
                           const std::size_t colIdx = buf.colIndex.at(col.name);
                           auto& vec = std::get<std::vector<float>>(buf.columns[colIdx]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<float>());
                           for (const auto& v : vals)
                               vec.push_back(v);
                       }
                       else if constexpr (std::is_same_v<ElemT, int32_t>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Int32;
                               buf.columns.emplace_back(std::vector<int32_t>{});
                           }
                           const std::size_t colIdx = buf.colIndex.at(col.name);
                           auto& vec = std::get<std::vector<int32_t>>(buf.columns[colIdx]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<int32_t>());
                           for (const auto& v : vals)
                               vec.push_back(v);
                       }
                       else if constexpr (std::is_same_v<ElemT, int64_t>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Int64;
                               buf.columns.emplace_back(std::vector<int64_t>{});
                           }
                           const std::size_t colIdx = buf.colIndex.at(col.name);
                           auto& vec = std::get<std::vector<int64_t>>(buf.columns[colIdx]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<int64_t>());
                           for (const auto& v : vals)
                               vec.push_back(v);
                       }
                       else if constexpr (std::is_same_v<ElemT, bool>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Bool;
                               buf.columns.emplace_back(std::vector<uint8_t>{});
                           }
                           const std::size_t colIdx = buf.colIndex.at(col.name);
                           auto& vec = std::get<std::vector<uint8_t>>(buf.columns[colIdx]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<uint8_t>());
                           for (bool v : vals)
                               vec.push_back(static_cast<uint8_t>(v ? 1 : 0));
                       }
                       else
                       {
                           // Non-scalar types (string, bytes, array) are not supported in tabular path.
                           const std::string key = col.name + ":unsupported_type";
                           if (buf.warnedUnknown.size() < TabularBuffer::kMaxWarnedUnknown &&
                               buf.warnedUnknown.insert(key).second)
                           {
                               warnf(*logger_,
                                     "HDF5Writer tabular source={} column '{}' has unsupported type for tabular path, skipping",
                                     sourceName, col.name);
                           }
                           else if (buf.warnedUnknown.size() == TabularBuffer::kMaxWarnedUnknown &&
                                    buf.warnedUnknown.find("__FULL__") == buf.warnedUnknown.end())
                           {
                               buf.warnedUnknown.insert("__FULL__");
                               warnf(*logger_,
                                     "HDF5Writer tabular source={} warnedUnknown set full ({} entries), suppressing further warnings",
                                     sourceName, TabularBuffer::kMaxWarnedUnknown);
                           }
                       }
                   },
                   col.values);
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
    buf.roundFirstTs = -1;
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

// ---------------------------------------------------------------------------
// Merge-mode helpers
// ---------------------------------------------------------------------------

void HDF5Writer::openMergeFile()
{
    // Build a UTC timestamp suffix: YYYYMMDDTHHMMSSz
    const auto     now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char tsbuf[20];
    std::strftime(tsbuf, sizeof(tsbuf), "%Y%m%dT%H%M%Sz", &utc);
    // Append a monotonic sequence number so rapid rotations within the same
    // second produce distinct filenames instead of colliding.
    const std::string suffix = std::string(tsbuf) + "_" + std::to_string(mergeFileSeq_++);

    const std::filesystem::path base(config_.basePath);
    std::filesystem::create_directories(base);

    const std::string stem = "merged";
    mergePath_      = base / ("." + stem + "_" + suffix + ".hdf5");
    mergeFinalPath_ = base / (stem + "_" + suffix + ".hdf5");

    std::lock_guard<std::mutex> lk(mergeFileMutex_);
    mergeFile_ = std::make_unique<H5::H5File>(mergePath_.string(), H5F_ACC_TRUNC);
    mergeFileOpenedAt_ = std::chrono::steady_clock::now();
    mergeBytesWritten_ = 0;
    mergeOpenGroups_.clear();
    infof(*logger_, "HDF5Writer [{}] merge file opened: {}", config_.name, mergePath_.string());
}

void HDF5Writer::closeMergeFile() noexcept
{
    std::unique_lock<std::mutex> lk(mergeFileMutex_);
    if (!mergeFile_)
        return;
    try
    {
        mergeFile_->flush(H5F_SCOPE_GLOBAL);
        mergeFile_->close();
        mergeFile_.reset();
        lk.unlock();

        if (mergePath_ != mergeFinalPath_)
        {
            std::error_code ec;
            std::filesystem::rename(mergePath_, mergeFinalPath_, ec);
            if (ec)
                warnf(*logger_, "HDF5Writer [{}] merge file rename failed: {} -> {}: {}",
                      config_.name, mergePath_.string(), mergeFinalPath_.string(), ec.message());
            else
                debugf(*logger_, "HDF5Writer [{}] merge file renamed -> {}",
                       config_.name, mergeFinalPath_.string());
            mergePath_ = mergeFinalPath_;
        }
    }
    catch (const H5::Exception& ex)
    {
        errorf(*logger_, "HDF5Writer [{}] merge file close HDF5 error: {}",
               config_.name, ex.getCDetailMsg());
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_, "HDF5Writer [{}] merge file close failed: {}",
               config_.name, ex.what());
    }
    catch (...)
    {
        errorf(*logger_, "HDF5Writer [{}] merge file close failed — unknown exception",
               config_.name);
    }
}

void HDF5Writer::rotateMergeFile()
{
    infof(*logger_, "HDF5Writer [{}] rotating merge file", config_.name);
    // Snapshot groups before closing.
    std::set<std::string> groupsToRecreate;
    {
        std::lock_guard<std::mutex> lk(mergeFileMutex_);
        groupsToRecreate = mergeOpenGroups_;
    }
    closeMergeFile();
    openMergeFile();

    // Recreate all known source groups in the new file.
    std::lock_guard<std::mutex> lk(mergeFileMutex_);
    for (const auto& g : groupsToRecreate)
    {
        if (!mergeFile_->nameExists(g))
        {
            mergeFile_->createGroup(g);
            mergeOpenGroups_.insert(g);
        }
    }
    infof(*logger_, "HDF5Writer [{}] merge file rotated, {} groups recreated",
          config_.name, groupsToRecreate.size());
}

void HDF5Writer::ensureMergeGroup(const std::string& sourceName)
{
    // Caller MUST hold mergeFileMutex_.
    if (mergeOpenGroups_.find(sourceName) == mergeOpenGroups_.end())
    {
        if (!mergeFile_->nameExists(sourceName))
        {
            mergeFile_->createGroup(sourceName);
            infof(*logger_, "HDF5Writer [{}] merge group created: /{}/",
                  config_.name, sourceName);
        }
        mergeOpenGroups_.insert(sourceName);
    }
    // Callers that need the group object should call mergeFile_->openGroup() themselves.
}

void HDF5Writer::checkMergeRotation()
{
    // Called without mergeFileMutex_ held.
    bool needRotate = false;
    {
        std::lock_guard<std::mutex> lk(mergeFileMutex_);
        if (!mergeFile_)
            return;
        const auto     now = std::chrono::steady_clock::now();
        const auto     age = std::chrono::duration_cast<std::chrono::seconds>(now - mergeFileOpenedAt_);
        const uint64_t sizeLimitBytes =
            static_cast<uint64_t>(config_.maxFileSizeMB) * 1024ULL * 1024ULL;
        needRotate = (age >= config_.maxFileAge) ||
                     (sizeLimitBytes > 0 && mergeBytesWritten_ >= sizeLimitBytes);
    }
    if (needRotate)
    {
        bool expected = false;
        if (mergeRotating_.compare_exchange_strong(expected, true))
        {
            rotateMergeFile();
            mergeRotating_.store(false);
        }
    }
}

void HDF5Writer::appendFrameMerge(const std::string&          sourceName,
                                  const util::bus::DataBatch& batch,
                                  uint64_t                    batchSeq)
{
    using namespace mldp_pvxs_driver::util::bus;

    if (batch.timestamps.empty())
    {
        debugf(*logger_, "HDF5Writer appendFrameMerge source={} — no timestamps, skipping", sourceName);
        return;
    }

    checkMergeRotation();

    const std::size_t           tsCount = batch.timestamps.size();
    std::lock_guard<std::mutex> lk(mergeFileMutex_);

    if (!mergeFile_)
    {
        warnf(*logger_, "HDF5Writer [{}] appendFrameMerge — merge file not open, skipping", config_.name);
        return;
    }

    ensureMergeGroup(sourceName);
    const std::string groupPrefix = sourceName + "/";

    // 1. timestamps
    {
        auto it = lastTsBatchSeq_.find(sourceName);
        if (it == lastTsBatchSeq_.end() || it->second != batchSeq)
        {
            std::vector<int64_t> nsVec;
            nsVec.reserve(tsCount);
            for (const auto& ts : batch.timestamps)
            {
                nsVec.push_back(
                    static_cast<int64_t>(ts.epoch_seconds) * 1'000'000'000LL +
                    static_cast<int64_t>(ts.nanoseconds));
            }
            auto ds = ensureDataset(*mergeFile_, groupPrefix + "timestamps", H5::PredType::NATIVE_INT64);
            append1D(ds, H5::PredType::NATIVE_INT64, nsVec.data(), static_cast<hsize_t>(tsCount));
            lastTsBatchSeq_[sourceName] = batchSeq;
        }
    }

    // 2. Columns
    writeColumnsImpl(batch,
        [&](const std::string& n, const H5::DataType& t)
            { return ensureDataset(*mergeFile_, groupPrefix + n, t); },
        [&](const std::string& n, const H5::DataType& t, hsize_t l)
            { return ensureDataset2D(*mergeFile_, groupPrefix + n, t, l); },
        [&](uint64_t bytes) { mergeBytesWritten_ += bytes; }
    );
}

void HDF5Writer::flushTabularBufferMerge(const std::string& sourceName,
                                         TabularBuffer&     buf)
{
    checkMergeRotation();

    std::lock_guard<std::mutex> lk(mergeFileMutex_);
    if (!mergeFile_)
    {
        warnf(*logger_, "HDF5Writer [{}] flushTabularBufferMerge — merge file not open, skipping",
              config_.name);
        return;
    }

    // Capture stats before flush clears the buffer.
    const std::size_t nRows = buf.rowCount;
    const std::size_t nCols = buf.colNames.size();

    // ensureMergeGroup records the group in mergeOpenGroups_.
    ensureMergeGroup(sourceName);

    // flushTabularBuffer already creates /<sourceName>/ group and writes
    // sourceName + "/col" paths — fully compatible with the merge file.
    flushTabularBuffer(sourceName, buf, *mergeFile_);

    // Approximate bytes written: nRows × (nCols × sizeof(double) + 2 × sizeof(int64_t))
    if (nRows > 0 && nCols > 0)
    {
        const uint64_t approxBytes =
            static_cast<uint64_t>(nRows) *
            (static_cast<uint64_t>(nCols) * sizeof(double) + 2ULL * sizeof(int64_t));
        mergeBytesWritten_ += approxBytes;
    }
}
