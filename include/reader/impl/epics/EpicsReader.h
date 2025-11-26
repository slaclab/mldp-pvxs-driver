#pragma once

#include <config/Config.h>
#include <bus/IEventBusPush.h>
#include <reader/Reader.h>
#include <reader/ReaderFactory.h>

#include <atomic>
#include <string>
#include <thread>

namespace mldp_pvxs_driver::reader::impl::epics {
class EpicsReader : public Reader
{
public:
    EpicsReader(std::shared_ptr<mldp_pvxs_driver::bus::IEventBusPush> bus, const mldp_pvxs_driver::config::Config& cfg);
    ~EpicsReader();
    
    std::string name() const override;

private:
    std::string       name_;
    std::atomic<bool> running_;
    std::thread       worker_;

    // Automatically registers this reader type in the factory
    REGISTER_READER("epics", EpicsReader)
};
} // namespace mldp_pvxs_driver::reader::impl::epics
