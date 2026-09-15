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
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::test::mock {

struct CalendarRequestLog
{
    std::string experiment;
    std::string start_time;
    std::string end_time;
    std::string limit;
    std::string path;
};

class MockCalendarHttpServer
{
public:
    MockCalendarHttpServer();
    ~MockCalendarHttpServer();

    void start();
    void stop();

    [[nodiscard]] int         port() const;
    [[nodiscard]] std::string baseUrl() const;

    void setResponse(const std::string& experiment, const std::string& json_body);
    void setStatusCode(const std::string& experiment, int code);

    [[nodiscard]] std::vector<CalendarRequestLog> requestHistory() const;
    bool waitForRequestCount(size_t min_requests, std::chrono::milliseconds timeout) const;

private:
    httplib::Server                      server_;
    mutable std::mutex                   mu_;
    mutable std::condition_variable      cv_;
    std::vector<CalendarRequestLog>      history_;
    std::unordered_map<std::string, std::string> responses_;
    std::unordered_map<std::string, int>         status_codes_;
    std::thread                          thread_;
    std::atomic<bool>                    running_{false};
    int                                  port_{-1};
};

} // namespace mldp_pvxs_driver::test::mock
