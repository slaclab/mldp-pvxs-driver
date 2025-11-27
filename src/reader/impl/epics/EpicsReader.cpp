#include <config/Config.h>
#include <reader/impl/epics/EpicsReader.h>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::reader::impl::epics;

EpicsReader::EpicsReader(std::shared_ptr<IEventBusPush> bus, const Config& cfg)
    : Reader(std::move(bus)), running_(false)
{

    running_ = true;
    worker_ = std::thread([this]
                          {
                              // Empty loop — just keep thread alive
                              while (running_)
                              {
                                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
                              }
                          });
}

EpicsReader::~EpicsReader()
{
    running_ = false;
    if (worker_.joinable())
        worker_.join();
}

std::string EpicsReader::name() const
{
    return name_;
}
