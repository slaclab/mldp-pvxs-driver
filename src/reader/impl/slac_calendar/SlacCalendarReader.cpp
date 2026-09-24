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

#include <algorithm>
#include <cctype>
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
    stats_thread_  = std::thread([this] { runStats(); });
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
    if (stats_thread_.joinable())
        stats_thread_.join();
}

void SlacCalendarReader::runStats()
{
    while (running_.load())
    {
        std::unique_lock<std::mutex> lk(worker_mutex_);
        worker_cv_.wait_for(lk,
                            std::chrono::seconds(10),
                            [this] { return !running_.load(); });
        if (!running_.load()) break;
        infof(*logger_,
              "SlacCalendarReader '{}' stats: configurations_pushed={} activations_pushed={}",
              config_.name(),
              cfg_pushed_.load(),
              act_pushed_.load());
    }
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
                start_iso = dateToIso(*config_.startDate());
                end_iso   = config_.endDate().has_value()
                          ? dateToIso(*config_.endDate())
                          : nowIso();
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

        if (config_.endDate().has_value() || config_.rescanIntervalSec() <= 0.0)
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
    for (const auto& accel : config_.accels())
    {
        try
        {
            SeenActivationMap             seen;
            std::vector<PendingActivation> pending;

            const int64_t range_start = static_cast<int64_t>(parseBusTimestamp(startIso).epoch_seconds);
            const int64_t range_end   = static_cast<int64_t>(parseBusTimestamp(endIso).epoch_seconds);
            const int64_t window_sec  = static_cast<int64_t>(config_.fetchWindowDays()) * 86400;

            for (int64_t win_start = range_start; win_start < range_end; win_start += window_sec)
            {
                const int64_t win_end = std::min(win_start + window_sec, range_end);
                const std::string win_start_iso = epochToIso(win_start);
                const std::string win_end_iso   = epochToIso(win_end);

                const auto body = fetchAccel(accel, win_start_iso, win_end_iso);
                parseAndPush(body, accel, seen, pending);
            }

            mergeAndPushActivations(pending);
        }
        catch (const std::exception& e)
        {
            errorf(*logger_,
                   "SlacCalendarReader '{}' accel '{}' error: {}",
                   config_.name(),
                   accel,
                   e.what());
        }
    }
}

std::string SlacCalendarReader::fetchAccel(const std::string& accel,
                                           const std::string& startIso,
                                           const std::string& endIso)
{
    const std::string url = buildUrl(accel, startIso, endIso);
    HttpRequest       req{url, {}};
    const auto        result = http_client_.get(req);

    if (result.info.http_status != 200)
    {
        throw std::runtime_error("HTTP " + std::to_string(result.info.http_status) +
                                 " from " + url);
    }
    return std::string(result.body.begin(), result.body.end());
}

void SlacCalendarReader::parseAndPush(const std::string& jsonBody, const std::string& accel,
                                       SeenActivationMap& seen, std::vector<PendingActivation>& pending)
{
    const auto events = nlohmann::json::parse(jsonBody);
    if (!events.is_array())
        throw std::runtime_error("expected JSON array from SLAC calendar API");

    for (const auto& ev : events)
        pushEvent(ev, accel, seen, pending);
}

static std::string jsonStr(const nlohmann::json& ev, const std::string& key, const std::string& def = "")
{
    if (!ev.contains(key) || ev[key].is_null() || !ev[key].is_string())
        return def;
    return ev[key].get<std::string>();
}

// Collapses runs of whitespace to a single space and trims the ends. The calendar API
// sometimes returns the same event's program_name with inconsistent spacing between two
// fetches/edits (e.g. "NC Linac BCSChecks" vs "NC Linac BCS Checks"), which otherwise
// produces two distinct configurationName strings for what MLDP's overlap check treats
// as the same key server-side.
static std::string normalizeWhitespace(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool prev_space = true; // drop leading spaces
    for (const char c : s)
    {
        const bool is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (is_space)
        {
            if (!prev_space)
                out += ' ';
            prev_space = true;
        }
        else
        {
            out += c;
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

void SlacCalendarReader::pushEvent(const nlohmann::json& ev, const std::string& accel,
                                    SeenActivationMap& seen, std::vector<PendingActivation>& pending)
{
    const std::string program_name = normalizeWhitespace(jsonStr(ev, "program_name"));
    if (program_name.empty())
    {
        warnf(*logger_,
              "SlacCalendarReader '{}' accel '{}': skipping event with empty program_name"
              " (start='{}', url='{}')",
              config_.name(), accel, jsonStr(ev, "start"), jsonStr(ev, "url"));
        return;
    }

    const std::string calendar  = normalizeWhitespace(jsonStr(ev, "calendar"));
    const std::string url       = jsonStr(ev, "url");
    const std::string start_str = jsonStr(ev, "start");
    const std::string end_str   = jsonStr(ev, "end");
    const std::string desc      = jsonStr(ev, "description");

    // Same activity is sometimes cross-posted under multiple Google Calendars
    // (identical program_name/time window, different source calendar/url).
    // Disambiguate the 2nd+ occurrence so it doesn't collide with the first one on
    // the server's per-configurationName overlap check.
    std::string configuration_name = program_name;
    if (!start_str.empty() && !end_str.empty())
    {
        const std::string key = program_name + "|" + start_str + "|" + end_str;
        const auto        it  = seen.find(key);
        if (it == seen.end())
        {
            seen.emplace(key, calendar);
        }
        else if (it->second != calendar)
        {
            configuration_name = calendar.empty()
                                ? program_name + " [" + accel + "]"
                                : program_name + " [" + calendar + "]";
        }
    }

    // category is a required field on the server and the server independently
    // rejects overlapping activations that share a category (same rule as for
    // configurationName - see annotation.proto SaveConfigurationActivationRequest).
    // Using the source calendar (e.g. NC-MEC, NC-PAMM) as category groups every
    // concurrent-but-unrelated activity in that hutch/area together and makes
    // that rule trigger on events that don't actually represent the same
    // activity. Default category to configuration_name instead, so the
    // category-overlap rule collapses onto the name-overlap rule instead of
    // adding false collisions; the source calendar is still preserved verbatim
    // in the "calendar" attribute below. A configured category override still
    // takes precedence when present.
    const std::string category = config_.category().has_value() ? *config_.category() : configuration_name;

    // --- ConfigurationPayload ---
    ConfigurationPayload cfg_payload;
    cfg_payload.root_source_name    = config_.name();
    cfg_payload.configuration_name = configuration_name;
    cfg_payload.category = category;

    if (!calendar.empty())
        cfg_payload.attributes["calendar"] = calendar;

    if (!desc.empty())
        cfg_payload.description = desc;

    if (ev.contains("tags") && !ev["tags"].is_null() && ev["tags"].is_array())
    {
        std::vector<std::string> tags;
        for (const auto& t : ev["tags"])
            if (t.is_string())
                tags.push_back(t.get<std::string>());
        if (!tags.empty())
            cfg_payload.tags = tags;
    }

    cfg_payload.attributes["accel"] = accel;
    cfg_payload.modified_by = "slac-calendar-reader";

    for (const auto& [key, attr] :
         std::vector<std::pair<std::string, std::string>>{
             {"note", "note"}, {"poc", "poc"}, {"config", "config"}, {"machine", "machine"}})
    {
        const std::string val = jsonStr(ev, key);
        if (!val.empty())
            cfg_payload.attributes[attr] = val;
    }

    if (ev.contains("details") && !ev["details"].is_null() && ev["details"].is_string())
    {
        const std::string raw = ev["details"].get<std::string>();
        const std::string det = extractHtmlInnerText(raw);
        if (!det.empty())
            cfg_payload.attributes["details"] = det;
    }

    if (ev.contains("hutch") && !ev["hutch"].is_null() && ev["hutch"].is_object())
    {
        const auto& h = ev["hutch"];
        for (const auto& [hk, ha] : std::vector<std::pair<std::string,std::string>>{
                 {"name","hutch_name"},{"color","hutch_color"},{"line","hutch_line"}})
        {
            const std::string val = jsonStr(h, hk);
            if (!val.empty())
                cfg_payload.attributes[ha] = val;
        }
    }

    {
        IDataBus::EventBatch b;
        b.reader_name = config_.name();
        b.payload     = std::move(cfg_payload);
        bus_->push(std::move(b));
        ++cfg_pushed_;
    }

    // --- ConfigurationActivationPayload (skip if start or end missing) ---
    if (start_str.empty() || end_str.empty())
    {
        warnf(*logger_,
              "SlacCalendarReader '{}' accel '{}': skipping activation for '{}' (missing start/end)",
              config_.name(), accel, program_name);
        return;
    }

    ConfigurationActivationPayload act_payload;
    if (!url.empty())
        act_payload.client_activation_id = url;
    act_payload.configuration_name   = configuration_name;
    act_payload.start_time           = parseBusTimestamp(start_str);
    act_payload.end_time             = parseBusTimestamp(end_str);
    if (!desc.empty())
        act_payload.description = desc;
    if (ev.contains("tags") && !ev["tags"].is_null() && ev["tags"].is_array())
    {
        std::vector<std::string> tags;
        for (const auto& t : ev["tags"])
            if (t.is_string())
                tags.push_back(t.get<std::string>());
        if (!tags.empty())
            act_payload.tags = tags;
    }
    act_payload.attributes["accel"] = accel;
    if (!calendar.empty())
        act_payload.attributes["calendar"] = calendar;
    act_payload.modified_by = "slac-calendar-reader";

    pending.push_back(PendingActivation{category, std::move(act_payload)});
}

void SlacCalendarReader::mergeAndPushActivations(std::vector<PendingActivation>& pending)
{
    // Group by configurationName+category (the server's overlap-check key), then merge
    // chronologically touching/overlapping occurrences into a single spanning activation —
    // the server rejects a new activation whose range merely touches an existing one under
    // the same key, which happens naturally for multi-day events split by the calendar API
    // into consecutive daily/shift blocks.
    std::unordered_map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < pending.size(); ++i)
        groups[pending[i].category + "|" + pending[i].payload.configuration_name].push_back(i);

    for (auto& [key, idxs] : groups)
    {
        std::sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b) {
            return pending[a].payload.start_time.epoch_seconds < pending[b].payload.start_time.epoch_seconds;
        });

        size_t run_start = 0;
        while (run_start < idxs.size())
        {
            auto& merged = pending[idxs[run_start]].payload;
            size_t j = run_start + 1;
            while (j < idxs.size())
            {
                auto& next = pending[idxs[j]].payload;
                const uint64_t merged_end = merged.end_time.has_value()
                                           ? merged.end_time->epoch_seconds
                                           : merged.start_time.epoch_seconds;
                if (next.start_time.epoch_seconds > merged_end)
                    break;
                if (next.end_time.has_value() &&
                    (!merged.end_time.has_value() || next.end_time->epoch_seconds > merged.end_time->epoch_seconds))
                    merged.end_time = next.end_time;
                ++j;
            }

            IDataBus::EventBatch b;
            b.reader_name = config_.name();
            b.payload     = merged;
            bus_->push(std::move(b));
            ++act_pushed_;

            run_start = j;
        }
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

std::string SlacCalendarReader::buildUrl(const std::string& accel,
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

    return config_.baseUrl() + "/" + accel + "/events.json" +
           "?non_program_events=false" +
           "&start_time=" + encode(startIso) +
           "&end_time="   + encode(endIso);
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

std::string SlacCalendarReader::epochToIso(int64_t epoch)
{
    const time_t t = static_cast<time_t>(epoch);
    struct tm    u{};
    gmtime_r(&t, &u);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &u);
    return std::string(buf) + "+00:00";
}

std::string SlacCalendarReader::dateToIso(const std::string& s)
{
    int y, m, d, H = 0, M = 0, S = 0;
    bool has_tz    = false;
    int  tz_offset = 0; // seconds east of UTC

    if (s.size() > 10 && s[10] == 'T')
    {
        // find tz suffix: Z or ±HH:MM
        std::string dt = s;
        if (!dt.empty() && dt.back() == 'Z')
        {
            has_tz = true; tz_offset = 0;
            dt.pop_back();
        }
        else if (dt.size() >= 22)
        {
            const size_t sign_pos = dt.size() - 6;
            const char   sign_ch  = dt[sign_pos];
            if (sign_ch == '+' || sign_ch == '-')
            {
                int th, tm2;
                if (sscanf(dt.c_str() + sign_pos + 1, "%d:%d", &th, &tm2) == 2)
                {
                    tz_offset = (sign_ch == '+' ? 1 : -1) * (th * 3600 + tm2 * 60);
                    has_tz    = true;
                    dt        = dt.substr(0, sign_pos);
                }
            }
        }
        if (sscanf(dt.c_str(), "%d-%d-%dT%d:%d:%d", &y, &m, &d, &H, &M, &S) < 3)
            throw std::runtime_error("bad date: " + s);
    }
    else
    {
        if (sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
            throw std::runtime_error("bad date: " + s);
    }

    time_t epoch;
    if (has_tz)
    {
        // interpret as UTC: (wall time - tz_offset) = UTC epoch
        struct tm u{};
        u.tm_year = y - 1900; u.tm_mon = m - 1; u.tm_mday = d;
        u.tm_hour = H;        u.tm_min = M;      u.tm_sec  = S;
        epoch = timegm(&u) - tz_offset;
    }
    else
    {
        struct tm lt{};
        lt.tm_year = y - 1900; lt.tm_mon = m - 1; lt.tm_mday = d;
        lt.tm_hour = H;        lt.tm_min = M;      lt.tm_sec  = S;
        lt.tm_isdst = -1;
        epoch = mktime(&lt);
    }

    struct tm t{};
    localtime_r(&epoch, &t);

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
