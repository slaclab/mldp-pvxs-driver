#pragma once

#include <config/Config.h>
#include <reader/Reader.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics/EpicsReaderConfig.h>
#include <util/bus/IEventBusPush.h>

#include <pvxs/client.h>
#include <pvxs/nt.h>

#include <atomic>
#include <set>
#include <string>
#include <thread>

namespace mldp_pvxs_driver::reader::impl::epics {

using PVSet = std::set<std::string>;

/**
 * @brief Reader implementation for monitoring EPICS process variables via PVXS.
 *
 * `EpicsReader` subscribes to a set of EPICS PVs (process variables) using the
 * PVXS client library and forwards updates to the event bus. The reader spawns
 * a worker thread that processes subscription events and publishes them to the
 * configured `IEventBusPush` bus so downstream components (e.g., the MLDP
 * ingestion driver) receive timely EPICS data.
 *
 * Configuration and lifecycle:
 * - The constructor accepts a configuration specifying which PVs to monitor
 *   and any reader-specific settings (e.g., name, filters).
 * - `run(timeout)` drives the main event-processing loop. It can be called
 *   with a timeout to run for a fixed duration or indefinitely if `timeout < 0`.
 * - The destructor ensures the worker thread is joined and PVXS resources
 *   (subscriptions, context) are properly released.
 *
 * Thread safety:
 * - The worker thread is spawned by the constructor and stopped by the destructor.
 * - Subscriptions are managed via PVXS MPMCFIFO queues for thread-safe access.
 *
 * Registration:
 * - This reader is registered with the `ReaderFactory` under the type name
 *   `"epics"` via the `REGISTER_READER` macro.
 */
class EpicsReader : public Reader
{
public:
    /**
     * @brief Construct an EPICS reader instance.
     *
     * Initializes the PVXS client context and prepares to monitor the PVs
     * specified in the configuration. The constructor does not start the
     * worker thread; call `run(timeout)` to begin event processing.
     *
     * @param bus Shared pointer to the event bus where decoded EPICS updates
     *            will be published.
     * @param cfg Configuration object containing reader settings such as the
     *            reader name and the list of PVs to monitor.
     */
    EpicsReader(std::shared_ptr<mldp_pvxs_driver::util::bus::IEventBusPush> bus,
                std::shared_ptr<mldp_pvxs_driver::metrics::Metrics>        metrics,
                const mldp_pvxs_driver::config::Config&                    cfg);

    /**
     * @brief Destructor stops the reader and releases PVXS resources.
     *
     * Signals the worker thread to terminate, joins it if running, and
     * cleans up subscriptions and the PVXS client context.
     */
    ~EpicsReader();

    /**
     * @brief Return the configured reader name.
     *
     * The name is obtained from the configuration and is used for logging,
     * debugging, and identification in multi-reader setups.
     *
     * @return std::string The reader's name.
     */
    std::string name() const override;

    /**
     * @brief Main event-processing loop for the EPICS reader.
     *
     * Processes PVXS subscription events from the work queue and publishes
     * decoded updates to the event bus. The loop continues until the
     * specified timeout expires (if `timeout >= 0`) or indefinitely (if
     * `timeout < 0`). The reader can be stopped by the destructor or by
     * signaling the internal `running_` flag.
     *
     * @param timeout Maximum time in milliseconds to run. A negative value
     *                means the loop runs until explicitly stopped.
     */
    void run(int timeout) override;

private:
    /** @brief Reader-specific configuration (name, PV list, etc.). */
    EpicsReaderConfig                                           config_;
    
    /** @brief Cached reader name from configuration. */
    std::string                                                 name_;
    
    /** @brief Flag indicating whether the reader is actively running. */
    std::atomic<bool>                                           running_;
    
    /** @brief Worker thread that processes subscription events. */
    std::thread                                                 worker_;
    
    /** @brief PVXS client context for creating subscriptions. */
    pvxs::client::Context                                       pva_context_;
    
    /** @brief Queue holding active PVXS subscriptions. */
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pva_subscriptions;
    
    /** @brief Work queue of subscription events to be processed by run(). */
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pva_workqueue;

    /**
     * @brief Subscribe to a set of EPICS PVs.
     *
     * Creates PVXS subscriptions for each PV name in the provided set and
     * stores them in the internal subscription queue. Subscription events
     * are pushed to the work queue for processing by the `run()` loop.
     *
     * @param pvNames Set of process variable names to monitor. Each name
     *                should be a valid EPICS PV identifier accessible via
     *                the PVXS client context.
     */
    void addPV(const PVSet& pvNames);

    /**
     * @brief Automatically registers this reader with the factory.
     *
     * This macro expands to a static `ReaderRegistrator` instance that
     * registers the `EpicsReader` class with the `ReaderFactory` under the
     * type name `"epics"`. Callers can instantiate this reader via
     * `ReaderFactory::create("epics", bus, cfg)`.
     */
    REGISTER_READER("epics", EpicsReader)
};
} // namespace mldp_pvxs_driver::reader::impl::epics
