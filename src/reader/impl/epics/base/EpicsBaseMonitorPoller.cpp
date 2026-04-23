//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/base/EpicsBaseMonitorPoller.h>

#include <algorithm>
#include <chrono>

using namespace mldp_pvxs_driver::reader::impl::epics;

namespace {
constexpr std::string_view kPvaPrefix{"pva://"};
constexpr std::string_view kCaPrefix{"ca://"};
} // namespace

EpicsBaseMonitorPoller::EpicsBaseMonitorPoller(const std::vector<std::string>&     pv_names,
                                               unsigned int                        poll_threads,
                                               unsigned int                        poll_interval_ms,
                                               DataHandler                         on_data_available,
                                               std::shared_ptr<util::log::ILogger> logger)
    : pva_client_(::epics::pvaClient::PvaClient::get("pva"))
    , ca_client_(::epics::pvaClient::PvaClient::get("ca"))
    , poll_interval_ms_(poll_interval_ms)
    , on_data_available_(std::move(on_data_available))
    , logger_(std::move(logger))
{
    subscriptions_.reserve(pv_names.size());
    for (const auto& pv : pv_names)
    {
        const auto selection = resolveProviderForPv(pv);
        if (selection.provider.empty() || selection.pv_name.empty())
        {
            if (logger_)
            {
                warnf(*logger_, "Skipping PV {} (no provider)", pv);
            }
            continue;
        }
        auto client_ = selection.provider == "pva" ? pva_client_ : ca_client_;
        auto channel = client_->channel(selection.pv_name);
        auto monitor = channel->createMonitor("field(value,alarm,timeStamp)");
        monitor->connect();
        monitor->start();
        subscriptions_.push_back({selection.pv_name, std::move(channel), std::move(monitor), selection.provider});
        if (logger_)
        {
            infof(*logger_, "Started monitoring (epics-base) {}", selection.pv_name);
        }
    }

    if (subscriptions_.empty())
    {
        return;
    }

    const auto thread_count = std::max(1u, poll_threads);
    poller_threads_.reserve(thread_count);
    for (unsigned int i = 0; i < thread_count; ++i)
    {
        poller_threads_.emplace_back([this, i, thread_count]()
                                     {
                                         pollerLoop(i, thread_count);
                                     });
    }
}

EpicsBaseMonitorPoller::~EpicsBaseMonitorPoller()
{
    running_ = false;
    for (auto& t : poller_threads_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    poller_threads_.clear();
}

EpicsBaseMonitorPoller::ProviderSelection EpicsBaseMonitorPoller::resolveProviderForPv(
    const std::string& pv)
{
    if (pv.starts_with(kPvaPrefix))
    {
        return {"pva", pv.substr(kPvaPrefix.size())};
    }
    if (pv.starts_with(kCaPrefix))
    {
        return {"ca", pv.substr(kCaPrefix.size())};
    }
    return {"pva", pv};
}

void EpicsBaseMonitorPoller::pollerLoop(unsigned int thread_index, unsigned int thread_count)
{
    const auto sleep_duration = std::chrono::milliseconds(poll_interval_ms_);
    while (running_.load())
    {
        bool hadData = false;
        for (size_t idx = thread_index; idx < subscriptions_.size(); idx += thread_count)
        {
            auto& sub = subscriptions_[idx];
            bool  subHadData = false;
            while (sub.monitor->poll())
            {
                auto data = sub.monitor->getData();
                auto src = data ? data->getPVStructure() : ::epics::pvData::PVStructurePtr{};
                if (!src)
                {
                    sub.monitor->releaseEvent();
                    break;
                }
                auto dest = ::epics::pvData::getPVDataCreate()->createPVStructure(src->getStructure());
                dest->copy(*src);
                {
                    std::lock_guard<std::mutex> lk(queue_mutex_);
                    queue_.push_back({sub.pv_name, std::move(dest)});
                }
                subHadData = true;
                hadData = true;
                sub.monitor->releaseEvent();
            }
        }

        if (hadData && on_data_available_)
        {
            on_data_available_();
        }
        else if (!hadData)
        {
            std::this_thread::sleep_for(sleep_duration);
        }
    }
}

void EpicsBaseMonitorPoller::drain(const DrainHandler& handler)
{
    if (!handler)
    {
        return;
    }

    std::deque<QueueItem> items;
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        if (queue_.empty())
        {
            return;
        }
        items.swap(queue_);
    }

    for (auto& item : items)
    {
        handler(item.pv_name, std::move(item.value));
    }
}
