//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "MockCalendarHttpServer.h"

#include <stdexcept>

namespace mldp_pvxs_driver::test::mock {

MockCalendarHttpServer::MockCalendarHttpServer()
{
    server_.Get(R"(/([^/]+)/events\.json)",
                [this](const httplib::Request& req, httplib::Response& res) {
                    std::string exp;
                    if (!req.matches.empty())
                        exp = req.matches[1].str();

                    CalendarRequestLog log;
                    log.experiment  = exp;
                    log.path        = req.path;
                    log.start_time  = req.get_param_value("start_time");
                    log.end_time    = req.get_param_value("end_time");
                    log.limit       = req.get_param_value("limit");

                    int status_code = 200;
                    std::string body = "[]";

                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        history_.push_back(log);
                        const auto sc_it = status_codes_.find(exp);
                        if (sc_it != status_codes_.end())
                            status_code = sc_it->second;
                        const auto body_it = responses_.find(exp);
                        if (body_it != responses_.end())
                            body = body_it->second;
                    }
                    cv_.notify_all();

                    res.status = status_code;
                    res.set_content(body, "application/json");
                });
}

MockCalendarHttpServer::~MockCalendarHttpServer()
{
    stop();
}

void MockCalendarHttpServer::start()
{
    if (running_.exchange(true))
        return;

    port_ = server_.bind_to_any_port("127.0.0.1");
    if (port_ <= 0)
    {
        running_ = false;
        throw std::runtime_error("MockCalendarHttpServer: failed to bind");
    }

    thread_ = std::thread([this] {
        server_.listen_after_bind();
        running_ = false;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
}

void MockCalendarHttpServer::stop()
{
    if (!running_.exchange(false))
        return;
    server_.stop();
    if (thread_.joinable())
        thread_.join();
}

int MockCalendarHttpServer::port() const
{
    return port_;
}

std::string MockCalendarHttpServer::baseUrl() const
{
    return "http://127.0.0.1:" + std::to_string(port_);
}

void MockCalendarHttpServer::setResponse(const std::string& experiment,
                                         const std::string& json_body)
{
    std::lock_guard<std::mutex> lk(mu_);
    responses_[experiment] = json_body;
}

void MockCalendarHttpServer::setStatusCode(const std::string& experiment, int code)
{
    std::lock_guard<std::mutex> lk(mu_);
    status_codes_[experiment] = code;
}

std::vector<CalendarRequestLog> MockCalendarHttpServer::requestHistory() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return history_;
}

bool MockCalendarHttpServer::waitForRequestCount(size_t             min_requests,
                                                  std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, timeout, [&] { return history_.size() >= min_requests; });
}

} // namespace mldp_pvxs_driver::test::mock
