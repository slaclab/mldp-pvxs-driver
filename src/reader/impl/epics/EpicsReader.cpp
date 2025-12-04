
#include <chrono>
#include <reader/impl/epics/EpicsMLDPConversion.h>
#include <reader/impl/epics/EpicsReader.h>
#include <spdlog/spdlog.h>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace pvxs::client;

using MldpDriverConfig = mldp_pvxs_driver::config::Config;

EpicsReader::EpicsReader(std::shared_ptr<IEventBusPush> bus, const MldpDriverConfig& cfg)
    : Reader(std::move(bus)), config_(EpicsReaderConfig(cfg)), name_(config_.name()), running_(false)
{

    running_ = true;
    pva_context_ = pvxs::client::Context::fromEnv();
    worker_ = std::thread([this]
                          {
                              run(-1);
                          });
    // add all configured pvs
    PVSet pvNames;
    for (const auto& pvConfig : config_.pvs())
    {
        pvNames.insert(pvConfig.name);
    }
    addPV(pvNames);
}

EpicsReader::~EpicsReader()
{
    running_ = false;
    m_pva_workqueue.push(nullptr); // Push a null subscription to unblock the queue
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
        auto pv_mon = pva_context_.monitor(pv)
                          .event([this](const pvxs::client::Subscription& s)
                                 {
                                     m_pva_workqueue.push(s.shared_from_this());
                                 })
                          .exec();
        m_pva_subscriptions.push(pv_mon);
        spdlog::info("Started monitoring PV {} on reader {}", pv, name_);
    }
}

void EpicsReader::run(int timeout)
{
    spdlog::info("EpicsReader worker thread started on reader {}.", name_);
    bool       expired = false;
    const auto start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    while (running_ && !expired)
    {
        auto sub = m_pva_workqueue.pop();
        if (!sub)
            continue; // Skip null subscription used to unblock queue on shutdown
        try
        {
            pvxs::Value update = sub->pop();
            if (!update)
            {
                continue;
            }

            spdlog::trace("Received update for PV {} on reader {}.", sub->name(), name_);
            {
                std::shared_ptr<DataValue> data_value = std::make_shared<DataValue>();
                EpicsMLDPConversion::convertPVToProtoValue(update, data_value.get());
                bus_->push(data_value);
            }
        }
        catch (const pvxs::client::RemoteError& e)
        {
            spdlog::error("Server error when reading PV {} on reader {}: {}", sub->name(), name_, e.what());
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
    spdlog::info("EpicsReader worker thread exiting on reader {}.", name_);
}