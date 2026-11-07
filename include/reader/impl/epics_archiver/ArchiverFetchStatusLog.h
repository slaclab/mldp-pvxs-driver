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

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

/**
 * @brief Outcome of one PV fetch attempt as recorded in the status log.
 */
enum class FetchAttemptResult
{
    Ok,              ///< Stream fetched completely.
    Retrying,        ///< Attempt failed; another attempt will follow.
    Partial,         ///< Retries exhausted; some complete batches were published.
    Failed,          ///< Retries exhausted; no sample received.
    SkippedNotFound, ///< Archiver answered 404; not retried.
};

/**
 * @brief Return the status-log string for a fetch attempt result.
 */
const char* toString(FetchAttemptResult result);

/**
 * @brief One status-log record describing a single PV fetch attempt.
 */
struct FetchAttemptRecord
{
    std::string                reader;          ///< Reader instance name.
    std::string                pv;              ///< PV name.
    long                       attempt = 1;     ///< 1-based attempt number.
    long                       max_attempts = 1; ///< Total attempts allowed.
    std::string                from;            ///< Request window start (ISO 8601).
    std::optional<std::string> to;              ///< Request window end (ISO 8601).
    std::string                resume_source;   ///< "config" or "failed_window" (periodic tail re-fetch).
    FetchAttemptResult         result = FetchAttemptResult::Ok;
    std::size_t                samples = 0;     ///< Samples received by this attempt.
    std::size_t                bytes = 0;       ///< Bytes received by this attempt.
    std::optional<std::string> first_sample_ts; ///< First received sample time (ISO 8601).
    std::optional<std::string> last_sample_ts;  ///< Last received sample time (ISO 8601).
    std::optional<long>        http_status;     ///< HTTP status, when known.
    std::optional<std::string> error;           ///< Error message for failed attempts.
    double                     duration_ms = 0; ///< Attempt wall time.
};

/**
 * @brief Thread-safe append-only JSON-lines log of PV fetch attempts.
 *
 * Each call writes one JSON object per line and flushes, so the file stays
 * readable after a crash. The log tracks the final result of every PV and
 * can emit an end-of-run summary line.
 */
class ArchiverFetchStatusLog
{
public:
    /**
     * @brief Open (append) the status log file.
     *
     * @param path File to append to; parent directory must exist.
     * @throws std::runtime_error when the file cannot be opened.
     */
    explicit ArchiverFetchStatusLog(const std::string& path);

    /**
     * @brief Append one attempt record.
     */
    void record(const FetchAttemptRecord& rec);

    /**
     * @brief Append a summary line with final results seen so far.
     *
     * Counts are per fetch window. `*_pvs` list unique PV names; `*_windows`
     * list each `{pv, from, to}` window to re-fetch.
     *
     * @param reader Reader instance name.
     */
    void writeSummary(const std::string& reader);

private:
    std::mutex               mutex_;
    std::ofstream            out_;
    std::size_t              ok_ = 0;
    struct WindowRef
    {
        std::string                pv;
        std::string                from;
        std::optional<std::string> to;
    };
    std::vector<WindowRef> failed_;
    std::vector<WindowRef> partial_;
    std::vector<WindowRef> not_found_;
};

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver
