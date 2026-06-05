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
#include <util/bus/IDataBus.h>
#include <writer/WriterFactory.h>
#include <writer/mldp_pv_metadata/MLDPPVMetadataWriter.h>

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
using mldp_pvxs_driver::writer::WriterFactory;
using mldp_pvxs_driver::util::bus::IDataBus;
using mldp_pvxs_driver::util::bus::BatchPayload;
using mldp_pvxs_driver::util::bus::SourceMetadataPayload;
using mldp_pvxs_driver::util::bus::SourceMetadataEntry;
using mldp_pvxs_driver::util::bus::TimeSeriesPayload;
using mldp_pvxs_driver::util::bus::ConfigurationPayload;
using mldp_pvxs_driver::util::bus::ConfigurationActivationPayload;

namespace {

// ---------------------------------------------------------------------------
// Fake DpAnnotationService server for unit tests
// ---------------------------------------------------------------------------

class TestAnnotationService final : public dp::service::annotation::DpAnnotationService::Service
{
public:
    std::atomic<int>                                                    save_pv_metadata_count{0};
    std::vector<dp::service::annotation::SavePvMetadataRequest>         captured_requests;
    std::mutex                                                          captured_mutex;

    grpc::Status savePvMetadata(grpc::ServerContext*,
                                const dp::service::annotation::SavePvMetadataRequest* request,
                                dp::service::annotation::SavePvMetadataResponse*      response) override
    {
        {
            std::lock_guard<std::mutex> lock(captured_mutex);
            captured_requests.push_back(*request);
        }
        save_pv_metadata_count.fetch_add(1, std::memory_order_relaxed);
        return grpc::Status::OK;
    }
};

// ---------------------------------------------------------------------------
// Helper: wait until counter reaches target or timeout expires
// ---------------------------------------------------------------------------

bool waitForCount(std::atomic<int>& counter, int target, std::chrono::milliseconds timeout)
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
// Helper: build a writer config YAML pointing at annotation-url <addr>
// ---------------------------------------------------------------------------

std::string makeWriterYaml(const std::string& annotation_addr,
                           const std::string& ingestion_addr  = "127.0.0.1:50051",
                           const std::string& query_addr      = "127.0.0.1:50052")
{
    std::ostringstream yaml;
    yaml << "name: annotation_test\n"
         << "thread-pool: 1\n"
         << "deadline-seconds: 5\n"
         << "mldp-pv-metadata-pool:\n"
         << "  provider-name: test-provider\n"
         << "  ingestion-url: " << ingestion_addr << "\n"
         << "  query-url: "     << query_addr     << "\n"
         << "  annotation-url: " << annotation_addr << "\n"
         << "  min-conn: 1\n"
         << "  max-conn: 1\n";
    return yaml.str();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. WriterFactory recognises the "mldp-pv-metadata" type and returns non-null.
TEST(MLDPPVMetadataWriterTest, WriterFactoryCreatesAnnotationWriter)
{
    // No real server is needed — we only test factory registration.
    const auto cfg = makeConfigFromYaml(
        "name: annotation_test\n"
        "mldp-pv-metadata-pool:\n"
        "  provider-name: test-provider\n"
        "  ingestion-url: 127.0.0.1:50051\n"
        "  query-url: 127.0.0.1:50052\n"
        "  annotation-url: 127.0.0.1:50053\n"
        "  min-conn: 1\n"
        "  max-conn: 1\n");

    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
}

// 2. acceptsPayload returns true only for SourceMetadataPayload.
TEST(MLDPPVMetadataWriterTest, AcceptsOnlySourceMetadataPayload)
{
    const auto cfg = makeConfigFromYaml(
        "name: annotation_test\n"
        "mldp-pv-metadata-pool:\n"
        "  provider-name: test-provider\n"
        "  ingestion-url: 127.0.0.1:50051\n"
        "  query-url: 127.0.0.1:50052\n"
        "  annotation-url: 127.0.0.1:50053\n"
        "  min-conn: 1\n"
        "  max-conn: 1\n");

    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);

    ASSERT_TRUE(writer->acceptsPayload(SourceMetadataPayload{}));
    ASSERT_FALSE(writer->acceptsPayload(TimeSeriesPayload{}));
    ASSERT_FALSE(writer->acceptsPayload(ConfigurationPayload{}));
    ASSERT_FALSE(writer->acceptsPayload(ConfigurationActivationPayload{}));
}

// 3. Pushing a SourceMetadataPayload causes savePvMetadata to be called on the server.
TEST(MLDPPVMetadataWriterTest, PushSourceMetadataCallsSavePvMetadata)
{
    TestAnnotationService service;
    grpc::ServerBuilder   builder;
    int                   port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    const std::string annotation_addr = "127.0.0.1:" + std::to_string(port);
    // ingestion and query must differ; use unused port variants
    const std::string ingestion_addr  = "127.0.0.1:" + std::to_string(port + 1);
    const std::string query_addr      = "127.0.0.1:" + std::to_string(port + 2);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(annotation_addr, ingestion_addr, query_addr));
    auto       writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);

    ASSERT_NO_THROW(writer->start());

    SourceMetadataEntry entry;
    entry.description = std::string("test pv");
    entry.attributes  = {{"key1", "val1"}};

    SourceMetadataPayload meta_payload;
    meta_payload.root_source_name = "MY:PV";
    meta_payload.sources["MY:PV"] = std::move(entry);

    IDataBus::EventBatch batch;
    batch.reader_name = "test_reader";
    batch.payload     = std::move(meta_payload);

    EXPECT_TRUE(writer->push(std::move(batch)));

    ASSERT_TRUE(waitForCount(service.save_pv_metadata_count, 1,
                             std::chrono::milliseconds(2000)));

    {
        std::lock_guard<std::mutex> lock(service.captured_mutex);
        ASSERT_EQ(service.captured_requests.size(), 1u);
        EXPECT_EQ(service.captured_requests[0].pvname(), "MY:PV");
    }

    writer->stop();
    server->Shutdown();
}

// 4. Pushing a non-metadata payload does not invoke savePvMetadata.
TEST(MLDPPVMetadataWriterTest, PushNonMetadataPayloadIsIgnored)
{
    TestAnnotationService service;
    grpc::ServerBuilder   builder;
    int                   port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    const std::string annotation_addr = "127.0.0.1:" + std::to_string(port);
    const std::string ingestion_addr2 = "127.0.0.1:" + std::to_string(port + 1);
    const std::string query_addr2     = "127.0.0.1:" + std::to_string(port + 2);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(annotation_addr, ingestion_addr2, query_addr2));
    auto       writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);

    ASSERT_NO_THROW(writer->start());

    IDataBus::EventBatch batch;
    batch.reader_name = "test_reader";
    batch.payload     = TimeSeriesPayload{.root_source_name = "SOME:PV"};

    const bool result = writer->push(std::move(batch));
    EXPECT_TRUE(result);

    // Allow time for any spurious async work (there should be none).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(service.save_pv_metadata_count.load(), 0);

    writer->stop();
    server->Shutdown();
}

// 5. Constructing and operating against an unreachable endpoint does not throw.
TEST(MLDPPVMetadataWriterTest, GracefulOnUnreachableEndpoint)
{
    // Port 19999 is expected to have nothing listening.
    const auto cfg = makeConfigFromYaml(
        "name: annotation_test\n"
        "thread-pool: 1\n"
        "deadline-seconds: 1\n"
        "mldp-pv-metadata-pool:\n"
        "  provider-name: test-provider\n"
        "  ingestion-url: 127.0.0.1:19998\n"
        "  query-url: 127.0.0.1:19997\n"
        "  annotation-url: 127.0.0.1:19999\n"
        "  min-conn: 1\n"
        "  max-conn: 1\n");

    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);

    EXPECT_NO_THROW(writer->start());

    SourceMetadataEntry entry;
    entry.description = std::string("unreachable test pv");

    SourceMetadataPayload meta_payload;
    meta_payload.root_source_name       = "UNREACHABLE:PV";
    meta_payload.sources["UNREACHABLE:PV"] = std::move(entry);

    IDataBus::EventBatch batch;
    batch.reader_name = "test_reader";
    batch.payload     = std::move(meta_payload);

    // push must not throw even if the RPC will eventually fail
    EXPECT_NO_THROW(writer->push(std::move(batch)));

    EXPECT_NO_THROW(writer->stop());
}

// 6. start() followed by stop() without any pushes completes without errors.
TEST(MLDPPVMetadataWriterTest, StartStopLifecycle)
{
    const auto cfg = makeConfigFromYaml(
        "name: annotation_test\n"
        "mldp-pv-metadata-pool:\n"
        "  provider-name: test-provider\n"
        "  ingestion-url: 127.0.0.1:50051\n"
        "  query-url: 127.0.0.1:50052\n"
        "  annotation-url: 127.0.0.1:50053\n"
        "  min-conn: 1\n"
        "  max-conn: 1\n");

    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);

    ASSERT_NO_THROW(writer->start());
    EXPECT_TRUE(writer->isHealthy());
    ASSERT_NO_THROW(writer->stop());
}

} // namespace
