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

#include <BS_thread_pool.hpp>
#include <config/Config.h>
#include <controller/MLDPPVXSControllerConfig.h>
#include <metrics/Metrics.h>
#include <pool/MLDPGrpcPool.h>
#include <pool/MLDPGrpcQueryPool.h>
#include <reader/Reader.h>
#include <util/bus/IDataBus.h>
#include <util/log/Logger.h>
#include <writer/IWriter.h>

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::controller {

/**
 * @brief High-level orchestrator wiring EPICS readers, the event bus, and MLDP.
 *
 * The controller owns the global thread pool, maintains the MLDP gRPC
 * connection pool, and implements the @ref util::bus::IDataBus interface
 * so readers can submit events to MLDP. It also exposes a shared metrics
 * collector that downstream components can use to report internal statistics.
 *
 * Metrics:
 * The controller publishes Prometheus metrics via the shared
 * @ref metrics::Metrics collector.
 *
 * Tagging:
 * - Controller-emitted metrics use the label key <tt>source</tt>.
 * - When forwarding a batch, the controller sets <tt>source</tt> to the
 *   root source name from the event batch.
 * - If a failure happens before a specific source is known (e.g. provider
 *   not registered, stream creation failure), the controller uses
 *   <tt>source="unknown"</tt>.
 *
 * Reader metrics (emitted by EpicsReader, tagged with <tt>source=PV_NAME</tt>):
 * - <tt>mldp_pvxs_driver_reader_events_received_total</tt>:
 *   incremented for every raw EPICS subscription update received from PVXS,
 *   before any processing or conversion. Allows computing drop rate when
 *   compared with <tt>reader_events_total</tt>.
 * - <tt>mldp_pvxs_driver_reader_events_total</tt>:
 *   incremented after an event is successfully converted and pushed to the
 *   controller bus.
 * - <tt>mldp_pvxs_driver_reader_errors_total</tt>:
 *   incremented on conversion failures or PVXS remote errors.
 * - <tt>mldp_pvxs_driver_reader_processing_time_ms</tt>:
 *   histogram of time spent converting an EPICS PV update to a protobuf
 *   payload (milliseconds).
 * - <tt>mldp_pvxs_driver_reader_queue_depth</tt>:
 *   gauge showing the depth of the reader's internal work queue (PV updates
 *   waiting to be processed).
 * - <tt>mldp_pvxs_driver_reader_pool_queue_depth</tt>:
 *   gauge showing the number of conversion tasks queued in the reader thread
 *   pool awaiting processing.
 *
 * Bus metrics:
 * - <tt>mldp_pvxs_driver_bus_payload_bytes_total</tt>:
 *   incremented for each successful gRPC <tt>Write()</tt> by the protobuf
 *   payload size (<tt>request.ByteSizeLong()</tt>). This is payload-only and
 *   does not include gRPC/HTTP2/TLS overhead.
 * - <tt>mldp_pvxs_driver_bus_payload_bytes_per_second</tt>:
 *   gauge set after a successful batch finishes, computed as
 *   <tt>payload_bytes_in_batch / elapsed_seconds</tt> for each source.
 * - <tt>mldp_pvxs_driver_bus_push_total</tt>:
 *   incremented by the number of accepted events written for each source.
 * - <tt>mldp_pvxs_driver_bus_failure_total</tt>:
 *   incremented when a streaming write/finish operation fails.
 * - <tt>mldp_pvxs_driver_bus_stream_rotations_total</tt>:
 *   incremented each time a gRPC ingestion stream is closed, labeled with
 *   <tt>reason</tt> (idle, max_bytes, max_age, write_failed, shutdown,
 *   threshold).
 *
 * Controller metrics:
 * - <tt>mldp_pvxs_driver_controller_send_time_seconds</tt>:
 *   histogram observing end-to-end time spent sending a batch to MLDP.
 * - <tt>mldp_pvxs_driver_controller_queue_depth</tt>:
 *   gauge showing the aggregate number of queued items across all worker
 *   channels.
 * - <tt>mldp_pvxs_driver_controller_channel_queue_depth</tt>:
 *   gauge showing the number of items queued in each per-worker channel,
 *   labeled with <tt>worker=INDEX</tt>. Useful for detecting load imbalance
 *   under round-robin dispatch.
 *
 * Typical lifecycle:
 * 1. Construct the controller with the parsed driver configuration YAML.
 * 2. Call @ref start to spin up the worker threads and reader instances.
 * 3. Readers push events via @ref push; the controller enqueues per-source
 *    items and worker threads batch them into gRPC streams.
 * 4. Call @ref stop to halt the workers and tear down resources.
 */
class MLDPPVXSController : public util::bus::IDataBus, public std::enable_shared_from_this<MLDPPVXSController>
{
public:
    /**
     * @brief Helper to build controllers owned by shared_ptr.
     *
     * The controller internally relies on @ref shared_from_this, so callers
     * must always manage it through shared ownership.
     */
    static std::shared_ptr<MLDPPVXSController> create(const config::Config& config);

    /**
     * @brief Build a controller using the root driver configuration.
     *
     * The constructor parses the typed controller config, initializes the
     * thread pool, creates the MLDP gRPC pool, and prepares the metrics
     * collector/exposer for later use.
     *
     * @param config Root driver configuration tree.
     */
    /// Ensure worker threads and pools are cleaned up.
    ~MLDPPVXSController() override;

    /**
     * @brief Start the controller and all dependent runtime components.
     *
     * Startup sequence:
     * 1. Register provider and initialize ingestion/query pools.
     * 2. Create per-worker channels and spawn worker loops.
     * 3. Instantiate configured readers (which then publish via @ref push).
     */
    void start();

    /**
     * @brief Stop readers/workers and release runtime resources.
     *
     * Stop is idempotent and performs an ordered shutdown:
     * reject new pushes, clear readers, signal worker shutdown, and wait for
     * worker completion before returning.
     */
    void stop();

    /**
     * @brief Forward a batch of events produced by readers to the MLDP ingestion API.
     *
     * Readers invoke this method with a collection of source/payload pairs and
     * optional batch tags. The controller splits each source into a queue item
     * and returns immediately after enqueueing; worker threads drain the queue,
     * aggregate requests into gRPC streams, and flush based on configured byte
     * and time thresholds. Network errors are logged and reported via metrics.
     */
    bool push(EventBatch batch_values) override;

    /**
     * @brief Query source metadata from MLDP query API for a list of PV names.
     *
     * The method prefers the metadata RPC. If unavailable (older MLDP query
     * servers), it falls back to queryData and derives first/last timestamps
     * from returned bucket timestamp structures.
     */
    std::vector<SourceInfo> querySourcesInfo(const std::set<std::string>& source_names) override;

    /**
     * @brief Query MLDP data values for a set of PV/source names.
     *
     * Retries until @p options.timeout expires because source visibility can
     * be eventually consistent. Returns nullopt on hard failure/timeout.
     */
    std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>> querySourcesData(
        const std::set<std::string>&              source_names,
        const util::bus::QuerySourcesDataOptions& options = util::bus::QuerySourcesDataOptions{}) override;

    /**
     * @brief Access the shared metrics collector.
     *
     * Returned reference remains valid for the lifetime of the controller.
     */
    metrics::Metrics& metrics() const;

private:
    std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> logger_;      ///< Logger instance for controller logging.
    MLDPPVXSControllerConfig                               config_;      ///< Typed controller configuration.
    std::shared_ptr<BS::light_thread_pool>                 thread_pool_; ///< Shared worker pool.
    std::shared_ptr<metrics::Metrics>                      metrics_;     ///< Shared metrics collector/exposer.
    std::atomic<bool>                                      running_{false};
    util::pool::MLDPGrpcQueryPool::MLDPGrpcQueryPoolShrdPtr mldp_query_pool_; ///< MLDP query gRPC connection pool.
    std::vector<reader::ReaderUPtr>                         readers_;    ///< Owned reader instances.
    std::vector<writer::IWriterUPtr>                        writers_;    ///< Fan-out writer instances.

    explicit MLDPPVXSController(const config::Config& config);
};

} // namespace mldp_pvxs_driver::controller
