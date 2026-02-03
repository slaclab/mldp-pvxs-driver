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
 * @file EpicsReader.h
 * @brief EPICS process variable reader implementation using PVXS protocol.
 *
 * This header defines the `EpicsReader` class, which provides real-time monitoring
 * of EPICS process variables (PVs) via the PVXS (PVAccess) client library. The
 * reader integrates with the multi-loader data pipeline (MLDP) by forwarding
 * PV updates to an event bus for downstream ingestion and processing.
 *
 * @defgroup EpicsReaderGroup EPICS Reader Module
 * @brief Reader implementation for EPICS process variables.
 * @ingroup ReaderModule
 *
 * @see Reader
 * @see ReaderFactory
 * @see IEventBusPush
 */

#pragma once

#include <config/Config.h>
#include <reader/Reader.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics/EpicsReaderConfig.h>
#include <util/bus/IEventBusPush.h>

#include <BS_thread_pool.hpp>
#include <pvxs/client.h>
#include <pvxs/nt.h>

#include <util/log/Logger.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <reader/impl/epics/EpicsBaseMonitorPoller.h>

namespace mldp_pvxs_driver::reader::impl::epics {

using PVSet = std::set<std::string>;

/**
 * @class EpicsReader
 * @brief Reader implementation for monitoring EPICS process variables via PVXS.
 *
 * @ingroup EpicsReaderGroup
 *
 * The `EpicsReader` class provides a concrete implementation of the `Reader` interface
 * for subscribing to and monitoring EPICS process variables using the PVXS
 * (PVAccess eXtensible Server) client library. It processes subscription updates
 * from the PVXS infrastructure and publishes them to an event bus for downstream
 * consumption by the MLDP ingestion pipeline.
 *
 * ## Features
 * - **PVXS Integration**: Uses PVXS client library for PV subscriptions and
 *   multi-protocol support (PVAccess, Channel Access).
 * - **Asynchronous Event Processing**: Spawns a dedicated worker thread for
 *   non-blocking event processing and forwarding.
 * - **Thread-Safe Queuing**: Leverages PVXS MPMCFIFO (multi-producer, multi-consumer
 *   first-in-first-out) queues for safe inter-thread communication.
 * - **Special PV Modes**: Supports custom handling for different PV data structures
 *   (e.g., NtTable with configurable timestamp fields).
 * - **Configurable Timeouts**: Allows run-loop execution with fixed duration or
 *   indefinite execution based on caller requirements.
 *
 * ## Lifecycle
 * 1. **Construction**: Initializes the PVXS client context and configuration.
 *    No subscriptions are created until `run()` is called.
 * 2. **Execution**: The `run(timeout)` method drives the main event-processing
 *    loop, handling incoming PV updates and publishing them to the event bus.
 * 3. **Cleanup**: The destructor signals thread termination, joins the worker thread,
 *    and releases all PVXS subscriptions and context resources.
 *
 * ## Thread Safety
 * - The worker thread is created by the constructor and terminated by the destructor.
 * - Subscriptions and work items are enqueued in thread-safe MPMCFIFO queues.
 * - The `running_` flag is an atomic boolean for safe signaling.
 * - PVXS library calls are serialized within the worker thread context.
 *
 * ## Configuration
 * The reader is configured via `EpicsReaderConfig`, which specifies:
 * - The reader's identifying name
 * - The set of EPICS PV names to monitor
 * - Any additional reader-specific options
 *
 * ## Registration
 * The `EpicsReader` is automatically registered with the `ReaderFactory` under
 * the type name `"epics"` via the `REGISTER_READER` macro. Instances can be
 * created through the factory interface.
 *
 * ## Example Usage
 * ```cpp
 * auto bus = std::make_shared<EventBus>();
 * auto metrics = std::make_shared<Metrics>();
 * auto cfg = Config::loadFromFile("config.yaml");
 * auto reader = std::make_shared<EpicsReader>(bus, metrics, cfg);
 * reader->run(-1);  // Run indefinitely
 * ```
 */
class EpicsReader : public Reader
{
public:
    /**
     * @brief Construct an EPICS reader instance.
     *
     * Initializes the PVXS client context and prepares to monitor the PVs
     * specified in the configuration. Spawns a dedicated worker thread that
     * will process subscription events asynchronously. The worker thread runs
     * until `run()` is called and processes events from the internal work queue.
     *
     * @param bus Shared pointer to the event bus where decoded EPICS updates
     *            will be published. Must not be null. The bus is used to forward
     *            PV updates to downstream MLDP ingestion components.
     * @param metrics Shared pointer to the metrics collection object for tracking
     *                reader performance and event statistics. May be null if metrics
     *                collection is disabled.
     * @param cfg Configuration object containing reader settings such as the
     *            reader name and the list of EPICS PV identifiers to monitor.
     *
     * @throws std::invalid_argument if the bus pointer is null.
     * @throws std::runtime_error if the PVXS client context cannot be initialized.
     *
     * @see run()
     * @see ~EpicsReader()
     */
    EpicsReader(std::shared_ptr<mldp_pvxs_driver::util::bus::IEventBusPush> bus,
                std::shared_ptr<mldp_pvxs_driver::metrics::Metrics>         metrics,
                const mldp_pvxs_driver::config::Config&                     cfg);

    /**
     * @brief Destructor stops the reader and releases PVXS resources.
     *
     * Signals the worker thread to terminate by setting the `running_` flag
     * to false, joins the thread to ensure graceful shutdown, and releases all
     * PVXS subscriptions and client context resources. This method ensures no
     * dangling subscriptions or threads remain after the reader is destroyed.
     *
     * @note This is safe to call from any thread, but typically called implicitly
     *       when a shared_ptr<EpicsReader> goes out of scope.
     *
     * @see run()
     */
    ~EpicsReader();

    /**
     * @brief Return the configured reader name.
     *
     * Retrieves the reader's identifying name from the configuration, which is
     * used for logging, debugging, and identification in multi-reader setups.
     * This name is typically set during configuration and uniquely identifies
     * the reader instance within the MLDP pipeline.
     *
     * @return std::string The reader's display name (e.g., "epics_reader_1").
     *
     * @see EpicsReaderConfig
     */
    std::string name() const override;

private:
    /**
     * @struct PVRuntimeConfig
     * @brief Per-process-variable runtime configuration and mode settings.
     *
     * Stores mode-specific configuration for individual EPICS process variables,
     * allowing specialized handling based on the data structure and format of
     * the PV's values. Currently supports default mode and NtTable mode with
     * custom timestamp field extraction.
     *
     * @see pvRuntimeByName_
     */
    struct PVRuntimeConfig
    {
        /**
         * @enum Mode
         * @brief Enumeration of supported PV data structure modes.
         *
         * @var Default
         *      Standard PV value handling with no special processing.
         * @var NtTableRowTs
         *      Special mode for NtTable structures with per-row timestamps.
         *      Requires custom extraction of timestamp fields from table rows.
         */
        enum class Mode
        {
            Default,      ///< Standard PV value handling
            NtTableRowTs, ///< NtTable with per-row timestamp handling
        };

        Mode        mode = Mode::Default;        ///< Current processing mode for this PV
        std::string tsSecondsField;              ///< Field name for timestamp seconds
        std::string tsNanosField;                ///< Field name for timestamp nanoseconds
    };

    enum class Backend
    {
        Pvxs,
        EpicsBase
    };

    /// @brief Process a single PV update event (runs in the reader thread pool).
    void processEvent(std::string pvName, pvxs::Value epics_value);

    /// @brief Process a single EPICS Base PV update event (runs in the reader thread pool).
    void processEvent(std::string pvName, ::epics::pvData::PVStructurePtr epics_value);

    void drainEpicsBaseQueue();

    /// @brief Logger instance for diagnostic and error messages.
    std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> logger_;

    /**
     * @brief Reader-specific configuration (name, PV list, etc.).
     *
     * Contains configuration parameters loaded from the pipeline configuration
     * file, including the reader's name and the list of PVs to monitor.
     *
     * @see EpicsReaderConfig
     */
    EpicsReaderConfig config_;

    /**
     * @brief Cached reader name from configuration.
     *
     * Stores the reader name for quick access without needing to look it up
     * from the configuration object on every `name()` call.
     */
    std::string name_;

    /**
     * @brief Atomic flag indicating whether the reader is actively running.
     *
     * When set to false, signals the pool tasks to stop. Used for safe
     * thread communication across the reader's lifecycle.
     */
    std::atomic<bool> running_;

    /**
     * @brief Thread pool for processing PV update events.
     *
     * Replaces the single worker thread, allowing concurrent conversion
     * of EPICS values to protobuf payloads.
     */
    std::shared_ptr<BS::light_thread_pool> reader_pool_;

    /**
     * @brief PVXS client context for creating subscriptions.
     *
     * The context manages the PVXS protocol stack and handles all low-level
     * communication with EPICS PV servers. Created during construction.
     */
    pvxs::client::Context pva_context_;

    /**
     * @brief Queue holding active PVXS subscriptions.
     *
     * Thread-safe MPMCFIFO queue storing shared pointers to all active
     * PVXS subscriptions. This ensures subscriptions remain alive and
     * continue generating events while in the queue.
     */
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pva_subscriptions;

    Backend backend_{Backend::Pvxs};

    std::unique_ptr<EpicsBaseMonitorPoller> epics_base_poller_;
    std::mutex                              epics_base_drain_mutex_;

    /**
     * @brief Fast per-PV lookup for special handling and configuration.
     *
     * Hash map providing O(1) lookup of mode-specific configuration for each
     * monitored PV. Allows different PVs to have different processing modes
     * (e.g., NtTable vs. standard) based on their data structure.
     */
    std::unordered_map<std::string, PVRuntimeConfig> pvRuntimeByName_;

    /**
     * @brief Subscribe to a set of EPICS process variables.
     *
     * Establishes PVXS subscriptions for each PV name in the provided set and
     * stores them in the internal subscription queue (`m_pva_subscriptions`).
     * When subscription events arrive from the PVXS infrastructure, they are
     * automatically enqueued to the work queue (`m_pva_workqueue`) for processing
     * by the `run()` loop.
     *
     * Each subscription is configured with a callback that enqueues received
     * events into the work queue, allowing asynchronous processing outside the
     * PVXS callback context.
     *
     * @param pvNames Set of EPICS process variable names to monitor. Each name
     *                should be a valid EPICS PV identifier accessible via the
     *                configured PVXS client context. Invalid or unreachable PVs
     *                will generate connection errors logged at debug level.
     *
     * @note This method is called during `run()` initialization to set up
     *       initial subscriptions. It can also be used to add additional
     *       PVs to an existing reader instance.
     *
     * @see m_pva_subscriptions
     * @see m_pva_workqueue
     * @see run()
     */
    void addPV(const PVSet& pvNames);

    /**
     * @brief Automatically registers this reader with the reader factory.
     *
     * This macro expands to a static `ReaderRegistrator` instance that
     * registers the `EpicsReader` class with the `ReaderFactory` at program
     * startup. Once registered, callers can instantiate this reader via
     * `ReaderFactory::create("epics", bus, metrics, cfg)`.
     *
     * The registration is performed using a static object that executes its
     * constructor during module initialization, before main() is called.
     *
     * @see ReaderFactory
     * @see Reader
     */
    REGISTER_READER("epics", EpicsReader)
};
} // namespace mldp_pvxs_driver::reader::impl::epics
