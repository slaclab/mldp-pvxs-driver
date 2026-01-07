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

#include <memory>
#include <string>

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
 * - Controller-emitted metrics use the label key <tt>reader</tt>.
 * - When forwarding a batch, the controller sets <tt>reader</tt> to the
 *   source name (the key in the event batch map).
 * - If a failure happens before a specific source is known (e.g. provider
 *   not registered, stream creation failure), the controller uses
 *   <tt>reader="unknown"</tt>.
 *
 * Bus metrics:
 * - <tt>mldp_pvxs_driver_bus_payload_bytes_total</tt>:
 *   incremented for each successful gRPC <tt>Write()</tt> by the protobuf
 *   payload size (<tt>request.ByteSizeLong()</tt>). This is payload-only and
 *   does not include gRPC/HTTP2/TLS overhead.
 * - <tt>mldp_pvxs_driver_bus_payload_bytes_per_second</tt>:
 *   gauge set after a successful batch finishes, computed as
 *   <tt>payload_bytes_in_batch / elapsed_seconds</tt> for each reader/source.
 * - <tt>mldp_pvxs_driver_bus_push_total</tt>:
 *   incremented by the number of accepted events written for each
 *   reader/source.
 * - <tt>mldp_pvxs_driver_bus_failure_total</tt>:
 *   incremented when a streaming write/finish operation fails.
 *
 * Typical lifecycle:
 * 1. Construct the controller with the parsed driver configuration YAML.
 * 2. Call @ref start to spin up the worker threads and reader instances.
 * 3. Readers push events via @ref push; the controller forwards them to MLDP.
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
     * optional batch tags. The controller schedules the resulting gRPC call on
     * the shared thread pool so readers are not blocked by network latency and
     * so multiple updates can be forwarded together. Network errors
     * are logged and reported via metrics.
     */
    bool push(EventBatch batch_values) override;

    /**
     * @brief Access the shared metrics collector.
     *
     * Returned reference remains valid for the lifetime of the controller.
     */
    metrics::Metrics& metrics() const;

private:
    std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> logger_;         ///< Logger instance for controller logging.
    MLDPPVXSControllerConfig                              config_;         ///< Typed controller configuration.
    std::shared_ptr<BS::light_thread_pool>                thread_pool_;    ///< Shared worker pool executing bus pushes.
    std::shared_ptr<metrics::Metrics>                     metrics_;        ///< Shared metrics collector/exposer.
    bool                                                  running_{false}; ///< Tracks controller lifecycle state.
    util::pool::MLDPGrpcPool::MLDPGrpcPoolShrdPtr         mldp_pool_;      ///< MLDP gRPC connection pool.
    std::vector<reader::ReaderUPtr>                       readers_;        ///< Ingestion readers instance.
    std::string                                           provider_id_;    ///< Provider identifier assigned by MLDP.

    explicit MLDPPVXSController(const config::Config& config);
    void pushImpl(EventBatch batch_values);
};

} // namespace mldp_pvxs_driver::controller
