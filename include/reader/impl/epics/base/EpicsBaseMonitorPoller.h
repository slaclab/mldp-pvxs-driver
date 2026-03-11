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
 * @file EpicsBaseMonitorPoller.h
 * @brief Polling-based monitor implementation for EPICS Base (pvaClient).
 *
 * This header provides the EpicsBaseMonitorPoller class, which implements a
 * multi-threaded polling approach for monitoring EPICS PVs using the EPICS Base
 * pvaClient library. Unlike callback-based monitors, this class uses dedicated
 * polling threads to check for new data, which can be more suitable for
 * high-throughput scenarios or when callback semantics are problematic.
 */

#pragma once

#include <pv/configuration.h>
#include <pv/pvData.h>
#include <pv/pvaClient.h>
#include <util/log/Logger.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Multi-threaded polling monitor for EPICS Base PV monitoring.
 *
 * This class provides a polling-based approach to monitoring EPICS Process
 * Variables using the pvaClient library. It spawns dedicated threads that
 * periodically poll monitor queues for new data, collecting updates into
 * an internal queue that can be drained by the owning reader.
 *
 * The polling model offers several advantages:
 * - Decouples EPICS callback threads from application processing
 * - Provides natural batching of updates
 * - Reduces callback overhead in high-update-rate scenarios
 *
 * @note This class is designed for EPICS Base (pvaClient). For PVXS-based
 *       monitoring, see EpicsPVXSReader which uses event-driven subscriptions.
 *
 * @see EpicsBaseReader for the reader that uses this poller
 */
class EpicsBaseMonitorPoller
{
public:
    /**
     * @brief Callback invoked when new data is available in the queue.
     *
     * This notification allows the owner to trigger a drain operation
     * without busy-waiting on the queue.
     */
    using DataHandler = std::function<void()>;

    /**
     * @brief Callback for processing each PV update during drain.
     *
     * @param pv_name The name of the PV that produced the update.
     * @param value The PVStructure containing the update data.
     */
    using DrainHandler = std::function<void(const std::string& pv_name, ::epics::pvData::PVStructurePtr value)>;

    /**
     * @brief Construct a poller for the specified PVs.
     *
     * Creates monitor subscriptions for all specified PVs and starts the
     * polling threads. The poller automatically determines the appropriate
     * provider (pva or ca) for each PV based on naming conventions.
     *
     * @param pv_names List of PV names to monitor.
     * @param poll_threads Number of threads to use for polling.
     * @param poll_interval_ms Interval between poll cycles when no data is available.
     * @param on_data_available Callback invoked when new data enters the queue.
     * @param logger Logger instance for diagnostic output.
     *
     * @throws std::runtime_error if pvaClient initialization fails.
     */
    EpicsBaseMonitorPoller(const std::vector<std::string>&     pv_names,
                           unsigned int                        poll_threads,
                           unsigned int                        poll_interval_ms,
                           DataHandler                         on_data_available,
                           std::shared_ptr<util::log::ILogger> logger);

    /**
     * @brief Destructor - stops polling threads and releases resources.
     *
     * Signals all polling threads to stop and waits for them to complete.
     * Any remaining queued items are discarded.
     */
    ~EpicsBaseMonitorPoller();

    /** @brief Copy constructor (deleted - poller is not copyable). */
    EpicsBaseMonitorPoller(const EpicsBaseMonitorPoller&) = delete;

    /** @brief Copy assignment (deleted - poller is not copyable). */
    EpicsBaseMonitorPoller& operator=(const EpicsBaseMonitorPoller&) = delete;

    /**
     * @brief Drain all queued updates through the provided handler.
     *
     * Removes all items from the internal queue, invoking the handler for
     * each one. This method is thread-safe and can be called concurrently
     * with the polling threads.
     *
     * @param handler Callback invoked for each queued update.
     *
     * @note The handler is invoked while holding the queue lock; keep
     *       processing lightweight or move data out for deferred processing.
     */
    void drain(const DrainHandler& handler);

private:
    ::epics::pvaClient::PvaClientPtr pva_client_; ///< Shared pvaClient instance for creating channels and monitors.
    ::epics::pvaClient::PvaClientPtr ca_client_;  ///< Shared caClient instance for Channel Access (if needed).

    /**
     * @brief Internal representation of a PV subscription.
     */
    struct Subscription
    {
        std::string                             pv_name;  ///< Name of the monitored PV.
        ::epics::pvaClient::PvaClientChannelPtr channel;  ///< pvaClient channel handle.
        ::epics::pvaClient::PvaClientMonitorPtr monitor;  ///< pvaClient monitor handle.
        std::string                             provider; ///< Provider name ("pva" or "ca").
    };

    /**
     * @brief Item stored in the internal update queue.
     */
    struct QueueItem
    {
        std::string                     pv_name; ///< Source PV name.
        ::epics::pvData::PVStructurePtr value;   ///< Update payload.
    };

    /**
     * @brief Result of provider resolution for a PV.
     */
    struct ProviderSelection
    {
        std::string provider; ///< Selected provider ("pva" or "ca").
        std::string pv_name;  ///< Possibly modified PV name.
    };

    /**
     * @brief Determine the appropriate provider for a PV name.
     *
     * Examines the PV name to determine whether to use PV Access ("pva")
     * or Channel Access ("ca") provider. Names starting with "ca://" use
     * Channel Access; others default to PV Access.
     *
     * @param pv The PV name to analyze.
     * @return ProviderSelection with the chosen provider and cleaned PV name.
     */
    static ProviderSelection resolveProviderForPv(const std::string& pv);

    /**
     * @brief Main loop for polling threads.
     *
     * Each polling thread is responsible for a subset of subscriptions,
     * determined by thread_index and thread_count. The thread polls its
     * assigned monitors, queues any received data, and notifies via the
     * data handler callback.
     *
     * @param thread_index Index of this thread (0-based).
     * @param thread_count Total number of polling threads.
     */
    void pollerLoop(unsigned int thread_index, unsigned int thread_count);

    std::atomic<bool>                   running_{true};       ///< Flag to signal thread shutdown.
    unsigned int                        poll_interval_ms_{5}; ///< Sleep interval when idle.
    DataHandler                         on_data_available_;   ///< New data notification callback.
    std::shared_ptr<util::log::ILogger> logger_;              ///< Logger instance.

    std::vector<Subscription> subscriptions_;  ///< All active subscriptions.
    std::vector<std::thread>  poller_threads_; ///< Polling thread handles.

    std::mutex            queue_mutex_; ///< Protects the update queue.
    std::deque<QueueItem> queue_;       ///< Queued updates awaiting drain.
};

} // namespace mldp_pvxs_driver::reader::impl::epics
