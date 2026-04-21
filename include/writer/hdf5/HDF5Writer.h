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
#include <common.pb.h>

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
 * ├── <col_name_0>        …       per DataFrame col
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
    ~HDF5Writer() override;

    std::string name() const override
    {
        return config_.name;
    }

    void start() override;
    bool push(util::bus::IDataBus::EventBatch batch) noexcept override;
    void stop() noexcept override;

private:
    using EventBatch = util::bus::IDataBus::EventBatch;

    static constexpr std::size_t kQueueCapacity = 8192;

public:
    /**
     * @brief Runtime type tag for each NTTable column field.
     * Declared public so anonymous-namespace helpers in the .cpp can use
     * HDF5Writer::FieldType without friendship.
     */
    enum class FieldType
    {
        Float64,
        Float32,
        Int32,
        Int64,
        Bool
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

    HDF5WriterConfig                    config_;
    std::shared_ptr<util::log::ILogger> logger_;
    std::unique_ptr<HDF5FilePool>       pool_;

    struct QueueEntry
    {
        uint64_t   batchSeq;
        EventBatch batch;
    };

    // Queue — shared between caller threads and writerThread_.
    std::mutex              queueMutex_;
    std::condition_variable queueCv_;
    std::deque<QueueEntry>  queue_;
    std::atomic<bool>       stopping_{false};
    std::atomic<uint64_t>   nextBatchSeq_{0};

    // Accessed only from writerThread_ — no lock needed.
    std::unordered_map<std::string, uint64_t>      lastTsBatchSeq_;
    std::unordered_map<std::string, TabularBuffer> tabularBuffers_;

    // Worker threads
    std::thread writerThread_;
    std::thread flushThread_;

    // -----------------------------------------------------------------------
    // Internal methods — all called only from writerThread_
    // -----------------------------------------------------------------------

    void writerLoop();
    void flushLoop();

    /// Returns true if @p batch should be routed to the compound tabular path.
    static bool isTabularBatch(const EventBatch& batch);

    // ---- columnar path (existing) ----

    void appendFrame(const std::string&                    sourceName,
                     const dp::service::common::DataFrame& frame,
                     H5::H5File&                           file,
                     uint64_t                              batchSeq);

    H5::DataSet ensureDataset(H5::H5File&         file,
                              const std::string&  name,
                              const H5::DataType& dtype);

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
     * @brief Accumulate one DataFrame into the tabular buffer.
     *
     * Extracts timestamps (first frame of each round) and column values into
     * @p buf.  Missing columns are filled with NaN at flush time.
     */
    void accumulateTabularFrame(const std::string&                    sourceName,
                                const dp::service::common::DataFrame& frame,
                                TabularBuffer&                        buf);

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
