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

#include <metrics/Metrics.h>
#include <util/log/ILog.h>
#include <writer/BaseQueuedWriter.h>
#include <writer/hdf5/HDF5WriterConfig.h>
#include <writer/hdf5/HDF5WriterMetrics.h>

#include <H5Cpp.h>
#include <util/bus/DataBatch.h>

#include <atomic>
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
 * @brief Queue item for HDF5WriterBase — one EventBatch with sequence number.
 */
struct HDF5QueueItem
{
    uint64_t                        batchSeq;
    util::bus::IDataBus::EventBatch batch;
};

/**
 * @brief Abstract base class for HDF5 writers.
 *
 * Contains all shared logic: the tabular accumulation buffers and the
 * ensureDataset helpers.  Queue, backpressure, and worker-thread management
 * are delegated to BaseQueuedWriter (worker_count=1 is enforced so that the
 * unprotected per-worker state below remains single-threaded).
 *
 * Subclasses implement the five pure-virtual hooks to provide either the
 * per-source (HDF5WriterPerSource) or the merge (HDF5WriterMerge) I/O path.
 */
class HDF5WriterBase : public BaseQueuedWriter<HDF5QueueItem>
{
public:
    /**
     * @brief Runtime type tag for each NTTable column field.
     * Declared public so anonymous-namespace helpers in the .cpp can use
     * HDF5WriterBase::FieldType without friendship.
     */
    enum class FieldType
    {
        Float64, ///< 64-bit IEEE 754 double.
        Float32, ///< 32-bit IEEE 754 float.
        Int32,   ///< Signed 32-bit integer.
        Int64,   ///< Signed 64-bit integer.
        Bool     ///< Boolean stored as uint8 (0 = false, 1 = true).
    };

protected:
    /**
     * @brief Accumulation buffer for one tabular source.
     */
    struct TabularBuffer
    {
        // ---- schema (locked after first flush) ----
        bool                                         schemaFixed{false};
        std::vector<std::string>                     colNames;
        std::unordered_map<std::string, std::size_t> colIndex;

        // ---- per-column variant storage ----
        using ColumnData = std::variant<
            std::vector<double>,
            std::vector<float>,
            std::vector<int32_t>,
            std::vector<int64_t>,
            std::vector<uint8_t> // bool stored as uint8
            >;

        std::vector<ColumnData>                    columns;
        std::unordered_map<std::string, FieldType> colTypes;

        // ---- cross-batch accumulation ----
        std::vector<int64_t> tsSeconds;
        std::vector<int64_t> tsNanos;
        std::size_t          rowCount{0};
        int64_t              roundFirstTs{-1};

        // ---- warned-once sets ----
        std::unordered_set<std::string> warnedMissing;
        std::unordered_set<std::string> warnedUnknown;

        // ---- metadata (captured from first batch for this source) ----
        std::unordered_map<std::string, std::string> pendingMetadata;

        static constexpr std::size_t kMaxWarnedUnknown = 128;
    };

    // -----------------------------------------------------------------------
    // Protected shared state — accessible to subclasses
    // -----------------------------------------------------------------------

    HDF5WriterConfig                            config_;
    std::shared_ptr<util::log::ILogger>         logger_;
    std::unique_ptr<metrics::HDF5WriterMetrics> writerMetrics_;
    std::shared_ptr<metrics::Metrics>           metrics_;

    // Accessed exclusively from the single worker thread — no mutex required
    std::unordered_map<std::string, uint64_t>      lastTsBatchSeq_;
    std::unordered_map<std::string, TabularBuffer> tabularBuffers_;
    std::unordered_set<std::string>                seen_groups_; ///< Sources whose HDF5 group has had metadata attributes written.

    // Set to true in doStop() so subclass destructors can guard stop() calls.
    std::atomic<bool> stopped_{false};

public:
    // -----------------------------------------------------------------------
    // IWriter interface — name / acceptsPayload / supports_multi_root_source
    // -----------------------------------------------------------------------

    std::string name() const override
    {
        return config_.name;
    }

    bool supports_multi_root_source() const noexcept override
    {
        return true;
    }

    bool acceptsPayload(const util::bus::BatchPayload& payload) const noexcept override
    {
        return std::holds_alternative<util::bus::TimeSeriesPayload>(payload);
    }

protected:
    explicit HDF5WriterBase(HDF5WriterConfig                  config,
                            std::shared_ptr<metrics::Metrics> metrics = nullptr);
    ~HDF5WriterBase() override;

    // -----------------------------------------------------------------------
    // BaseQueuedWriter hooks — implemented here
    // -----------------------------------------------------------------------

    std::vector<HDF5QueueItem> toItems(util::bus::IDataBus::EventBatch& batch) override;
    void                       processItem(std::size_t workerIndex, HDF5QueueItem item) override;

    /// Spawns flushThread_ and calls onHDF5Start().
    void doStart() override;

    /// Stops flushThread_, calls onHDF5Stop(), sets stopped_=true.
    void doStop() noexcept override;

    // -----------------------------------------------------------------------
    // Pure-virtual hooks — implemented by each mode subclass
    // -----------------------------------------------------------------------

    /// Write one DataBatch frame; subclass handles file acquisition and byte metrics.
    /// Returns the estimated number of post-conversion bytes written.
    virtual std::size_t writeFrameImpl(const std::string&          source,
                                       const util::bus::DataBatch& frame,
                                       uint64_t                    batchSeq) = 0;

    /// Flush the accumulated tabular buffer for one source.
    virtual void flushTabularBufferImpl(const std::string& source,
                                        TabularBuffer&     buf) = 0;

    /// Flush all open HDF5 files to disk (called periodically by flushThread_).
    virtual void doFlushAll() noexcept = 0;

    /// Called inside doStart(), after flushThread_ is spawned.
    virtual void onHDF5Start() = 0;

    /// Called inside doStop(), after flushThread_ is joined.
    virtual void onHDF5Stop() noexcept = 0;

    // -----------------------------------------------------------------------
    // Protected helpers — available to subclasses
    // -----------------------------------------------------------------------

    H5::DataSet ensureDataset(H5::H5File&         file,
                              const std::string&  name,
                              const H5::DataType& dtype);

    H5::DataSet ensureDataset2D(H5::H5File&         file,
                                const std::string&  name,
                                const H5::DataType& dtype,
                                hsize_t             arrayLen);

    void flushTabularBuffer(const std::string& sourceName,
                            TabularBuffer&     buf,
                            H5::H5File&        file);

private:
    void flushLoop();
    void processTabularBatch(const HDF5QueueItem& entry);
    void accumulateTabularFrame(const std::string&          sourceName,
                                const util::bus::DataBatch& batch,
                                TabularBuffer&              buf);
    static bool isTabularBatch(const util::bus::IDataBus::EventBatch& batch);

    std::thread            flushThread_;
    std::atomic<bool>      flushStopping_{false};
    std::atomic<uint64_t>  nextBatchSeq_{0};

    // Worker-thread-exclusive: last EPICS event timestamp per source for inter-arrival Bps.
    std::unordered_map<std::string, double> lastEventTime_;
};

} // namespace mldp_pvxs_driver::writer
