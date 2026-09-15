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
 * Integration test:
 *   MockCalendarHttpServer -> MLDPPVXSController (slac-calendar reader +
 *       mldp-configuration writer) -> TestCalendarAnnotationSvc (fake gRPC)
 *
 * Verifies that calendar events are forwarded end-to-end through the full
 * controller pipeline as saveConfiguration + saveConfigurationActivation
 * gRPC calls.
 */

#include <gtest/gtest.h>

#include <annotation.grpc.pb.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <controller/MLDPPVXSController.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../config/test_config_helpers.h"
#include "../mock/MockCalendarHttpServer.h"


using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::controller::MLDPPVXSController;
using mldp_pvxs_driver::test::mock::MockCalendarHttpServer;

namespace {

// ---------------------------------------------------------------------------
// Fake gRPC annotation service
// ---------------------------------------------------------------------------

class TestCalendarAnnotationSvc final
    : public dp::service::annotation::DpAnnotationService::Service
{
public:
    std::atomic<int> save_configuration_count{0};
    std::atomic<int> save_activation_count{0};

    grpc::Status saveConfiguration(
        grpc::ServerContext*,
        const dp::service::annotation::SaveConfigurationRequest* req,
        dp::service::annotation::SaveConfigurationResponse*) override
    {
        save_configuration_count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mu_);
        cfg_requests_.push_back(*req);
        return grpc::Status::OK;
    }

    grpc::Status saveConfigurationActivation(
        grpc::ServerContext*,
        const dp::service::annotation::SaveConfigurationActivationRequest* req,
        dp::service::annotation::SaveConfigurationActivationResponse*) override
    {
        save_activation_count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mu_);
        act_requests_.push_back(*req);
        return grpc::Status::OK;
    }

    std::vector<dp::service::annotation::SaveConfigurationRequest> cfgRequests() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return cfg_requests_;
    }

    std::vector<dp::service::annotation::SaveConfigurationActivationRequest> actRequests() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return act_requests_;
    }

private:
    mutable std::mutex mu_;
    std::vector<dp::service::annotation::SaveConfigurationRequest>           cfg_requests_;
    std::vector<dp::service::annotation::SaveConfigurationActivationRequest> act_requests_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool waitForCount(std::atomic<int>& counter, int target, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (counter.load(std::memory_order_relaxed) >= target)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return counter.load(std::memory_order_relaxed) >= target;
}

std::string makeControllerYaml(const std::string& annotation_url,
                                const std::string& base_url,
                                const std::string& experiments_yaml)
{
    std::ostringstream ss;
    ss << "writer:\n"
       << "  mldp-configuration:\n"
       << "    - name: cal-writer-test\n"
       << "      thread-pool: 2\n"
       << "      deadline-seconds: 5\n"
       << "      mldp-annotation-pool:\n"
       << "        annotation-url: " << annotation_url << "\n"
       << "        min-conn: 1\n"
       << "        max-conn: 2\n"
       << "reader:\n"
       << "  - slac-calendar:\n"
       << "      - name: cal-reader-test\n"
       << "        base-url: " << base_url << "\n"
       << "        experiments:\n"
       << experiments_yaml
       << "        lookahead-days: 30\n"
       << "        lookback-days: 1\n"
       << "        rescan-interval-sec: 0.0\n"
       << "        connect-timeout-sec: 5\n"
       << "        total-timeout-sec: 15\n"
       << "        tls-verify-peer: false\n"
       << "        tls-verify-host: false\n";
    return ss.str();
}

// LCLS sample events (3 events)
static const char* kThreeLclsEvents = R"json([
  {
    "url": "https://www.google.com/calendar/event?eid=ev1",
    "program_name": "CXI Run 1",
    "description": "First run",
    "note": null,
    "calendar": "NC-CXI",
    "details": null,
    "start": "2026-05-28T06:00:00-07:00",
    "end":   "2026-05-28T18:00:00-07:00",
    "hutch": {"name": "CXI", "color": "#a00000", "line": "HXR", "text_color": "white"},
    "machine": "NC"
  },
  {
    "url": "https://www.google.com/calendar/event?eid=ev2",
    "program_name": "MEC Run 2",
    "description": "",
    "note": null,
    "calendar": "NC-MEC",
    "details": null,
    "start": "2026-05-29T06:00:00-07:00",
    "end":   "2026-05-29T18:00:00-07:00",
    "hutch": {"name": "MEC", "color": "#00a000", "line": "HXR", "text_color": "black"},
    "machine": "NC"
  },
  {
    "url": "https://www.google.com/calendar/event?eid=ev3",
    "program_name": "TMO Run 3",
    "description": "Third run",
    "note": "note text",
    "calendar": "NC-TMO",
    "details": "<a href=\"https://example.com\">https://example.com</a>",
    "start": "2026-05-30T06:00:00-07:00",
    "end":   "2026-05-30T18:00:00-07:00",
    "tags": ["tag_a", "tag_b"],
    "poc": "Doe",
    "config": "800 eV",
    "hutch": {"name": "TMO", "color": "#0000a0", "line": "SXR", "text_color": "white"},
    "machine": "NC"
  }
])json";

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class SlacCalendarIntegrationTest : public ::testing::Test
{
protected:
    TestCalendarAnnotationSvc           svc_;
    int                                 grpc_port_ = 0;
    std::unique_ptr<grpc::Server>       grpc_server_;
    MockCalendarHttpServer              calendar_server_;
    std::shared_ptr<MLDPPVXSController> controller_;

    void SetUp() override
    {
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(),
                                 &grpc_port_);
        builder.RegisterService(&svc_);
        grpc_server_ = builder.BuildAndStart();
        ASSERT_TRUE(grpc_server_);
        ASSERT_GT(grpc_port_, 0);

        calendar_server_.start();
    }

    void TearDown() override
    {
        if (controller_)
            controller_->stop();
        calendar_server_.stop();
        if (grpc_server_)
            grpc_server_->Shutdown();
    }

    std::string annotationUrl() const
    {
        return "127.0.0.1:" + std::to_string(grpc_port_);
    }

    void startController(const std::string& experiments_yaml)
    {
        controller_ = MLDPPVXSController::create(
            makeConfigFromYaml(makeControllerYaml(
                annotationUrl(), calendar_server_.baseUrl(), experiments_yaml)));
        controller_->start();
    }
};

// ---------------------------------------------------------------------------
// TEST 1 — 3 events produce 3 saveConfiguration + 3 saveConfigurationActivation calls
// ---------------------------------------------------------------------------

TEST_F(SlacCalendarIntegrationTest, ThreeEventsProduceSixGrpcCalls)
{
    calendar_server_.setResponse("lcls", kThreeLclsEvents);

    ASSERT_NO_THROW(startController("          - lcls\n"));

    ASSERT_TRUE(waitForCount(svc_.save_configuration_count, 3, std::chrono::milliseconds(8000)))
        << "Expected 3 saveConfiguration calls, got " << svc_.save_configuration_count.load();
    ASSERT_TRUE(waitForCount(svc_.save_activation_count, 3, std::chrono::milliseconds(8000)))
        << "Expected 3 saveConfigurationActivation calls, got " << svc_.save_activation_count.load();
}

// ---------------------------------------------------------------------------
// TEST 2 — Attributes forwarded correctly (experiment, hutch, tags)
// ---------------------------------------------------------------------------

TEST_F(SlacCalendarIntegrationTest, AttributesForwardedCorrectly)
{
    calendar_server_.setResponse("lcls", kThreeLclsEvents);

    ASSERT_NO_THROW(startController("          - lcls\n"));

    ASSERT_TRUE(waitForCount(svc_.save_configuration_count, 3, std::chrono::milliseconds(8000)));

    const auto reqs = svc_.cfgRequests();
    ASSERT_EQ(reqs.size(), 3u);

    // Find the TMO Run 3 request (has tags, details, poc, config)
    const dp::service::annotation::SaveConfigurationRequest* tmo_req = nullptr;
    for (const auto& r : reqs)
    {
        if (r.configurationname() == "TMO Run 3")
        {
            tmo_req = &r;
            break;
        }
    }
    ASSERT_NE(tmo_req, nullptr) << "TMO Run 3 config request not found";

    EXPECT_EQ(tmo_req->category(), "NC-TMO");
    EXPECT_EQ(tmo_req->description(), "Third run");
    ASSERT_EQ(tmo_req->tags_size(), 2);

    bool found_experiment = false, found_poc = false, found_details = false;
    for (const auto& attr : tmo_req->attributes())
    {
        if (attr.name() == "experiment" && attr.value() == "lcls")
            found_experiment = true;
        if (attr.name() == "poc" && attr.value() == "Doe")
            found_poc = true;
        if (attr.name() == "details" && attr.value() == "https://example.com")
            found_details = true;
    }
    EXPECT_TRUE(found_experiment) << "attribute experiment=lcls not found";
    EXPECT_TRUE(found_poc)        << "attribute poc=Doe not found";
    EXPECT_TRUE(found_details)    << "attribute details=https://example.com not found";
}

// ---------------------------------------------------------------------------
// TEST 3 — Activation idempotency keys match event URLs
// ---------------------------------------------------------------------------

TEST_F(SlacCalendarIntegrationTest, ActivationClientIdMatchesEventUrl)
{
    calendar_server_.setResponse("lcls", kThreeLclsEvents);

    ASSERT_NO_THROW(startController("          - lcls\n"));

    ASSERT_TRUE(waitForCount(svc_.save_activation_count, 3, std::chrono::milliseconds(8000)));

    const auto acts = svc_.actRequests();
    ASSERT_EQ(acts.size(), 3u);

    bool found_ev1 = false;
    for (const auto& a : acts)
    {
        if (a.clientactivationid() == "https://www.google.com/calendar/event?eid=ev1")
        {
            found_ev1 = true;
            break;
        }
    }
    EXPECT_TRUE(found_ev1) << "Expected activation with clientActivationId=ev1 URL";
}
