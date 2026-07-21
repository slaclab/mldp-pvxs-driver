//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/slac_calendar/SlacCalendarReaderConfig.h>

#include <regex>

namespace mldp_pvxs_driver::reader::impl::slac_calendar {

static constexpr auto kNameKey               = "name";
static constexpr auto kBaseUrlKey            = "base-url";
static constexpr auto kExperimentsKey        = "experiments";
static constexpr auto kLookaheadDaysKey      = "lookahead-days";
static constexpr auto kLookbackDaysKey       = "lookback-days";
static constexpr auto kStartDateKey          = "start-date";
static constexpr auto kEndDateKey            = "end-date";
static constexpr auto kRescanIntervalSecKey  = "rescan-interval-sec";
static constexpr auto kConnectTimeoutSecKey  = "connect-timeout-sec";
static constexpr auto kTotalTimeoutSecKey    = "total-timeout-sec";
static constexpr auto kTlsVerifyPeerKey      = "tls-verify-peer";
static constexpr auto kTlsVerifyHostKey      = "tls-verify-host";
static constexpr auto kEventLimitKey         = "event-limit";

SlacCalendarReaderConfig::SlacCalendarReaderConfig(const config::Config& cfg)
{
    parse(cfg);
}

void SlacCalendarReaderConfig::parse(const config::Config& cfg)
{
    if (!cfg.hasChild(kNameKey))
        throw Error("slac-calendar reader: 'name' is required");
    name_ = cfg.get(kNameKey);
    if (name_.empty())
        throw Error("slac-calendar reader: 'name' must not be empty");

    if (!cfg.hasChild(kBaseUrlKey))
        throw Error("slac-calendar reader: 'base-url' is required");
    base_url_ = cfg.get(kBaseUrlKey);
    if (base_url_.empty())
        throw Error("slac-calendar reader: 'base-url' must not be empty");

    if (!cfg.hasChild(kExperimentsKey))
        throw Error("slac-calendar reader: 'experiments' is required");
    const auto exp_nodes = cfg.subConfig(kExperimentsKey);
    if (exp_nodes.empty())
        throw Error("slac-calendar reader: 'experiments' must not be empty");
    for (const auto& node : exp_nodes)
    {
        std::string val;
        node >> val;
        if (!val.empty())
            experiments_.push_back(val);
    }
    if (experiments_.empty())
        throw Error("slac-calendar reader: 'experiments' must contain at least one entry");

    static const std::regex kDateRe(R"(\d{4}-\d{2}-\d{2}(T\d{2}:\d{2}:\d{2}(Z|[+-]\d{2}:\d{2})?)?)");

    if (cfg.hasChild(kStartDateKey))
    {
        const std::string sd = cfg.get(kStartDateKey);
        if (!std::regex_match(sd, kDateRe))
            throw Error("slac-calendar reader: 'start-date' must be YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS, got: " + sd);
        start_date_ = sd;
    }

    if (cfg.hasChild(kEndDateKey))
    {
        if (!start_date_.has_value())
            throw Error("slac-calendar reader: 'end-date' requires 'start-date'");
        const std::string ed = cfg.get(kEndDateKey);
        if (!std::regex_match(ed, kDateRe))
            throw Error("slac-calendar reader: 'end-date' must be YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS, got: " + ed);
        if (ed < *start_date_)
            throw Error("slac-calendar reader: 'end-date' must be >= 'start-date'");
        end_date_ = ed;
    }

    if (end_date_.has_value())
    {
        if (cfg.hasChild(kLookaheadDaysKey) || cfg.hasChild(kLookbackDaysKey) || cfg.hasChild(kRescanIntervalSecKey))
            throw Error("slac-calendar reader: 'end-date' is incompatible with 'lookahead-days', 'lookback-days', and 'rescan-interval-sec'");
    }
    else
    {
        if (!cfg.hasChild(kLookaheadDaysKey))
            throw Error("slac-calendar reader: 'lookahead-days' is required");
        lookahead_days_ = cfg.getInt(kLookaheadDaysKey, 0);
        if (lookahead_days_ <= 0)
            throw Error("slac-calendar reader: 'lookahead-days' must be > 0");

        lookback_days_ = cfg.getInt(kLookbackDaysKey, 1);
        if (lookback_days_ < 0)
            throw Error("slac-calendar reader: 'lookback-days' must be >= 0");
    }

    rescan_interval_sec_ = cfg.getDouble(kRescanIntervalSecKey, 0.0);
    connect_timeout_sec_ = static_cast<long>(cfg.getInt(kConnectTimeoutSecKey, 30));
    total_timeout_sec_   = static_cast<long>(cfg.getInt(kTotalTimeoutSecKey, 60));

    if (total_timeout_sec_ < connect_timeout_sec_)
        throw Error("slac-calendar reader: 'total-timeout-sec' must be >= 'connect-timeout-sec'");

    tls_verify_peer_ = cfg.getBool(kTlsVerifyPeerKey, true);
    tls_verify_host_ = cfg.getBool(kTlsVerifyHostKey, true);
    event_limit_     = cfg.getInt(kEventLimitKey, 1000);

    valid_ = true;
}

} // namespace mldp_pvxs_driver::reader::impl::slac_calendar
