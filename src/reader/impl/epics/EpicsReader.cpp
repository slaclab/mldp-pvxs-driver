
#include <chrono>
#include <reader/impl/epics/EpicsReader.h>

#include <spdlog/spdlog.h>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace pvxs::client;

using MldpDriverConfig = mldp_pvxs_driver::config::Config;

EpicsReader::EpicsReader(std::shared_ptr<IEventBusPush> bus, const MldpDriverConfig& cfg)
    : Reader(std::move(bus)), running_(false)
{

    running_ = true;
    pva_context_ = pvxs::client::Context::fromEnv();
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

void EpicsReader::addPV(const PVSet& pvNames)
{
    for (const auto& pv : pvNames)
    {
        m_pva_subscriptions.push(pva_context_.monitor(pv)
                                     .event([this](const pvxs::client::Subscription& s)
                                            {
                                                m_pva_workqueue.push(s.shared_from_this());
                                            })
                                     .exec());
    }
}

void EpicsReader::run(int timeout)
{
    bool expired = false;
    const auto start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    while (running_ && !expired)
    {
        auto sub = m_pva_workqueue.pop();
        try
        {
            pvxs::Value update = sub->pop();
            if (!update)
            {
                continue;
            }
            spdlog::info("Received update for PV {}", sub->name());
        }
        catch (const pvxs::client::RemoteError& e)
        {
            spdlog::error("Server error when reading PV {}: {}", sub->name(), e.what());
        }

        if (timeout > 0)
        {
            if (
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()) - start;
                elapsed.count() > timeout)
            {
                expired = true;
            }
        }
    }
}