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
 * End-to-end tests: MLDPPVXSController (epics-ds-metadata reader +
 *     mldp-pv-metadata writer) → TestAnnotationSvc (fake gRPC)
 *
 * Each test spins up:
 *   - MockDSServer     : fake EPICS Directory Service (PVXS RPC)
 *   - TestAnnotationSvc: in-process fake DpAnnotationService gRPC server
 *   - MLDPPVXSController wiring the epics-ds-metadata reader to the
 *     mldp-pv-metadata writer via the full controller pipeline
 *
 * TEST 3 additionally constructs an MLDPAnnotationQueryClient pointing at the
 * same in-process server and calls getPvMetadata() to verify the saved record
 * is retrievable through the annotation IQueryable interface.
 */

#include <gtest/gtest.h>

#include <annotation.grpc.pb.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <controller/MLDPPVXSController.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <pool/MLDPGrpcAnnotationPoolConfig.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include "../config/test_config_helpers.h"
#include "../mock/MockDSServer.h"

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::controller::MLDPPVXSController;
using mldp_pvxs_driver::query::impl::mldp::MLDPAnnotationQueryClient;
using mldp_pvxs_driver::test::mock::MockDSServer;

namespace {

// ---------------------------------------------------------------------------
// Fake annotation gRPC server: stores savePvMetadata calls; serves them back
// via getPvMetadata so MLDPAnnotationQueryClient can verify the saved records.
// ---------------------------------------------------------------------------

class TestAnnotationSvc final
    : public dp::service::annotation::DpAnnotationService::Service
{
public:
    std::atomic<int> save_count{0};

    grpc::Status savePvMetadata(
        grpc::ServerContext*,
        const dp::service::annotation::SavePvMetadataRequest* req,
        dp::service::annotation::SavePvMetadataResponse*) override
    {
        dp::service::common::PvMetadata pvm;
        for (const auto& a : req->attributes())
            *pvm.add_attributes() = a;
        {
            std::lock_guard<std::mutex> lock(mu_);
            saved_[req->pvname()] = std::move(pvm);
        }
        save_count.fetch_add(1, std::memory_order_relaxed);
        return grpc::Status::OK;
    }

    grpc::Status getPvMetadata(
        grpc::ServerContext*,
        const dp::service::annotation::GetPvMetadataRequest* req,
        dp::service::annotation::GetPvMetadataResponse*      resp) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = saved_.find(req->pvnameoralias());
        if (it == saved_.end())
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "not found");
        *resp->mutable_getpvmetadataresult()->mutable_pvmetadata() = it->second;
        return grpc::Status::OK;
    }

    std::unordered_map<std::string, dp::service::common::PvMetadata> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return saved_;
    }

private:
    mutable std::mutex                                                 mu_;
    std::unordered_map<std::string, dp::service::common::PvMetadata>  saved_;
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
                                const std::string& ds_service,
                                const std::string& reader_name)
{
    std::ostringstream ss;
    ss << "writer:\n"
       << "  mldp-pv-metadata:\n"
       << "    - name: ds-metadata-writer-test\n"
       << "      thread-pool: 2\n"
       << "      deadline-seconds: 5\n"
       << "      mldp-pv-metadata-pool:\n"
       << "        annotation-url: " << annotation_url << "\n"
       << "        min-conn: 1\n"
       << "        max-conn: 2\n"
       << "reader:\n"
       << "  - epics-ds-metadata:\n"
       << "      - name: " << reader_name << "\n"
       << "        service: " << ds_service << "\n"
       << "        timeout-sec: 5.0\n"
       << "        source-name-column: channelName\n"
       << "        tags-column: tags\n"
       << "        rescan-interval-sec: 0.0\n"
       << "        pvs:\n"
       << "          - name: BPMS:IN20:221:TMIT\n";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Fixture: starts fake gRPC server on ephemeral port; tears down after test.
// ---------------------------------------------------------------------------

class DsMetadataWriterTest : public ::testing::Test
{
protected:
    TestAnnotationSvc                   svc_;
    int                                 port_ = 0;
    std::unique_ptr<grpc::Server>       grpc_server_;
    std::shared_ptr<MLDPPVXSController> controller_;

    void SetUp() override
    {
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(),
                                 &port_);
        builder.RegisterService(&svc_);
        grpc_server_ = builder.BuildAndStart();
        ASSERT_TRUE(grpc_server_);
        ASSERT_GT(port_, 0);
    }

    void TearDown() override
    {
        if (controller_)
            controller_->stop();
        if (grpc_server_)
            grpc_server_->Shutdown();
    }

    std::string annotationUrl() const
    {
        return "127.0.0.1:" + std::to_string(port_);
    }

    void startController(const std::string& ds_service, const std::string& reader_name)
    {
        controller_ = MLDPPVXSController::create(
            makeConfigFromYaml(makeControllerYaml(annotationUrl(), ds_service, reader_name)));
        controller_->start();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TEST 1 — reader feeds writer: savePvMetadata called for all 30 DS rows
// ---------------------------------------------------------------------------

TEST_F(DsMetadataWriterTest, ReaderPushesAllRowsToAnnotationService)
{
    MockDSServer mock_ds("test:ds-wr1");

    ASSERT_NO_THROW(startController("test:ds-wr1", "wr1-reader"));

    ASSERT_TRUE(waitForCount(svc_.save_count, 30, std::chrono::milliseconds(8000)))
        << "Expected >= 30 savePvMetadata calls, got " << svc_.save_count.load();
}

// ---------------------------------------------------------------------------
// TEST 2 — PV metadata attributes from the DS row reach the annotation server
// ---------------------------------------------------------------------------

TEST_F(DsMetadataWriterTest, PvMetadataAttributesForwardedCorrectly)
{
    MockDSServer mock_ds("test:ds-wr2");

    ASSERT_NO_THROW(startController("test:ds-wr2", "wr2-reader"));

    ASSERT_TRUE(waitForCount(svc_.save_count, 30, std::chrono::milliseconds(8000)));

    const auto saved = svc_.snapshot();
    ASSERT_EQ(saved.count("BPMS:IN20:221:X"), 1u);

    bool found_owner = false;
    for (const auto& attr : saved.at("BPMS:IN20:221:X").attributes())
    {
        if (attr.name() == "owner" && attr.value() == "diagnostics")
        {
            found_owner = true;
            break;
        }
    }
    EXPECT_TRUE(found_owner)
        << "Expected attribute owner=diagnostics in saved PvMetadata for BPMS:IN20:221:X";
}

// ---------------------------------------------------------------------------
// TEST 3 — MLDPAnnotationQueryClient (annotation IQueryable) retrieves a
//           record that was saved via the reader→writer pipeline
// ---------------------------------------------------------------------------

TEST_F(DsMetadataWriterTest, QueryClientRetrievesSavedPvMetadata)
{
    MockDSServer mock_ds("test:ds-wr3");

    ASSERT_NO_THROW(startController("test:ds-wr3", "wr3-reader"));

    ASSERT_TRUE(waitForCount(svc_.save_count, 30, std::chrono::milliseconds(8000)));

    mldp_pvxs_driver::util::pool::MLDPGrpcAnnotationPoolConfig acfg(
        makeConfigFromYaml(
            "annotation-url: " + annotationUrl() + "\n"
            "min-conn: 1\n"
            "max-conn: 1\n"));
    MLDPAnnotationQueryClient query_client{acfg};

    const auto result = query_client.getPvMetadata("VPIO:IN20:111:PRES");
    ASSERT_TRUE(result.has_value())
        << "getPvMetadata returned nullopt for VPIO:IN20:111:PRES";

    bool found_hostname = false;
    for (const auto& attr : result->attributes())
    {
        if (attr.name() == "hostName" && attr.value() == "cpu-li20-vac1")
        {
            found_hostname = true;
            break;
        }
    }
    EXPECT_TRUE(found_hostname)
        << "Expected attribute hostName=cpu-li20-vac1 in queried PvMetadata";
}

// ---------------------------------------------------------------------------
// TEST 4 — controller stops cleanly without hanging (shutdown test)
// ---------------------------------------------------------------------------

TEST_F(DsMetadataWriterTest, ControllerStopsCleanlyWithinDeadline)
{
    MockDSServer mock_ds("test:ds-wr4");

    ASSERT_NO_THROW(startController("test:ds-wr4", "wr4-reader"));

    // Wait for at least one RPC cycle to confirm the reader is running
    ASSERT_TRUE(waitForCount(svc_.save_count, 1, std::chrono::milliseconds(8000)))
        << "Reader never produced any saves — cannot verify clean shutdown";

    // stop() must return within 3 seconds; if the worker is stuck in wait()
    // the interrupt() fix ensures it unblocks immediately
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(controller_->stop());
    controller_.reset();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_LT(elapsed, std::chrono::seconds(3))
        << "controller->stop() took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << "ms — worker likely blocked in RPC wait()";
}
