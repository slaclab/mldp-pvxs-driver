#pragma once

#include <config/Config.h>
#include <util/bus/IEventBusPush.h>
#include <reader/Reader.h>
#include <reader/ReaderFactory.h>

#include <atomic>
#include <string>
#include <thread>

namespace mldp_pvxs_driver::reader::impl::epics {
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

private:
    std::string       name_;
    std::atomic<bool> running_;
    std::thread       worker_;

    /** @brief Automatically registers this reader type in the factory. */
    REGISTER_READER("epics", EpicsReader)
};
} // namespace mldp_pvxs_driver::reader::impl::epics
