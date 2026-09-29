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
#include <unordered_map>
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

    std::mutex                                                          seed_mutex;
    std::unordered_map<std::string, dp::service::common::PvMetadata>   seeded_metadata;

    void seedPvMetadata(const std::string& pvName, const dp::service::common::PvMetadata& meta)
    {
        std::lock_guard<std::mutex> lock(seed_mutex);
        seeded_metadata[pvName] = meta;
    }

    grpc::Status getPvMetadata(grpc::ServerContext*,
                               const dp::service::annotation::GetPvMetadataRequest*  request,
                               dp::service::annotation::GetPvMetadataResponse*       response) override
    {
        std::lock_guard<std::mutex> lock(seed_mutex);
        auto it = seeded_metadata.find(request->pvnameoralias());
        if (it == seeded_metadata.end())
        {
            response->mutable_exceptionalresult()->set_message("not found");
            return grpc::Status::OK;
        }
        *response->mutable_getpvmetadataresult()->mutable_pvmetadata() = it->second;
        return grpc::Status::OK;
    }

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

// 6. stop() remains idempotent after a clean start/stop cycle.
TEST(MLDPPVMetadataWriterTest, StopIsIdempotentAfterLifecycle)
{
    const auto cfg = makeConfigFromYaml(
        "name: annotation_test\n"
        "thread-pool: 1\n"
        "deadline-seconds: 1\n"
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
    ASSERT_NO_THROW(writer->stop());
    ASSERT_NO_THROW(writer->stop());
}

// ---------------------------------------------------------------------------
// 7. Merge: existing PV has tags/attrs; incoming has only new attrs →
//    result preserves existing tags and merges attributes.
// ---------------------------------------------------------------------------

TEST(MLDPPVMetadataWriterTest, MergesWithExistingPvMetadata)
{
    TestAnnotationService service;

    // Seed existing PV metadata on the fake server.
    dp::service::common::PvMetadata existing;
    existing.set_pvname("MY:PV");
    existing.add_tags("existing-tag");
    existing.set_description("original desc");
    existing.set_modifiedby("original-user");
    auto* attr1 = existing.add_attributes();
    attr1->set_name("keep_me");
    attr1->set_value("old_val");
    auto* attr2 = existing.add_attributes();
    attr2->set_name("overwrite_me");
    attr2->set_value("old_val");
    service.seedPvMetadata("MY:PV", existing);

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(
        "127.0.0.1:" + std::to_string(port),
        "127.0.0.1:" + std::to_string(port + 1),
        "127.0.0.1:" + std::to_string(port + 2)));
    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    writer->start();

    // Incoming entry: no tags, no aliases, new attr + overwrite one, new description.
    SourceMetadataEntry entry;
    entry.attributes = {{"overwrite_me", "new_val"}, {"new_key", "new_val"}};
    entry.description = std::string("updated desc");
    // tags intentionally absent → should preserve "existing-tag"
    // modified_by intentionally absent → should preserve "original-user"

    SourceMetadataPayload meta_payload;
    meta_payload.root_source_name = "MY:PV";
    meta_payload.sources["MY:PV"] = std::move(entry);

    IDataBus::EventBatch batch;
    batch.reader_name = "test_reader";
    batch.payload     = std::move(meta_payload);

    ASSERT_TRUE(writer->push(std::move(batch)));
    ASSERT_TRUE(waitForCount(service.save_pv_metadata_count, 1, std::chrono::milliseconds(2000)));

    writer->stop();
    server->Shutdown();

    std::lock_guard<std::mutex> lock(service.captured_mutex);
    ASSERT_EQ(service.captured_requests.size(), 1u);
    const auto& req = service.captured_requests[0];

    // Tags preserved from existing.
    ASSERT_EQ(req.tags_size(), 1);
    EXPECT_EQ(req.tags(0), "existing-tag");

    // Description overwritten by incoming.
    EXPECT_EQ(req.description(), "updated desc");

    // modified_by preserved from existing (incoming was absent).
    EXPECT_EQ(req.modifiedby(), "original-user");

    // Attributes merged: keep_me preserved, overwrite_me updated, new_key added.
    std::unordered_map<std::string, std::string> attrs;
    for (const auto& a : req.attributes())
        attrs[a.name()] = a.value();

    EXPECT_EQ(attrs.size(), 3u);
    EXPECT_EQ(attrs["keep_me"], "old_val");
    EXPECT_EQ(attrs["overwrite_me"], "new_val");
    EXPECT_EQ(attrs["new_key"], "new_val");
}

// ---------------------------------------------------------------------------
// 8. No existing record → standard create behavior (no merge side-effects).
// ---------------------------------------------------------------------------

TEST(MLDPPVMetadataWriterTest, NewPvWithoutExistingRecordUsesEntryAsIs)
{
    TestAnnotationService service;
    // No seeded metadata — getPvMetadata will return "not found".

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(
        "127.0.0.1:" + std::to_string(port),
        "127.0.0.1:" + std::to_string(port + 1),
        "127.0.0.1:" + std::to_string(port + 2)));
    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    writer->start();

    SourceMetadataEntry entry;
    entry.tags        = std::vector<std::string>{"new-tag"};
    entry.description = std::string("brand new");
    entry.attributes  = {{"a", "1"}};

    SourceMetadataPayload meta_payload;
    meta_payload.root_source_name   = "NEW:PV";
    meta_payload.sources["NEW:PV"]  = std::move(entry);

    IDataBus::EventBatch batch;
    batch.reader_name = "test_reader";
    batch.payload     = std::move(meta_payload);

    ASSERT_TRUE(writer->push(std::move(batch)));
    ASSERT_TRUE(waitForCount(service.save_pv_metadata_count, 1, std::chrono::milliseconds(2000)));

    writer->stop();
    server->Shutdown();

    std::lock_guard<std::mutex> lock(service.captured_mutex);
    ASSERT_EQ(service.captured_requests.size(), 1u);
    const auto& req = service.captured_requests[0];

    EXPECT_EQ(req.pvname(), "NEW:PV");
    ASSERT_EQ(req.tags_size(), 1);
    EXPECT_EQ(req.tags(0), "new-tag");
    EXPECT_EQ(req.description(), "brand new");
    ASSERT_EQ(req.attributes_size(), 1);
    EXPECT_EQ(req.attributes(0).name(), "a");
    EXPECT_EQ(req.attributes(0).value(), "1");
}

// ---------------------------------------------------------------------------
// 9. Incoming tags override existing tags entirely when present.
// ---------------------------------------------------------------------------

TEST(MLDPPVMetadataWriterTest, IncomingTagsReplaceExistingTags)
{
    TestAnnotationService service;

    dp::service::common::PvMetadata existing;
    existing.set_pvname("MY:PV");
    existing.add_tags("old-tag-1");
    existing.add_tags("old-tag-2");
    service.seedPvMetadata("MY:PV", existing);

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(
        "127.0.0.1:" + std::to_string(port),
        "127.0.0.1:" + std::to_string(port + 1),
        "127.0.0.1:" + std::to_string(port + 2)));
    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    writer->start();

    SourceMetadataEntry entry;
    entry.tags = std::vector<std::string>{"new-tag-only"};

    SourceMetadataPayload meta_payload;
    meta_payload.root_source_name = "MY:PV";
    meta_payload.sources["MY:PV"] = std::move(entry);

    IDataBus::EventBatch batch;
    batch.reader_name = "test_reader";
    batch.payload     = std::move(meta_payload);

    ASSERT_TRUE(writer->push(std::move(batch)));
    ASSERT_TRUE(waitForCount(service.save_pv_metadata_count, 1, std::chrono::milliseconds(2000)));

    writer->stop();
    server->Shutdown();

    std::lock_guard<std::mutex> lock(service.captured_mutex);
    ASSERT_EQ(service.captured_requests.size(), 1u);
    const auto& req = service.captured_requests[0];

    ASSERT_EQ(req.tags_size(), 1);
    EXPECT_EQ(req.tags(0), "new-tag-only");
}

// ---------------------------------------------------------------------------
// 10. Empty-valued attributes must not be sent (new-record path).
// ---------------------------------------------------------------------------

TEST(MLDPPVMetadataWriterTest, EmptyAttributesOmittedOnNewRecord)
{
    TestAnnotationService service;
    // No seeded metadata — getPvMetadata will return "not found".

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(
        "127.0.0.1:" + std::to_string(port),
        "127.0.0.1:" + std::to_string(port + 1),
        "127.0.0.1:" + std::to_string(port + 2)));
    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    writer->start();

    SourceMetadataEntry entry;
    entry.attributes = {{"dname", ""}, {"ename", ""}, {"z", "some_z"}};

    SourceMetadataPayload meta_payload;
    meta_payload.root_source_name              = "WIRE_LI28_144_POSNCUSBR";
    meta_payload.sources["WIRE_LI28_144_POSNCUSBR"] = std::move(entry);

    IDataBus::EventBatch batch;
    batch.reader_name = "test_reader";
    batch.payload     = std::move(meta_payload);

    ASSERT_TRUE(writer->push(std::move(batch)));
    ASSERT_TRUE(waitForCount(service.save_pv_metadata_count, 1, std::chrono::milliseconds(2000)));

    writer->stop();
    server->Shutdown();

    std::lock_guard<std::mutex> lock(service.captured_mutex);
    ASSERT_EQ(service.captured_requests.size(), 1u);
    const auto& req = service.captured_requests[0];

    // Only the non-empty attribute should be present.
    ASSERT_EQ(req.attributes_size(), 1);
    EXPECT_EQ(req.attributes(0).name(), "z");
    EXPECT_EQ(req.attributes(0).value(), "some_z");
}

// ---------------------------------------------------------------------------
// 11. Empty-valued attributes must not be sent when merging with existing.
// ---------------------------------------------------------------------------

TEST(MLDPPVMetadataWriterTest, EmptyAttributesOmittedOnMerge)
{
    TestAnnotationService service;

    dp::service::common::PvMetadata existing;
    existing.set_pvname("MY:PV");
    auto* attr1 = existing.add_attributes();
    attr1->set_name("keep_me");
    attr1->set_value("old_val");
    service.seedPvMetadata("MY:PV", existing);

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    const auto cfg = makeConfigFromYaml(makeWriterYaml(
        "127.0.0.1:" + std::to_string(port),
        "127.0.0.1:" + std::to_string(port + 1),
        "127.0.0.1:" + std::to_string(port + 2)));
    auto writer = WriterFactory::create("mldp-pv-metadata", cfg, nullptr);
    ASSERT_NE(writer, nullptr);
    writer->start();

    // Incoming clears keep_me's value to empty and adds another empty attr.
    SourceMetadataEntry entry;
    entry.attributes = {{"keep_me", ""}, {"blank_attr", ""}, {"real_attr", "val"}};

    SourceMetadataPayload meta_payload;
    meta_payload.root_source_name = "MY:PV";
    meta_payload.sources["MY:PV"] = std::move(entry);

    IDataBus::EventBatch batch;
    batch.reader_name = "test_reader";
    batch.payload     = std::move(meta_payload);

    ASSERT_TRUE(writer->push(std::move(batch)));
    ASSERT_TRUE(waitForCount(service.save_pv_metadata_count, 1, std::chrono::milliseconds(2000)));

    writer->stop();
    server->Shutdown();

    std::lock_guard<std::mutex> lock(service.captured_mutex);
    ASSERT_EQ(service.captured_requests.size(), 1u);
    const auto& req = service.captured_requests[0];

    std::unordered_map<std::string, std::string> attrs;
    for (const auto& a : req.attributes())
        attrs[a.name()] = a.value();

    // keep_me and blank_attr overwritten/added with empty value → must be omitted.
    EXPECT_EQ(attrs.size(), 1u);
    EXPECT_EQ(attrs.count("keep_me"), 0u);
    EXPECT_EQ(attrs.count("blank_attr"), 0u);
    EXPECT_EQ(attrs["real_attr"], "val");
}

} // namespace
