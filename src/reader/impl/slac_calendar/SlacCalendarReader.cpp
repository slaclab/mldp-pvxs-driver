//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file   SlacCalendarReader.cpp
 * @brief  Implementation of SlacCalendarReader.
 * @author SLAC MLDP Team
 * @date   2025-01-01
 * @copyright Copyright (c) 2025 SLAC National Accelerator Laboratory
 */

#include <reader/impl/slac_calendar/SlacCalendarReader.h>

#include <util/log/Logger.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

namespace mldp_pvxs_driver::reader::impl::slac_calendar {

using namespace mldp_pvxs_driver::util::http;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::log;

SlacCalendarReader::SlacCalendarReader(std::shared_ptr<IDataBus>         bus,
                                       std::shared_ptr<metrics::Metrics> metrics,
                                       const config::Config&             cfg)
    : reader::Reader(std::move(bus), std::move(metrics))
    , config_(cfg)
    , logger_(newLogger("reader:slac-calendar:" + config_.name()))
{
    HttpClientOptions opts;
    opts.connect_timeout_sec = config_.connectTimeoutSec();
    opts.total_timeout_sec   = config_.totalTimeoutSec();
    opts.tls.verify_peer     = config_.tlsVerifyPeer();
    opts.tls.verify_host     = config_.tlsVerifyHost();
    http_client_.setDefaultOptions(opts);

    running_ = true;
    worker_thread_ = std::thread([this] { runWorker(); });
}

SlacCalendarReader::~SlacCalendarReader()
{
    {
        std::lock_guard<std::mutex> lk(worker_mutex_);
        running_ = false;
    }
    worker_cv_.notify_all();
    if (worker_thread_.joinable())
        worker_thread_.join();
}

void SlacCalendarReader::runWorker()
{
    do
    {
        try
        {
            std::string start_iso, end_iso;
            if (first_run_.load() && config_.startDate().has_value())
            {
                start_iso = startDateToIso(*config_.startDate());
                end_iso   = nowIso();
            }
            else
            {
                start_iso = nowOffsetIso(-config_.lookbackDays());
                end_iso   = nowOffsetIso(+config_.lookaheadDays());
            }
            first_run_ = false;
            fetchAndPublish(start_iso, end_iso);
        }
        catch (const std::exception& e)
        {
            errorf(*logger_, "SlacCalendarReader '{}' fetch error: {}", config_.name(), e.what());
        }

        if (config_.rescanIntervalSec() <= 0.0)
        {
            signalCompleted();
            break;
        }

        std::unique_lock<std::mutex> lk(worker_mutex_);
        worker_cv_.wait_for(lk,
                            std::chrono::duration<double>(config_.rescanIntervalSec()),
                            [this] { return !running_.load(); });

    } while (running_.load());
}

void SlacCalendarReader::fetchAndPublish(const std::string& startIso, const std::string& endIso)
{
    for (const auto& exp : config_.experiments())
    {
        try
        {
            const auto body = fetchExperiment(exp, startIso, endIso);
            parseAndPush(body, exp);
        }
        catch (const std::exception& e)
        {
            errorf(*logger_,
                   "SlacCalendarReader '{}' experiment '{}' error: {}",
                   config_.name(),
                   exp,
                   e.what());
        }
    }
}

std::string SlacCalendarReader::fetchExperiment(const std::string& experiment,
                                                const std::string& startIso,
                                                const std::string& endIso)
{
    const std::string url = buildUrl(experiment, startIso, endIso);
    HttpRequest       req{url, {}};
    const auto        result = http_client_.get(req);

    if (result.info.http_status != 200)
    {
        throw std::runtime_error("HTTP " + std::to_string(result.info.http_status) +
                                 " from " + url);
    }
    return std::string(result.body.begin(), result.body.end());
}

void SlacCalendarReader::parseAndPush(const std::string& jsonBody, const std::string& experiment)
{
    const auto events = nlohmann::json::parse(jsonBody);
    if (!events.is_array())
        throw std::runtime_error("expected JSON array from SLAC calendar API");

    for (const auto& ev : events)
        pushEvent(ev, experiment);
}

void SlacCalendarReader::pushEvent(const nlohmann::json& ev, const std::string& experiment)
{
    // --- ConfigurationPayload ---
    ConfigurationPayload cfg_payload;
    cfg_payload.root_source_name    = config_.name();
    cfg_payload.configuration_name = ev["program_name"].get<std::string>();
    cfg_payload.category           = ev["calendar"].get<std::string>();

    const std::string desc = ev.value("description", "");
    if (!desc.empty())
        cfg_payload.description = desc;

    if (ev.contains("tags") && !ev["tags"].is_null())
    {
        std::vector<std::string> tags;
        for (const auto& t : ev["tags"])
            tags.push_back(t.get<std::string>());
        if (!tags.empty())
        {
            cfg_payload.tags = tags;
            for (size_t i = 0; i < tags.size(); ++i)
                cfg_payload.attributes["tag_" + std::to_string(i)] = tags[i];
        }
    }

    cfg_payload.attributes["experiment"] = experiment;

    for (const auto& [key, attr] :
         std::vector<std::pair<std::string, std::string>>{
             {"note", "note"}, {"poc", "poc"}, {"config", "config"}, {"machine", "machine"}})
    {
        if (ev.contains(key) && !ev[key].is_null())
            cfg_payload.attributes[attr] = ev[key].get<std::string>();
    }

    if (ev.contains("details") && !ev["details"].is_null())
    {
        const std::string raw = ev["details"].get<std::string>();
        const std::string url = extractHtmlInnerText(raw);
        if (!url.empty())
            cfg_payload.attributes["details"] = url;
    }

    if (ev.contains("hutch") && !ev["hutch"].is_null())
    {
        const auto& h = ev["hutch"];
        if (h.contains("name") && !h["name"].is_null())
            cfg_payload.attributes["hutch_name"] = h["name"].get<std::string>();
        if (h.contains("color") && !h["color"].is_null())
            cfg_payload.attributes["hutch_color"] = h["color"].get<std::string>();
        if (h.contains("line") && !h["line"].is_null())
            cfg_payload.attributes["hutch_line"] = h["line"].get<std::string>();
    }

    {
        IDataBus::EventBatch b;
        b.reader_name = config_.name();
        b.payload     = std::move(cfg_payload);
        bus_->push(std::move(b));
    }

    // --- ConfigurationActivationPayload ---
    ConfigurationActivationPayload act_payload;
    act_payload.client_activation_id = ev["url"].get<std::string>();
    act_payload.configuration_name   = ev["program_name"].get<std::string>();
    act_payload.start_time           = parseBusTimestamp(ev["start"].get<std::string>());
    act_payload.end_time             = parseBusTimestamp(ev["end"].get<std::string>());
    if (!desc.empty())
        act_payload.description = desc;
    if (ev.contains("tags") && !ev["tags"].is_null())
    {
        std::vector<std::string> tags;
        for (const auto& t : ev["tags"])
            tags.push_back(t.get<std::string>());
        if (!tags.empty())
            act_payload.tags = tags;
    }
    act_payload.attributes["experiment"] = experiment;
    act_payload.attributes["calendar"]   = ev["calendar"].get<std::string>();

    {
        IDataBus::EventBatch b;
        b.reader_name = config_.name();
        b.payload     = std::move(act_payload);
        bus_->push(std::move(b));
    }
}

BusTimestamp SlacCalendarReader::parseBusTimestamp(const std::string& iso)
{
    if (iso.size() < 19)
        throw std::runtime_error("timestamp too short: " + iso);

    struct tm t{};
    const char* p = strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &t);
    if (!p)
        throw std::runtime_error("cannot parse timestamp: " + iso);

    long offset_sec = 0;
    if (*p == 'Z')
    {
        offset_sec = 0;
    }
    else if (*p == '+' || *p == '-')
    {
        const int sign = (*p == '+') ? 1 : -1;
        int hh = 0, mm = 0;
        if (sscanf(p + 1, "%d:%d", &hh, &mm) != 2)
            throw std::runtime_error("bad TZ offset in: " + iso);
        offset_sec = sign * (hh * 3600 + mm * 60);
    }
    else
    {
        throw std::runtime_error("missing TZ in: " + iso);
    }

    const time_t epoch = timegm(&t) - offset_sec;
    return BusTimestamp{static_cast<uint64_t>(epoch), 0};
}

std::string SlacCalendarReader::buildUrl(const std::string& experiment,
                                         const std::string& startIso,
                                         const std::string& endIso)
{
    auto encode = [](const std::string& s) {
        std::string out;
        for (char c : s)
        {
            if (c == ':')       out += "%3A";
            else if (c == '+')  out += "%2B";
            else                out += c;
        }
        return out;
    };

    return config_.baseUrl() + "/" + experiment + "/events.json" +
           "?non_program_events=false" +
           "&start_time=" + encode(startIso) +
           "&end_time="   + encode(endIso) +
           "&limit="      + std::to_string(config_.eventLimit());
}

std::string SlacCalendarReader::nowOffsetIso(int offsetDays)
{
    const auto now = std::chrono::system_clock::now();
    const auto tp  = now + std::chrono::hours(24 * offsetDays);
    time_t t = std::chrono::system_clock::to_time_t(tp);
    struct tm lt{};
    localtime_r(&t, &lt);
    char buf[64], tz[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &lt);
    std::strftime(tz, sizeof(tz), "%z", &lt);
    std::string result(buf);
    std::string tzs(tz);
    if (tzs.size() == 5)
        result += tzs.substr(0, 3) + ":" + tzs.substr(3);
    else
        result += tzs;
    return result;
}

std::string SlacCalendarReader::nowIso()
{
    return nowOffsetIso(0);
}

std::string SlacCalendarReader::startDateToIso(const std::string& yyyymmdd)
{
    struct tm t{};
    int y, m, d;
    if (sscanf(yyyymmdd.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
        throw std::runtime_error("bad start-date: " + yyyymmdd);
    t.tm_year  = y - 1900;
    t.tm_mon   = m - 1;
    t.tm_mday  = d;
    t.tm_hour  = 0;
    t.tm_min   = 0;
    t.tm_sec   = 0;
    t.tm_isdst = -1;
    mktime(&t);

    char buf[64], tz[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
    std::strftime(tz, sizeof(tz), "%z", &t);
    std::string result(buf);
    std::string tzs(tz);
    if (tzs.size() == 5)
        result += tzs.substr(0, 3) + ":" + tzs.substr(3);
    else
        result += tzs;
    return result;
}

std::string SlacCalendarReader::extractHtmlInnerText(const std::string& html)
{
    if (html.empty())
        return "";

    std::string result;
    bool        in_tag = false;
    for (const char c : html)
    {
        if (c == '<')       { in_tag = true;  continue; }
        if (c == '>')       { in_tag = false; continue; }
        if (in_tag)         continue;
        if (c == '&')       continue; // skip entity openers (rare in URLs)
        result += c;
    }

    const auto start = result.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    const auto end = result.find_last_not_of(" \t\r\n");
    return result.substr(start, end - start + 1);
}

} // namespace mldp_pvxs_driver::reader::impl::slac_calendar
