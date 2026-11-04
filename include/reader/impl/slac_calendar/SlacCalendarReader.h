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

#include <config/Config.h>
#include <reader/IReader.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/slac_calendar/SlacCalendarReaderConfig.h>
#include <util/bus/IDataBus.h>
#include <util/http/CurlHttpClient.h>
#include <util/log/ILog.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::metrics {
class Metrics;
}

namespace mldp_pvxs_driver::reader::impl::slac_calendar {

class SlacCalendarReader final : public reader::Reader
{
    REGISTER_READER("slac-calendar", SlacCalendarReader)

public:
    SlacCalendarReader(std::shared_ptr<util::bus::IDataBus> bus,
                       std::shared_ptr<metrics::Metrics>    metrics,
                       const config::Config&                cfg);

    ~SlacCalendarReader() override;

    std::string name() const override { return config_.name(); }

private:
    // Tracks (category|program_name|start|end) -> source calendar already seen in this
    // fetch cycle, so cross-calendar duplicate events can be disambiguated instead of
    // colliding on the server's overlap check.
    using SeenActivationMap = std::unordered_map<std::string, std::string>;

    // An activation built from one calendar event, held back so back-to-back
    // (touching/overlapping) occurrences of the same configuration can be merged into one
    // span before being pushed — the server rejects a new activation whose range merely
    // touches an existing one under the same configurationName+category. Flushed after
    // every fetch window (not just once at the end) so activations aren't all delayed
    // until the whole date range has been fetched; only a merge run that is still
    // extendable by a later window is carried forward instead of being pushed.
    struct PendingActivation
    {
        std::string                                category;
        util::bus::ConfigurationActivationPayload  payload;
    };

    void runWorker();
    void runStats();
    void fetchAndPublish(const std::string& startIso, const std::string& endIso);
    std::string fetchAccel(const std::string& accel,
                           const std::string& startIso,
                           const std::string& endIso);
    void        parseAndPush(const std::string& jsonBody, const std::string& accel,
                             SeenActivationMap& seen, std::vector<PendingActivation>& pending);
    void        pushEvent(const nlohmann::json& event, const std::string& accel,
                          SeenActivationMap& seen, std::vector<PendingActivation>& pending);
    std::vector<PendingActivation> mergeAndPushActivations(std::vector<PendingActivation>& pending,
                                                            bool flush_all);

    util::bus::BusTimestamp parseBusTimestamp(const std::string& iso8601);
    std::string buildUrl(const std::string& accel,
                         const std::string& startIso,
                         const std::string& endIso);
    std::string nowOffsetIso(int offsetDays);
    std::string nowIso();
    std::string dateToIso(const std::string& s);
    std::string epochToIso(int64_t epoch);
    std::string extractHtmlInnerText(const std::string& html);

    SlacCalendarReaderConfig              config_;
    std::shared_ptr<util::log::ILogger>   logger_;
    util::http::CurlHttpClient            http_client_;
    std::thread                           worker_thread_;
    std::thread                           stats_thread_;
    std::atomic<bool>                     running_{false};
    std::atomic<bool>                     first_run_{true};
    std::atomic<uint64_t>                 cfg_pushed_{0};
    std::atomic<uint64_t>                 act_pushed_{0};
    std::condition_variable               worker_cv_;
    std::mutex                            worker_mutex_;
};

} // namespace mldp_pvxs_driver::reader::impl::slac_calendar
