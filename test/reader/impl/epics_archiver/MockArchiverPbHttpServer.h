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

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

inline constexpr const char* kMockArchiverPbHttpPath = "/retrieval/data/getData.raw";
inline constexpr const char* kMockArchiverPbHttpPv = "TEST:PV:DOUBLE";

/**
 * @brief Reusable local HTTP mock for Archiver Appliance PB/HTTP tests.
 *
 * Serves randomized `SCALAR_DOUBLE` PB/HTTP data for
 * `/retrieval/data/getData.raw`, validates required query parameters (`pv`,
 * `from`), accepts optional `to`, and records the last request. Samples are
 * generated across the requested time range with configurable production
 * behavior.
 */
class MockArchiverPbHttpServer
{
public:
    /**
     * @brief Controls how the mock generates archiver samples.
     */
    struct GenerationConfig
    {
        uint32_t    min_events_per_second = 4;   ///< Lower bound for samples emitted in each second bucket.
        uint32_t    max_events_per_second = 8;   ///< Upper bound for samples emitted in each second bucket.
        double      min_value = -50.0;           ///< Minimum generated scalar value.
        double      max_value = 50.0;            ///< Maximum generated scalar value.
        uint32_t    open_ended_duration_sec = 1; ///< Duration used when `to` is omitted.
        uint64_t    random_seed = 0x5A17C0DEULL; ///< Base seed (combined with request fields).
        std::string egu = "kG";                  ///< Unit metadata emitted in PayloadInfo headers.
        uint32_t    precision = 3;               ///< PREC metadata emitted in PayloadInfo headers.
    };

    struct RequestLog
    {
        std::string                path;
        std::optional<std::string> pv;
        std::optional<std::string> from;
        std::optional<std::string> to;
    };

    MockArchiverPbHttpServer();
    explicit MockArchiverPbHttpServer(const GenerationConfig& config);
    ~MockArchiverPbHttpServer();

    /**
     * @brief Start the local HTTP server on 127.0.0.1 using an ephemeral port.
     *
     * Spawns the cpp-httplib listen loop on a background thread.
     */
    void start();
    /**
     * @brief Stop the server and join the background thread.
     *
     * Safe to call multiple times.
     */
    void stop();

    /**
     * @brief Return the bound local TCP port after @ref start().
     */
    [[nodiscard]] int                     port() const;
    /**
     * @brief Return the base URL for tests (e.g. http://127.0.0.1:PORT).
     */
    [[nodiscard]] std::string             baseUrl() const;
    /**
     * @brief Return the most recently recorded request snapshot.
     *
     * This is non-blocking and may return an empty/default log if no request
     * has been handled yet.
     */
    [[nodiscard]] RequestLog              lastRequest() const;
    /**
     * @brief Wait until the most recently recorded request has finished sending its response.
     *
     * Returns true when the response is fully sent (or otherwise completed),
     * false on timeout.
     */
    bool                                  waitForLastResponseComplete(std::chrono::milliseconds timeout) const;
    /**
     * @brief Return the sample generation settings used by this mock instance.
     */
    [[nodiscard]] const GenerationConfig& generationConfig() const;

private:
    /**
     * @brief Advance the request sequence while holding @ref mu_.
     *
     * Called when a request is accepted and recorded in @ref last_request_.
     */
    void markLastRequestStartedLocked();
    /**
     * @brief Mark a request id as completed and notify waiters.
     *
     * Completion means the response finished sending (or errored before body streaming).
     */
    void markRequestCompleted(uint64_t request_id);

    GenerationConfig   config_;
    httplib::Server    server_;
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    RequestLog         last_request_;
    uint64_t           last_request_id_ = 0;
    uint64_t           last_completed_request_id_ = 0;
    std::thread        thread_;
    std::atomic<bool>  running_{false};
    int                port_ = -1;
};

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver
