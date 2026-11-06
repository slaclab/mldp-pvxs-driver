//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics_archiver/ArchiverFetchStatusLog.h>
#include <util/time/DateTimeUtils.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

const char* toString(FetchAttemptResult result)
{
    switch (result)
    {
    case FetchAttemptResult::Ok: return "ok";
    case FetchAttemptResult::Retrying: return "retrying";
    case FetchAttemptResult::Partial: return "partial";
    case FetchAttemptResult::Failed: return "failed";
    case FetchAttemptResult::SkippedNotFound: return "skipped_not_found";
    }
    return "unknown";
}

namespace {
std::string nowIso()
{
    return util::time::DateTimeUtils::formatIso8601UtcMillis(std::chrono::system_clock::now());
}
} // namespace

ArchiverFetchStatusLog::ArchiverFetchStatusLog(const std::string& path)
    : out_(path, std::ios::out | std::ios::app)
{
    if (!out_.is_open())
    {
        throw std::runtime_error("cannot open archiver status log '" + path + "'");
    }
}

void ArchiverFetchStatusLog::record(const FetchAttemptRecord& rec)
{
    nlohmann::json j;
    j["ts"] = nowIso();
    j["type"] = "attempt";
    j["reader"] = rec.reader;
    j["pv"] = rec.pv;
    j["attempt"] = rec.attempt;
    j["max_attempts"] = rec.max_attempts;
    j["from"] = rec.from;
    j["to"] = rec.to ? nlohmann::json(*rec.to) : nlohmann::json(nullptr);
    j["resume_source"] = rec.resume_source;
    j["result"] = toString(rec.result);
    j["samples"] = rec.samples;
    j["bytes"] = rec.bytes;
    j["first_sample_ts"] = rec.first_sample_ts ? nlohmann::json(*rec.first_sample_ts) : nlohmann::json(nullptr);
    j["last_sample_ts"] = rec.last_sample_ts ? nlohmann::json(*rec.last_sample_ts) : nlohmann::json(nullptr);
    j["http_status"] = rec.http_status ? nlohmann::json(*rec.http_status) : nlohmann::json(nullptr);
    j["error"] = rec.error ? nlohmann::json(*rec.error) : nlohmann::json(nullptr);
    j["duration_ms"] = rec.duration_ms;

    std::lock_guard<std::mutex> lock(mutex_);
    switch (rec.result)
    {
    case FetchAttemptResult::Ok: ++ok_; break;
    case FetchAttemptResult::Partial:
        ++partial_;
        partial_pvs_.push_back(rec.pv);
        break;
    case FetchAttemptResult::Failed:
        ++failed_;
        failed_pvs_.push_back(rec.pv);
        break;
    case FetchAttemptResult::SkippedNotFound:
        ++not_found_;
        not_found_pvs_.push_back(rec.pv);
        break;
    case FetchAttemptResult::Retrying: break;
    }
    // Invalid UTF-8 in archiver error text must not throw here.
    out_ << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    out_.flush();
}

void ArchiverFetchStatusLog::writeSummary(const std::string& reader)
{
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json j;
    j["ts"] = nowIso();
    j["type"] = "summary";
    j["reader"] = reader;
    j["ok"] = ok_;
    j["partial"] = partial_;
    j["failed"] = failed_;
    j["not_found"] = not_found_;
    j["partial_pvs"] = partial_pvs_;
    j["failed_pvs"] = failed_pvs_;
    j["not_found_pvs"] = not_found_pvs_;
    out_ << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    out_.flush();
}

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver
