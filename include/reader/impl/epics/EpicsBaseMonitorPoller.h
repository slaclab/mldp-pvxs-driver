//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

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

class EpicsBaseMonitorPoller
{
public:
    using DataHandler = std::function<void()>;
    using DrainHandler = std::function<void(const std::string& pv_name, ::epics::pvData::PVStructurePtr value)>;

    EpicsBaseMonitorPoller(const std::vector<std::string>& pv_names,
                           unsigned int                   poll_threads,
                           unsigned int                   poll_interval_ms,
                           DataHandler                    on_data_available,
                           std::shared_ptr<util::log::ILogger> logger);

    ~EpicsBaseMonitorPoller();

    EpicsBaseMonitorPoller(const EpicsBaseMonitorPoller&) = delete;
    EpicsBaseMonitorPoller& operator=(const EpicsBaseMonitorPoller&) = delete;

    void drain(const DrainHandler& handler);

private:
    struct Subscription
    {
        std::string         pv_name;
        ::epics::pvaClient::PvaClientChannelPtr channel;
        ::epics::pvaClient::PvaClientMonitorPtr monitor;
        std::string         provider;
    };

    struct QueueItem
    {
        std::string                      pv_name;
        ::epics::pvData::PVStructurePtr    value;
    };

    struct ProviderSelection
    {
        std::string provider;
        std::string           pv_name;
    };

    static ProviderSelection resolveProviderForPv(const std::string& pv);

    void pollerLoop(unsigned int thread_index, unsigned int thread_count);

    std::atomic<bool> running_{true};
    unsigned int      poll_interval_ms_{5};
    DataHandler       on_data_available_;
    std::shared_ptr<util::log::ILogger> logger_;

    ::epics::pvaClient::PvaClientPtr pva_client_;

    std::vector<Subscription> subscriptions_;
    std::vector<std::thread>  poller_threads_;

    std::mutex                queue_mutex_;
    std::deque<QueueItem>     queue_;
};

} // namespace mldp_pvxs_driver::reader::impl::epics
