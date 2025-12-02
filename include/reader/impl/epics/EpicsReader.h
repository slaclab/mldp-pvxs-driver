#pragma once

#include <config/Config.h>
#include <reader/Reader.h>
#include <reader/ReaderFactory.h>
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
 * @brief Reader implementation tailored for pushing EPICS records onto the event bus.
 *
 * It owns a worker thread that watches EPICS records defined in the provided configuration.
 */
class EpicsReader : public Reader
{
public:
    /**
     * @brief Create an EPICS reader that publishes via \p bus and watches the provided \p cfg.
     * @param bus Event bus that receives EPICS updates.
     * @param cfg Configuration listing the PVs to monitor along with polling intervals or filters.
     */
    EpicsReader(std::shared_ptr<mldp_pvxs_driver::util::bus::IEventBusPush> bus, const mldp_pvxs_driver::config::Config& cfg);

    /** @brief Stop the worker thread and release EPICS resources. */
    ~EpicsReader();

    /** @brief Return the configured EPICS reader name. */
    std::string name() const override;
    /** @brief Main loop that processes EPICS updates and pushes them onto the event bus.
     * @param timeout Maximum time in milliseconds to run before returning. A negative value means run indefinitely.
     */
    void run (int timeout) override;



private:
    std::string                                                 name_;
    std::atomic<bool>                                           running_;
    std::thread                                                 worker_;
    pvxs::client::Context                                       pva_context_;
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pva_subscriptions;
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pva_workqueue;

    /**
     * @brief Add the provided PV names to the list of monitored EPICS records.
     * @param pvNames Set of PV names to monitor.
     */    
    void addPV(const PVSet& pvNames);

    /** @brief Automatically registers this reader type in the factory. */
    REGISTER_READER("epics", EpicsReader)
};
} // namespace mldp_pvxs_driver::reader::impl::epics
