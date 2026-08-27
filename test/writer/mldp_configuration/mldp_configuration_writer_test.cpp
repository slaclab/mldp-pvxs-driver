//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>

#include <annotation.grpc.pb.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <metrics/Metrics.h>
#include <metrics/MetricsConfig.h>
#include <util/bus/IDataBus.h>
#include <writer/WriterFactory.h>
#include <writer/mldp_configuration/MLDPConfigurationWriter.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../config/test_config_helpers.h"

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::util::bus::BatchPayload;
using mldp_pvxs_driver::util::bus::ConfigurationActivationPayload;
using mldp_pvxs_driver::util::bus::ConfigurationPayload;
using mldp_pvxs_driver::util::bus::IDataBus;
using mldp_pvxs_driver::util::bus::SourceMetadataPayload;
using mldp_pvxs_driver::util::bus::TimeSeriesPayload;
using mldp_pvxs_driver::writer::WriterFactory;

namespace {

// ---------------------------------------------------------------------------
// Fake gRPC annotation service
// ---------------------------------------------------------------------------

class TestAnnotationService final
    : public dp::service::annotation::DpAnnotationService::Service
{
public:
    std::atomic<int> save_configuration_count{0};
    std::atomic<int> save_configuration_activation_count{0};
    bool             reject_configuration{false};
    bool             reject_activation{false};

    std::vector<dp::service::annotation::SaveConfigurationRequest>           captured_config_requests;
    std::vector<dp::service::annotation::SaveConfigurationActivationRequest> captured_activation_requests;
    std::mutex                                                                captured_mutex;

    grpc::Status saveConfiguration(
        grpc::ServerContext*,
        const dp::service::annotation::SaveConfigurationRequest* req,
        dp::service::annotation::SaveConfigurationResponse* response) override
    {
        save_configuration_count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(captured_mutex);
        captured_config_requests.push_back(*req);
        if (reject_configuration)
        {
            response->mutable_exceptionalresult()->set_message("configuration rejected for test");
        }
        else
        {
            response->mutable_saveconfigurationresult()->set_configurationname(req->configurationname());
        }
        return grpc::Status::OK;
    }

    grpc::Status saveConfigurationActivation(
        grpc::ServerContext*,
        const dp::service::annotation::SaveConfigurationActivationRequest* req,
        dp::service::annotation::SaveConfigurationActivationResponse* response) override
    {
        save_configuration_activation_count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(captured_mutex);
        captured_activation_requests.push_back(*req);
        if (reject_activation)
        {
            response->mutable_exceptionalresult()->set_message("activation rejected for test");
        }
        else
        {
            response->mutable_saveconfigurationactivationresult()->set_clientactivationid(req->clientactivationid());
        }
        return grpc::Status::OK;
    }
};

// ---------------------------------------------------------------------------
// Helper: poll an atomic counter until it reaches the target or timeout.
// ---------------------------------------------------------------------------

bool waitForCount(std::atomic<int>& counter, int target,
                  std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (counter.load(std::memory_order_relaxed) >= target)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return counter.load(std::memory_order_relaxed) >= target;
}

// ---------------------------------------------------------------------------
// Helper: build writer YAML anchored at the writer instance block.
//
// annotation_port  — the gRPC port the annotation pool connects to.
// query_port       — a different port for query-url (required by config
//                    parser: query-url != ingestion-url).
// ---------------------------------------------------------------------------

static std::string makeWriterYaml(int annotation_port, int query_port)
{
    std::ostringstream yaml;
    yaml << "name: cfg_writer_test\n"
         << "thread-pool: 1\n"
         << "deadline-seconds: 5\n"
         << "mldp-annotation-pool:\n"
         << "  provider-name: test-provider\n"
         << "  ingestion-url: 127.0.0.1:" << annotation_port << "\n"
         << "  query-url: 127.0.0.1:" << query_port << "\n"
         << "  annotation-url: 127.0.0.1:" << annotation_port << "\n"
         << "  min-conn: 1\n"
         << "  max-conn: 1\n";
    return yaml.str();
}

// ---------------------------------------------------------------------------
// Test 1 — WriterFactory creates a configuration writer.
// ---------------------------------------------------------------------------

TEST(MLDPConfigurationWriterTest, WriterFactoryCreatesConfigurationWriter)
{
    // Use static ports; writer is constructed but never started so no
    // network connection is attempted.
    const auto cfg = makeConfigFromYaml(makeWriterYaml(19990, 19991));
    auto writer = WriterFactory::create("mldp-configuration", cfg, nullptr);
    EXPECT_NE(writer, nullptr);
}

// ---------------------------------------------------------------------------
// Test 2 — acceptsPayload returns correct results for each payload type.
// ---------------------------------------------------------------------------

TEST(MLDPConfigurationWriterTest, AcceptsConfigurationAndActivationPayloads)
{
    const auto cfg = makeConfigFromYaml(makeWriterYaml(19990, 19991));
    auto writer = WriterFactory::create("mldp-configuration", cfg, nullptr);
    ASSERT_NE(writer, nullptr);

    EXPECT_TRUE(writer->acceptsPayload(ConfigurationPayload{}));
    EXPECT_TRUE(writer->acceptsPayload(ConfigurationActivationPayload{}));
    EXPECT_FALSE(writer->acceptsPayload(TimeSeriesPayload{}));
    EXPECT_FALSE(writer->acceptsPayload(SourceMetadataPayload{}));
}

// ---------------------------------------------------------------------------
// Test 3 — push(ConfigurationPayload) triggers saveConfiguration RPC.
// ---------------------------------------------------------------------------

TEST(MLDPConfigurationWriterTest, PushConfigurationPayloadCallsSaveConfiguration)
{
    // Start an ephemeral fake annotation server.
    TestAnnotationService service;
    grpc::ServerBuilder   builder;
    int                   annotation_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &annotation_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(annotation_port, 0);

    // query-url must differ from ingestion-url; use annotation_port+1 as a
    // dummy — the annotation pool never connects there.
    const auto cfg = makeConfigFromYaml(makeWriterYaml(annotation_port, annotation_port + 1));
    auto writer = WriterFactory::create("mldp-configuration", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    ASSERT_NO_THROW(writer->start());

    ConfigurationPayload payload;
    payload.root_source_name   = "test-root";
    payload.configuration_name = "MY_CONFIG";
    payload.category           = "beam_params";

    IDataBus::EventBatch batch;
    batch.payload     = std::move(payload);

    ASSERT_TRUE(writer->push(std::move(batch)));
    EXPECT_TRUE(waitForCount(service.save_configuration_count, 1,
                             std::chrono::milliseconds(2000)));

    {
        std::lock_guard<std::mutex> lock(service.captured_mutex);
        ASSERT_GE(service.captured_config_requests.size(), 1u);
        EXPECT_EQ(service.captured_config_requests[0].configurationname(), "MY_CONFIG");
        EXPECT_EQ(service.captured_config_requests[0].category(), "beam_params");
    }

    writer->stop();
    server->Shutdown();
}

// ---------------------------------------------------------------------------
// Test 4 — push(ConfigurationActivationPayload) triggers
//           saveConfigurationActivation RPC.
// ---------------------------------------------------------------------------

TEST(MLDPConfigurationWriterTest,
     PushActivationPayloadCallsSaveConfigurationActivation)
{
    TestAnnotationService service;
    grpc::ServerBuilder   builder;
    int                   annotation_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &annotation_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(annotation_port, 0);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(annotation_port, annotation_port + 1));
    auto writer = WriterFactory::create("mldp-configuration", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    ASSERT_NO_THROW(writer->start());

    ConfigurationActivationPayload payload;
    payload.configuration_name     = "MY_CONFIG";
    payload.start_time.epoch_seconds = 1000;
    payload.start_time.nanoseconds   = 0;

    IDataBus::EventBatch batch;
    batch.payload     = std::move(payload);

    ASSERT_TRUE(writer->push(std::move(batch)));
    EXPECT_TRUE(waitForCount(service.save_configuration_activation_count, 1,
                             std::chrono::milliseconds(2000)));

    {
        std::lock_guard<std::mutex> lock(service.captured_mutex);
        ASSERT_GE(service.captured_activation_requests.size(), 1u);
        EXPECT_EQ(service.captured_activation_requests[0].configurationname(),
                  "MY_CONFIG");
        EXPECT_EQ(service.captured_activation_requests[0].starttime().epochseconds(),
                  static_cast<decltype(
                      service.captured_activation_requests[0].starttime().epochseconds())>(1000));
    }

    writer->stop();
    server->Shutdown();
}

TEST(MLDPConfigurationWriterTest, ExceptionalActivationResponseCountsAsFailure)
{
    TestAnnotationService service;
    service.reject_activation = true;
    grpc::ServerBuilder builder;
    int annotation_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &annotation_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);

    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    auto writer = WriterFactory::create("mldp-configuration", makeConfigFromYaml(makeWriterYaml(annotation_port, annotation_port + 1)), metrics);
    ASSERT_NE(writer, nullptr);
    writer->start();

    IDataBus::EventBatch batch;
    batch.payload = ConfigurationActivationPayload{
        .client_activation_id = "rejected-activation",
        .configuration_name = "MY_CONFIG",
        .start_time = {.epoch_seconds = 1000, .nanoseconds = 0},
    };
    ASSERT_TRUE(writer->push(std::move(batch)));
    ASSERT_TRUE(waitForCount(service.save_configuration_activation_count, 1, std::chrono::milliseconds(2000)));
    writer->stop();

    EXPECT_DOUBLE_EQ(metrics->writerPushTotal({{"writer", "cfg_writer_test"}}), 0.0);
    EXPECT_DOUBLE_EQ(metrics->writerFailuresTotal({{"writer", "cfg_writer_test"}}), 1.0);
    server->Shutdown();
}

TEST(MLDPConfigurationWriterTest, ExceptionalConfigurationResponseCountsAsFailure)
{
    TestAnnotationService service;
    service.reject_configuration = true;
    grpc::ServerBuilder builder;
    int annotation_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &annotation_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);

    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    auto writer = WriterFactory::create("mldp-configuration", makeConfigFromYaml(makeWriterYaml(annotation_port, annotation_port + 1)), metrics);
    ASSERT_NE(writer, nullptr);
    writer->start();

    IDataBus::EventBatch batch;
    batch.payload = ConfigurationPayload{
        .configuration_name = "MY_CONFIG",
        .category = "test-category",
    };
    ASSERT_TRUE(writer->push(std::move(batch)));
    ASSERT_TRUE(waitForCount(service.save_configuration_count, 1, std::chrono::milliseconds(2000)));
    writer->stop();

    EXPECT_DOUBLE_EQ(metrics->writerPushTotal({{"writer", "cfg_writer_test"}}), 0.0);
    EXPECT_DOUBLE_EQ(metrics->writerFailuresTotal({{"writer", "cfg_writer_test"}}), 1.0);
    server->Shutdown();
}

// ---------------------------------------------------------------------------
// Test 5 — pushing a TimeSeriesPayload is silently ignored.
// ---------------------------------------------------------------------------

TEST(MLDPConfigurationWriterTest, PushNonConfigurationPayloadIsIgnored)
{
    TestAnnotationService service;
    grpc::ServerBuilder   builder;
    int                   annotation_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &annotation_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(annotation_port, 0);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(annotation_port, annotation_port + 1));
    auto writer = WriterFactory::create("mldp-configuration", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    ASSERT_NO_THROW(writer->start());

    IDataBus::EventBatch batch;
    batch.payload     = TimeSeriesPayload{.root_source_name = "test-root"};

    EXPECT_TRUE(writer->push(std::move(batch)));

    // Give a short window for any spurious call to arrive.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(service.save_configuration_count.load(), 0);

    writer->stop();
    server->Shutdown();
}

// ---------------------------------------------------------------------------
// Test 6 — writer does not throw when the endpoint is unreachable.
// ---------------------------------------------------------------------------

TEST(MLDPConfigurationWriterTest, GracefulOnUnreachableEndpoint)
{
    // Use a static port where nothing is listening.
    const auto cfg = makeConfigFromYaml(makeWriterYaml(19998, 19999));
    auto writer = WriterFactory::create("mldp-configuration", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    ASSERT_NO_THROW(writer->start());

    ConfigurationPayload payload;
    payload.root_source_name   = "test-root";
    payload.configuration_name = "UNREACHABLE_CONFIG";
    payload.category           = "test";

    IDataBus::EventBatch batch;
    batch.payload     = std::move(payload);

    EXPECT_NO_THROW(writer->push(std::move(batch)));
    EXPECT_NO_THROW(writer->stop());
}

// ---------------------------------------------------------------------------
// Test 7 — start() followed immediately by stop() does not crash.
// ---------------------------------------------------------------------------

TEST(MLDPConfigurationWriterTest, StartStopLifecycle)
{
    const auto cfg = makeConfigFromYaml(makeWriterYaml(19992, 19993));
    auto writer = WriterFactory::create("mldp-configuration", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    ASSERT_NO_THROW(writer->start());
    ASSERT_NO_THROW(writer->stop());
}

// ---------------------------------------------------------------------------
// Test 8 — regression for the production race: without itemRoutingKey(),
//           round-robin dispatch sends cfg to worker N and activation to
//           worker N+1.  saveConfiguration intentionally sleeps to guarantee
//           activation would win the race on buggy code.  With the fix,
//           both items for the same name hash to the same channel and are
//           processed FIFO, so cfg always completes first.
//
// This test is deterministic: it ALWAYS fails without the routing fix and
// ALWAYS passes with it.
// ---------------------------------------------------------------------------

TEST(MLDPConfigurationWriterTest,
     RoutingKeyEnsuresCfgBeforeActivationDespiteSlowCfgRpc)
{
    // Service that:
    //   • sleeps 30 ms in saveConfiguration (guarantees activation wins the
    //     race on unfixed code where both items run on different workers)
    //   • records global monotonic sequence numbers keyed by config name
    struct SlowSequencingService final
        : dp::service::annotation::DpAnnotationService::Service
    {
        std::atomic<int>                     global_seq{0};
        std::mutex                           mtx;
        std::unordered_map<std::string, int> cfg_seq;
        std::unordered_map<std::string, int> act_seq;
        std::atomic<int>                     cfg_count{0};
        std::atomic<int>                     act_count{0};

        grpc::Status saveConfiguration(
            grpc::ServerContext*,
            const dp::service::annotation::SaveConfigurationRequest* req,
            dp::service::annotation::SaveConfigurationResponse* resp) override
        {
            // Simulate a slow DB write — on buggy (round-robin) code this
            // gives the activation worker a 30 ms head start, ensuring it
            // arrives at the service first.
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const int s = global_seq.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(mtx);
                cfg_seq[req->configurationname()] = s;
            }
            cfg_count.fetch_add(1, std::memory_order_relaxed);
            resp->mutable_saveconfigurationresult()->set_configurationname(
                req->configurationname());
            return grpc::Status::OK;
        }

        grpc::Status saveConfigurationActivation(
            grpc::ServerContext*,
            const dp::service::annotation::SaveConfigurationActivationRequest* req,
            dp::service::annotation::SaveConfigurationActivationResponse* resp) override
        {
            const int s = global_seq.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(mtx);
                act_seq[req->configurationname()] = s;
            }
            act_count.fetch_add(1, std::memory_order_relaxed);
            resp->mutable_saveconfigurationactivationresult()->set_clientactivationid(
                req->clientactivationid());
            return grpc::Status::OK;
        }
    };

    SlowSequencingService service;
    grpc::ServerBuilder   builder;
    int                   annotation_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &annotation_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(annotation_port, 0);

    // 4 workers: without routing fix, cfg→worker0 and act→worker1 for the
    // same name; act would always win with the 30 ms cfg sleep.
    std::ostringstream yaml;
    yaml << "name: routing_regression_test\n"
         << "thread-pool: 4\n"
         << "deadline-seconds: 10\n"
         << "mldp-annotation-pool:\n"
         << "  provider-name: test-provider\n"
         << "  ingestion-url: 127.0.0.1:" << annotation_port << "\n"
         << "  query-url: 127.0.0.1:" << (annotation_port + 1) << "\n"
         << "  annotation-url: 127.0.0.1:" << annotation_port << "\n"
         << "  min-conn: 1\n"
         << "  max-conn: 4\n";

    auto writer = WriterFactory::create(
        "mldp-configuration", makeConfigFromYaml(yaml.str()), nullptr);
    ASSERT_NE(writer, nullptr);
    ASSERT_NO_THROW(writer->start());

    // Push cfg immediately followed by activation for each name — exactly
    // the pattern the SlacCalendarReader uses per calendar event.
    constexpr int kNames = 8;
    for (int i = 0; i < kNames; ++i)
    {
        const std::string name = "SCHED_EVENT_" + std::to_string(i);

        IDataBus::EventBatch cfg_batch;
        cfg_batch.payload = ConfigurationPayload{
            .root_source_name   = "slac-calendar",
            .configuration_name = name,
            .category           = "lcls",
        };
        ASSERT_TRUE(writer->push(std::move(cfg_batch)));

        IDataBus::EventBatch act_batch;
        act_batch.payload = ConfigurationActivationPayload{
            .configuration_name = name,
            .start_time         = {.epoch_seconds = static_cast<uint64_t>(1700000000 + i),
                                   .nanoseconds   = 0},
        };
        ASSERT_TRUE(writer->push(std::move(act_batch)));
    }

    // 8 names × 30 ms cfg sleep + margin
    EXPECT_TRUE(waitForCount(service.cfg_count, kNames, std::chrono::milliseconds(10000)));
    EXPECT_TRUE(waitForCount(service.act_count, kNames, std::chrono::milliseconds(10000)));

    writer->stop();
    server->Shutdown();

    // Core assertion: for every event name, saveConfiguration must have
    // been issued (lower global seq#) before saveConfigurationActivation.
    // Without itemRoutingKey this fails deterministically because the 30 ms
    // sleep lets the activation worker lap the cfg worker every time.
    std::lock_guard<std::mutex> lk(service.mtx);
    for (int i = 0; i < kNames; ++i)
    {
        const std::string name = "SCHED_EVENT_" + std::to_string(i);
        ASSERT_TRUE(service.cfg_seq.count(name))
            << "saveConfiguration never called for " << name;
        ASSERT_TRUE(service.act_seq.count(name))
            << "saveConfigurationActivation never called for " << name;
        EXPECT_LT(service.cfg_seq.at(name), service.act_seq.at(name))
            << name << ": activation arrived before configuration (routing bug)";
    }
}

} // namespace
