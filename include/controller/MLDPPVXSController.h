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
#include <reader/Reader.h>
#include <util/bus/IEventBusPush.h>
#include <util/log/Logger.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::controller {

/**
 * @brief High-level orchestrator wiring EPICS readers, the event bus, and MLDP.
 *
 * The controller owns the global thread pool, maintains the MLDP gRPC
 * connection pool, and implements the @ref util::bus::IEventBusPush interface
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
 *   labeled with <tt>worker=INDEX</tt>. Useful for detecting hot-partition
 *   imbalance under hash-based routing.
 *
 * Typical lifecycle:
 * 1. Construct the controller with the parsed driver configuration YAML.
 * 2. Call @ref start to spin up the worker threads and reader instances.
 * 3. Readers push events via @ref push; the controller enqueues per-source
 *    items and worker threads batch them into gRPC streams.
 * 4. Call @ref stop to halt the workers and tear down resources.
 */
class MLDPPVXSController : public util::bus::IEventBusPush, public std::enable_shared_from_this<MLDPPVXSController>
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

    /// Start reader orchestration and overall control loop.
    void start();

    /// Stop all workers and release owned resources.
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
     * @brief Access the shared metrics collector.
     *
     * Returned reference remains valid for the lifetime of the controller.
     */
    metrics::Metrics& metrics() const;

private:
    /**
     * @brief Smallest unit of queued work for ingestion.
     *
     * Each item represents a single source's values plus shared batch metadata
     * (root source and tags). Workers build a per-source request from the item
     * and include tags on that request.
     */
    struct QueueItem
    {
        std::string                                       root_source;
        std::shared_ptr<const std::vector<std::string>>   tags;
        std::string                                       src_name;
        std::vector<util::bus::IEventBusPush::EventValue> events;
    };

    /// Per-worker channel: each worker has its own queue for source-affinity.
    struct WorkerChannel
    {
        std::mutex              mutex;
        std::condition_variable cv;
        std::deque<QueueItem>   items;
        bool                    shutdown{false};
    };

    std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> logger_;          ///< Logger instance for controller logging.
    MLDPPVXSControllerConfig                              config_;          ///< Typed controller configuration.
    std::shared_ptr<BS::light_thread_pool>                thread_pool_;     ///< Shared worker pool executing bus pushes.
    std::shared_ptr<metrics::Metrics>                     metrics_;         ///< Shared metrics collector/exposer.
    std::atomic<bool>                                     running_{false};  ///< Tracks controller lifecycle state.
    util::pool::MLDPGrpcPool::MLDPGrpcPoolShrdPtr         mldp_pool_;       ///< MLDP gRPC connection pool.
    std::vector<reader::ReaderUPtr>                       readers_;         ///< Ingestion readers instance.
    std::string                                           provider_id_;     ///< Provider identifier assigned by MLDP.
    std::vector<std::unique_ptr<WorkerChannel>>           channels_;        ///< Per-worker queues for hash-partitioned dispatch.
    std::atomic<std::size_t>                              queued_items_{0}; ///< Number of queued items.

    explicit MLDPPVXSController(const config::Config& config);
    void workerLoop(std::size_t worker_index);
    bool buildRequest(const QueueItem&                           item,
                      const std::string&                         request_id,
                      dp::service::ingestion::IngestDataRequest& request,
                      std::size_t&                               accepted_events,
                      std::size_t&                               payload_bytes);
    void updateQueueDepthMetric();
};

} // namespace mldp_pvxs_driver::controller
