//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <config/Config.h>
#include <util/log/ILog.h>
#include <writer/IWriter.h>
#include <writer/WriterFactory.h>
#include <writer/hdf5/HDF5FilePool.h>
#include <writer/hdf5/HDF5WriterConfig.h>

#include <H5Cpp.h>
#include <util/bus/DataBatch.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::writer {

/**
 * @brief Writer that stores incoming event batches as HDF5 datasets.
 *
 * One HDF5 file per `root_source` is maintained by an `HDF5FilePool`;
 * file rotation is driven by age and size thresholds in `HDF5WriterConfig`.
 *
 * **Columnar layout** (default, non-NTTable sources):
 * @code
 * / (root)
 * ├── timestamps          int64   ns-since-epoch   shape=(N,) unlimited+chunked
 * ├── <col_name_0>        …       per DataBatch column
 * └── …
 * @endcode
 *
 * **Compound layout** (auto-detected NTTable sources via `EventBatch::is_tabular`):
 * @code
 * / (root)
 * └── <root_source>   compound   shape=(N_rows,) unlimited+chunked
 *       Fields: { col_A: f64, col_B: f64, …, secondsPastEpoch: i64, nanoseconds: i64 }
 * @endcode
 *
 * Internal threading:
 * - **Writer thread**: drains the bounded MPSC queue and calls `appendFrame()`.
 * - **Flush thread**: calls `HDF5FilePool::flushAll()` every `flush-interval-ms`.
 */
class HDF5Writer final : public IWriter
{
    REGISTER_WRITER("hdf5", HDF5Writer)
public:
    /**
     * @brief Factory constructor — parses config from the writer.hdf5 YAML sub-node.
     *
     * Called by the @ref WriterFactory registry. The @p metrics parameter is
     * accepted for interface uniformity but is not used by the HDF5 writer.
     */
    explicit HDF5Writer(const config::Config&             node,
                        std::shared_ptr<metrics::Metrics> metrics = nullptr);

    /**
     * @brief Typed constructor — for direct use and unit tests.
     */
    explicit HDF5Writer(HDF5WriterConfig config);
    /**
     * @brief Destructor — calls stop() if the writer is still running.
     *
     * Blocks until both the writer thread and the flush thread have exited,
     * then closes all open HDF5 file handles via HDF5FilePool::closeAll().
     * Safe to call from any thread.
     */
    ~HDF5Writer() override;

    /**
     * @brief Returns the unique instance name from @ref HDF5WriterConfig::name.
     *
     * Used by the WriterFactory registry to identify this writer instance in
     * log messages and metrics labels.
     */
    std::string name() const override
    {
        return config_.name;
    }

    /**
     * @brief Start the writer thread and the flush thread.
     *
     * Spawns:
     * - **writerThread_**: loops in writerLoop(), dequeuing EventBatches and
     *   writing them to HDF5 via appendFrame() or processTabularBatch().
     * - **flushThread_**: loops in flushLoop(), calling HDF5FilePool::flushAll()
     *   every HDF5WriterConfig::flushInterval milliseconds.
     *
     * @throws std::system_error if thread creation fails.
     * @pre start() must not have been called already.
     */
    void start() override;

    /**
     * @brief Enqueue an event batch for asynchronous HDF5 writing.
     *
     * Caller threads (e.g. data-bus dispatch) call this method.  The batch is
     * placed on the bounded MPSC deque (capacity kQueueCapacity).  If the queue
     * is full the oldest entry is evicted and a warning is logged — this is a
     * back-pressure signal indicating the writer cannot keep up with the
     * incoming event rate.
     *
     * @param batch  Move-only EventBatch produced by the data bus.
     * @return true  always (drops oldest entry instead of blocking on overflow).
     * @note noexcept — all exceptions caught internally; errors logged.
     */
    bool push(util::bus::IDataBus::EventBatch batch) noexcept override;

    /**
     * @brief Signal the writer and flush threads to stop, then join them.
     *
     * Sets stopping_ = true, notifies the queue condition variable so the
     * writer thread wakes and drains remaining entries, waits for both threads,
     * then calls HDF5FilePool::closeAll() to flush and close all HDF5 files.
     *
     * @note noexcept — safe to call from destructors and signal handlers.
     * @note Idempotent: calling stop() more than once is safe.
     */
    void stop() noexcept override;

private:
    using EventBatch = util::bus::IDataBus::EventBatch;

    static constexpr std::size_t kQueueCapacity = 8192;

public:
    /**
     * @brief Runtime type tag for each NTTable column field.
     * Declared public so anonymous-namespace helpers in the .cpp can use
     * HDF5Writer::FieldType without friendship.
     *
     * Values map directly to the HDF5 atomic types used when creating
     * per-column datasets:
     * | Tag      | HDF5 type            | C++ storage              |
     * |----------|----------------------|--------------------------|
     * | Float64  | H5::PredType::NATIVE_DOUBLE | `std::vector<double>`  |
     * | Float32  | H5::PredType::NATIVE_FLOAT  | `std::vector<float>`   |
     * | Int32    | H5::PredType::NATIVE_INT32  | `std::vector<int32_t>` |
     * | Int64    | H5::PredType::NATIVE_INT64  | `std::vector<int64_t>` |
     * | Bool     | H5::PredType::NATIVE_UINT8  | `std::vector<uint8_t>` |
     */
    enum class FieldType
    {
        Float64, ///< 64-bit IEEE 754 double (most NTTable scalar columns).
        Float32, ///< 32-bit IEEE 754 float.
        Int32,   ///< Signed 32-bit integer.
        Int64,   ///< Signed 64-bit integer (used for timestamp fields).
        Bool     ///< Boolean stored as uint8 (0 = false, 1 = true).
    };

private:
    /**
     * @brief Accumulation buffer for one tabular source.
     *
     * Tabular data is stored as one independent 1-D dataset per column, each
     * with its native type, plus separate 1-D timestamp datasets.  Column
     * names are written once to a companion string dataset.  This avoids the
     * HDF5 compound-type object-header size limit that would otherwise reject
     * tables with hundreds of fields, and preserves each column's native type.
     *
     * Layout in the HDF5 file:
     *  /<source>/                       HDF5 group
     *  /<source>/secondsPastEpoch       int64   [N_rows]
     *  /<source>/nanoseconds            int64   [N_rows]
     *  /<source>/<colName>              typed   [N_rows]  (one per column, original type preserved)
     *
     * Columns from the same tabular update round may arrive in multiple
     * EventBatches (split by column-batch-size).  The buffer accumulates all
     * columns until the end_of_batch_group marker arrives, then flushes.
     *
     * Schema is locked after the first flush; subsequent rounds must match
     * (unknown columns warned+skipped; missing columns filled with NaN/0).
     */
    struct TabularBuffer
    {
        // ---- schema (locked after first flush) ----
        bool                                         schemaFixed{false};
        std::vector<std::string>                     colNames; ///< Ordered column names.
        std::unordered_map<std::string, std::size_t> colIndex; ///< colName → column index.

        // ---- per-column variant storage ----
        using ColumnData = std::variant<
            std::vector<double>,
            std::vector<float>,
            std::vector<int32_t>,
            std::vector<int64_t>,
            std::vector<uint8_t> // bool stored as uint8
            >;

        std::vector<ColumnData>                    columns;  // columns[colIdx], one per column
        std::unordered_map<std::string, FieldType> colTypes; // colName -> FieldType enum

        // ---- cross-batch accumulation ----
        std::vector<int64_t> tsSeconds; ///< Row epoch-seconds for current round.
        std::vector<int64_t> tsNanos;   ///< Row nanoseconds for current round.
        std::size_t          rowCount{0};
        int64_t              roundFirstTs{-1}; ///< First-row ns-epoch of current round; -1 = none.

        // ---- warned-once sets ----
        std::unordered_set<std::string> warnedMissing;
        std::unordered_set<std::string> warnedUnknown;
    };

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    HDF5WriterConfig                    config_;  ///< Immutable writer configuration (name, paths, thresholds).
    std::shared_ptr<util::log::ILogger> logger_;  ///< Shared logger; messages tagged with config_.name.
    std::unique_ptr<HDF5FilePool>       pool_;    ///< Manages per-source HDF5 file handles and rotation.

    /**
     * @brief Single entry in the bounded writer queue.
     *
     * batchSeq is a monotonically increasing sequence number assigned by push()
     * and used in lastTsBatchSeq_ to detect whether a timestamp dataset has
     * already been written for this batch (timestamps are shared across all
     * columns of the same NTTable update).
     */
    struct QueueEntry
    {
        uint64_t   batchSeq; ///< Monotonically increasing push() sequence number.
        EventBatch batch;    ///< The event batch to be written; move-only.
    };

    // Queue — shared between caller threads and writerThread_.
    std::mutex              queueMutex_; ///< Guards queue_ and is paired with queueCv_.
    std::condition_variable queueCv_;   ///< Notified by push() on enqueue and stop() on shutdown.
    std::deque<QueueEntry>  queue_;     ///< Bounded MPSC work queue (capacity: kQueueCapacity).
    std::atomic<bool>       stopping_{false};      ///< Set to true by stop(); read by both threads.
    std::atomic<uint64_t>   nextBatchSeq_{0};      ///< Incremented atomically by push() for each enqueue.

    // Accessed only from writerThread_ — no lock needed.
    /// Maps root_source → the batchSeq of the last batch for which timestamps were written.
    /// Prevents duplicate timestamp rows when multiple column batches share one NTTable update.
    std::unordered_map<std::string, uint64_t>      lastTsBatchSeq_;

    /// Maps root_source → per-source accumulation buffer for multi-column NTTable writes.
    std::unordered_map<std::string, TabularBuffer> tabularBuffers_;

    // Worker threads
    std::thread writerThread_; ///< Drains queue_ via writerLoop(); owns all HDF5 write I/O.
    std::thread flushThread_;  ///< Calls flushAll() periodically via flushLoop().

    // -----------------------------------------------------------------------
    // Internal methods — all called only from writerThread_
    // -----------------------------------------------------------------------

    /**
     * @brief Main loop executed by writerThread_.
     *
     * Blocks on queueCv_ until a QueueEntry is available or stopping_ is set.
     * For each dequeued entry, routes to appendFrame() (columnar path) or
     * processTabularBatch() (NTTable path) based on isTabularBatch().
     * Exits when stopping_ == true and the queue is empty (drain-then-stop).
     */
    void writerLoop();

    /**
     * @brief Main loop executed by flushThread_.
     *
     * Sleeps for HDF5WriterConfig::flushInterval milliseconds between each
     * call to HDF5FilePool::flushAll().  Exits when stopping_ == true.
     * Flush occurs once more after stopping_ is set to persist any data
     * written between the last flush and stop().
     */
    void flushLoop();

    /// Returns true if @p batch should be routed to the compound tabular path.
    static bool isTabularBatch(const EventBatch& batch);

    // ---- columnar path (existing) ----

    /**
     * @brief Append one non-tabular DataBatch to the open HDF5 file.
     *
     * Called from writerLoop() for batches that do not carry NTTable data.
     * Each column in @p batch is mapped to a 1-D unlimited+chunked dataset
     * directly under the file root.  A `timestamps` dataset (int64, ns-epoch)
     * is also extended, but only once per batch sequence (@p batchSeq is
     * compared against lastTsBatchSeq_[sourceName]).
     *
     * @param sourceName  Root source identifier (e.g. PV name); used as the
     *                    dataset name prefix and to look up the file in pool_.
     * @param batch       Columnar data to append; must be non-tabular.
     * @param file        Open HDF5 file handle returned by pool_.acquire().
     * @param batchSeq    Monotonic push() sequence number; guards duplicate-ts writes.
     */
    void appendFrame(const std::string&              sourceName,
                     const util::bus::DataBatch&     batch,
                     H5::H5File&                     file,
                     uint64_t                        batchSeq);

    /**
     * @brief Return (or create) a 1-D unlimited dataset with the given name and type.
     *
     * If a dataset named @p name already exists in @p file it is opened and
     * returned.  Otherwise a new dataset is created with:
     * - initial dims  = {0}
     * - maximum dims  = {H5S_UNLIMITED}
     * - chunk size    = {kQueueCapacity} elements
     * - DEFLATE level = config_.compressionLevel (0 = no compression)
     *
     * @param file   Open HDF5 file.
     * @param name   Absolute dataset path (e.g. "/timestamps" or "/pv:ch0").
     * @param dtype  HDF5 atomic type (e.g. H5::PredType::NATIVE_DOUBLE).
     * @return Open dataset handle (shared with the file).
     */
    H5::DataSet ensureDataset(H5::H5File&         file,
                              const std::string&  name,
                              const H5::DataType& dtype);

    /**
     * @brief Return (or create) a 2-D unlimited dataset for array-valued columns.
     *
     * Extends ensureDataset() for columns where each row is a fixed-length
     * array (waveforms, matrices).  The dataset shape is (N_rows, arrayLen)
     * with unlimited rows and a fixed second dimension.
     *
     * @param file      Open HDF5 file.
     * @param name      Absolute dataset path.
     * @param dtype     HDF5 atomic element type.
     * @param arrayLen  Fixed length of the second dimension (number of elements per row).
     * @return Open dataset handle.
     */
    H5::DataSet ensureDataset2D(H5::H5File&         file,
                                const std::string&  name,
                                const H5::DataType& dtype,
                                hsize_t             arrayLen);

    // ---- NTTable 2D matrix path ----

    /**
     * @brief Process one tabular batch: accumulate all frames then flush.
     */
    void processTabularBatch(const QueueEntry& entry);

    /**
     * @brief Accumulate one DataBatch into the tabular buffer.
     *
     * Extracts timestamps (first frame of each round) and column values into
     * @p buf.  Missing columns are filled with NaN at flush time.
     */
    void accumulateTabularFrame(const std::string&              sourceName,
                                const util::bus::DataBatch&     batch,
                                TabularBuffer&                  buf);

    /**
     * @brief Write buffered rows to the per-column HDF5 datasets and clear the buffer.
     *
     * Datasets created on first call (one group per source):
     *   /<source>/secondsPastEpoch  int64    (0,) → (N_rows,)  unlimited
     *   /<source>/nanoseconds       int64    (0,) → (N_rows,)  unlimited
     *   /<source>/<colName>         typed    (0,) → (N_rows,)  unlimited, one per column
     */
    void flushTabularBuffer(const std::string& sourceName,
                            TabularBuffer&     buf,
                            H5::H5File&        file);
};

} // namespace mldp_pvxs_driver::writer
