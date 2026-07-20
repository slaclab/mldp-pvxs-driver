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
 * @file EpicsArchiverReader.h
 * @brief Reader implementation for SLAC Archiver Appliance API.
 *
 * This header provides the EpicsArchiverReader class, which implements a reader
 * that retrieves archived EPICS data from the SLAC Archiver Appliance.
 */

#pragma once

#include <EPICSEvent.pb.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics_archiver/EpicsArchiverReaderConfig.h>
#include <util/bus/IDataBus.h>
#include <util/log/ILog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mldp_pvxs_driver::util::http {
class IHttpClient;
}

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

/**
 * @brief Pending in-memory batch for one PV when pv-samples-per-batch is enabled.
 */
struct PendingPvBatch
{
    util::bus::DataBatch                         accumulated;      ///< Single DataBatch merging all pending samples for this PV.
    std::unordered_map<std::string, std::string> metadata;         ///< Merged reader+PV metadata for the batch.
    std::string                                  root_source_name; ///< PV name used as root source.
    std::chrono::steady_clock::time_point        created_at;       ///< Wall-clock time of first sample in batch.
};

/**
 * @brief Incremental state while decoding one PB/HTTP chunk.
 *
 * A PB/HTTP chunk begins with a @ref EPICS::PayloadInfo line and is followed by
 * one or more sample lines of the type declared in that header, ending with an
 * empty line. This structure accumulates the parsed header and converted event
 * values until the chunk terminator is reached and the batch is published.
 */
struct PbChunkState
{
    bool                              have_header = false; ///< True after PayloadInfo has been parsed.
    EPICS::PayloadInfo                header;             ///< Payload header for the current chunk.
    std::vector<util::bus::DataBatch> events;             ///< Converted sample batches for this chunk.
};

/**
 * @brief Reader implementation for SLAC Archiver Appliance API.
 *
 * This reader retrieves archived EPICS data from the SLAC Archiver Appliance
 * using the protobuf-based data format (EPICSEvent.proto).
 */
class EpicsArchiverReader : public ::mldp_pvxs_driver::reader::Reader
{
public:
    /**
     * @brief Construct an Archiver reader from configuration.
     *
     * @param bus Event bus for publishing retrieved data.
     * @param metrics Metrics collector for instrumentation (may be null).
     * @param cfg Reader configuration.
     */
    EpicsArchiverReader(std::shared_ptr<util::bus::IDataBus>                  bus,
                        std::shared_ptr<::mldp_pvxs_driver::metrics::Metrics> metrics,
                        const ::mldp_pvxs_driver::config::Config&             cfg);

    /**
     * @brief Destructor - stops reader and releases resources.
     */
    ~EpicsArchiverReader() override;

    /**
     * @brief Get the reader's configured name.
     *
     * @return The name specified in the reader's configuration.
     */
    std::string name() const override;

private:
    struct WorkerContext
    {
        std::unique_ptr<::mldp_pvxs_driver::util::http::IHttpClient> http_client;
        std::unordered_map<std::string, PendingPvBatch>              pending_pv_batches;
        std::map<std::string, std::pair<uint64_t, uint32_t>>         last_published_ns_per_pv;
    };

    class PVWorkQueue
    {
    public:
        void populate(const std::vector<std::string>& pv_names);
        std::optional<std::string> pop();

    private:
        std::mutex              mutex_;
        std::queue<std::string> pvs_;
    };

    std::shared_ptr<util::log::ILogger> logger_;
    std::string                         name_;
    EpicsArchiverReaderConfig           config_;
    std::atomic<bool>                   running_{false};
    mutable std::mutex                  worker_mutex_;
    std::condition_variable             worker_cv_;
    std::exception_ptr                  worker_error_;
    bool                                worker_done_ = false;
    std::vector<std::thread>            worker_threads_;
    std::vector<WorkerContext>          worker_contexts_;
    PVWorkQueue                         pv_queue_;
    std::atomic<std::size_t>            workers_completed_{0};

    std::mutex                          cycle_mutex_;
    std::condition_variable             cycle_cv_;
    std::atomic<std::size_t>            cycle_ready_{0};
    std::string                         cycle_from_;
    std::optional<std::string>          cycle_to_;

    void initializeHttpClients();
    void destroyHttpClients();
    void startWorkers();
    void stopWorkers();
    void runWorker(std::size_t index);

    void fetchSinglePV(WorkerContext& ctx,
                       const std::string& pv,
                       const std::string& from,
                       const std::optional<std::string>& to);

    void flushChunk(WorkerContext& ctx, PbChunkState& state);
    void finalizeChunk(WorkerContext& ctx, PbChunkState& state);
    void parsePbHttpLineIntoState(WorkerContext& ctx, const std::string& line, PbChunkState& state);
    void submitPendingBatch(WorkerContext& ctx, const std::string& pv);
    void flushAllPendingBatches(WorkerContext& ctx);
    void flushExpiredPendingBatches(WorkerContext& ctx);

    bool pushBatch(util::bus::IDataBus::EventBatch batch);

    REGISTER_READER("epics-archiver", EpicsArchiverReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver
