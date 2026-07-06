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

#include <controller/MLDPPVXSController.h>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <ingestion.grpc.pb.h>

#include "../common/MldpQueryTestUtils.h"
#include "../config/test_config_helpers.h"
#include "../mock/BsasGen1HDF5Mock.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::test::mock::BsasGen1HDF5Mock;
using mldp_pvxs_driver::testutil::queryAndCollectColumns;

namespace fs = std::filesystem;

namespace {

using ColumnBuckets = std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>;
using Attributes = google::protobuf::RepeatedPtrField<dp::service::common::Attribute>;

// ---------------------------------------------------------------------------
// Fake gRPC ingestion service — captures IngestDataRequest messages
// ---------------------------------------------------------------------------
class CaptureIngestionService final
    : public dp::service::ingestion::DpIngestionService::Service
{
public:
    std::atomic<int>                                         request_count{0};
    std::vector<dp::service::ingestion::IngestDataRequest>  captured;
    std::mutex                                               mu;

    grpc::Status registerProvider(
        grpc::ServerContext*,
        const dp::service::ingestion::RegisterProviderRequest* req,
        dp::service::ingestion::RegisterProviderResponse*      resp) override
    {
        auto* r = resp->mutable_registrationresult();
        r->set_providerid("test-provider-id");
        r->set_providername(req->providername());
        r->set_isnewprovider(true);
        return grpc::Status::OK;
    }

    grpc::Status ingestDataStream(
        grpc::ServerContext*,
        grpc::ServerReader<dp::service::ingestion::IngestDataRequest>* reader,
        dp::service::ingestion::IngestDataStreamResponse*) override
    {
        dp::service::ingestion::IngestDataRequest req;
        while (reader->Read(&req))
        {
            request_count.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(mu);
            captured.push_back(req);
        }
        return grpc::Status::OK;
    }
};

bool waitForRequests(std::atomic<int>& counter, int target, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (counter.load(std::memory_order_relaxed) >= target)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return counter.load(std::memory_order_relaxed) >= target;
}

constexpr auto kQueryTimeout = std::chrono::seconds(20);
constexpr auto kLookbackWindow = std::chrono::seconds(120);
constexpr auto kStartupDelay = std::chrono::milliseconds(1500);

std::string makeUniquePrefix(const std::string& base)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return base + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now).count()) + "_";
}

std::string buildYaml(const fs::path& hdf5Path,
                      const std::string& readerName,
                      const std::string& sourceTag,
                      const std::unordered_map<std::string, std::string>& provenance)
{
    std::ostringstream yaml;
    yaml << "writer:\n"
         << "  mldp:\n"
         << "    - name: mldp_prov\n"
         << "      mldp-pool:\n"
         << "        provider-name: prov-test-provider\n"
         << "        ingestion-url: dp-ingestion:50051\n"
         << "        query-url: dp-query:50052\n"
         << "        min-conn: 1\n"
         << "        max-conn: 1\n"
         << "reader:\n"
         << "  hdf5-bsas-gen1:\n"
         << "    - name: " << readerName << "\n"
         << "      file-path: " << hdf5Path.string() << "\n"
         << "      chunk-size: 1000\n"
         << "      use-label-as-name: false\n"
         << "      metadata:\n"
         << "        test_run: " << sourceTag << "\n";

    if (!provenance.empty())
    {
        yaml << "      provenance:\n";
        for (const auto& [key, value] : provenance)
        {
            yaml << "        " << key << ": " << value << "\n";
        }
    }

    return yaml.str();
}

bool hasAttribute(const Attributes& attrs, const std::string& name, const std::string& value)
{
    for (const auto& attr : attrs)
    {
        if (attr.name() == name && attr.value() == value)
        {
            return true;
        }
    }
    return false;
}

bool hasProvenanceAttributePrefix(const Attributes& attrs)
{
    for (const auto& attr : attrs)
    {
        if (attr.name().rfind("provenance.", 0) == 0)
        {
            return true;
        }
    }
    return false;
}

const auto& metadataFromValues(const dp::service::common::DataValues& values)
{
    using DataValues = dp::service::common::DataValues;

    switch (values.values_case())
    {
    case DataValues::kDoubleColumn:
        return values.doublecolumn().metadata();
    case DataValues::kFloatColumn:
        return values.floatcolumn().metadata();
    case DataValues::kInt32Column:
        return values.int32column().metadata();
    case DataValues::kInt64Column:
        return values.int64column().metadata();
    case DataValues::kBoolColumn:
        return values.boolcolumn().metadata();
    case DataValues::kStringColumn:
        return values.stringcolumn().metadata();
    case DataValues::kDoubleArrayColumn:
        return values.doublearraycolumn().metadata();
    case DataValues::kFloatArrayColumn:
        return values.floatarraycolumn().metadata();
    case DataValues::kInt32ArrayColumn:
        return values.int32arraycolumn().metadata();
    case DataValues::kInt64ArrayColumn:
        return values.int64arraycolumn().metadata();
    case DataValues::kBoolArrayColumn:
        return values.boolarraycolumn().metadata();
    default:
        throw std::runtime_error("Unsupported DataValues variant in provenance integration test");
    }
}

class ControllerHDF5ProvenanceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info = testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = fs::temp_directory_path() / info->test_suite_name() / info->name();
        fs::remove_all(tempDir_);
        fs::create_directories(tempDir_);
        mockFile_ = tempDir_ / "provenance_test.h5";

    }

    void TearDown() override
    {
        if (controller_)
        {
            controller_->stop();
            controller_.reset();
        }
        fs::remove_all(tempDir_);
    }

    ColumnBuckets runPipeline(const std::string& readerName,
                              const std::string& sourceTag,
                              const std::string& floatPrefix,
                              const std::unordered_map<std::string, std::string>& provenance,
                              const std::vector<std::string>& pvNames)
    {
        BsasGen1HDF5Mock::Params params;
        params.numFloatCols = 3;
        params.numIntCols = 2;
        params.numRows = 5;
        params.baseEpoch = static_cast<uint32_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) - 5);
        params.floatColPrefix = floatPrefix;
        BsasGen1HDF5Mock::generate(mockFile_.string(), params);

        auto config = makeConfigFromYaml(buildYaml(mockFile_, readerName, sourceTag, provenance));
        if (!config.valid())
        {
            ADD_FAILURE() << "Generated controller config is invalid";
            return {};
        }

        controller_ = mldp_pvxs_driver::controller::MLDPPVXSController::create(config);
        if (!controller_)
        {
            ADD_FAILURE() << "Failed to create controller for provenance integration test";
            return {};
        }
        controller_->start();

        // Give the HDF5 reader enough time to emit its one-shot file contents
        // before the first query poll against MLDP.
        std::this_thread::sleep_for(kStartupDelay);

        auto columns = queryAndCollectColumns(pvNames, kQueryTimeout, kLookbackWindow);
        if (!columns.has_value())
        {
            ADD_FAILURE() << "Failed to query MLDP columns for provenance verification";
            controller_->stop();
            controller_.reset();
            return {};
        }

        controller_->stop();
        controller_.reset();
        return *columns;
    }

    static bool isCurrentReaderBucket(const dp::service::common::DataValues& bucket,
                                      const std::string&                    readerName,
                                      const std::string&                    sourceTag)
    {
        const auto& metadata = metadataFromValues(bucket);
        const auto& attrs = metadata.attributes();

        if (!hasAttribute(attrs, "source", readerName))
        {
            return false;
        }
        if (metadata.provenance().source() != readerName)
        {
            return false;
        }
        if (!hasAttribute(attrs, "test_run", sourceTag))
        {
            return false;
        }
        return true;
    }

    static std::vector<dp::service::common::DataValues> filterCurrentReaderBuckets(
        const std::vector<dp::service::common::DataValues>& buckets,
        const std::string&                                  readerName,
        const std::string&                                  sourceTag)
    {
        std::vector<dp::service::common::DataValues> filtered;
        for (const auto& bucket : buckets)
        {
            if (isCurrentReaderBucket(bucket, readerName, sourceTag))
            {
                filtered.push_back(bucket);
            }
        }
        return filtered;
    }

    fs::path                                                           tempDir_;
    fs::path                                                           mockFile_;
    std::shared_ptr<mldp_pvxs_driver::controller::MLDPPVXSController> controller_;
};

} // namespace

TEST_F(ControllerHDF5ProvenanceTest, ProvenanceFlowsFromHDF5ReaderThroughControllerToMLDP)
{
    const std::string readerName = "bsas_prov_test";
    const std::string sourceTag = readerName + "_run";
    const std::string floatPrefix = makeUniquePrefix("PROV_SIG_");
    const auto columns = runPipeline(readerName,
                                     sourceTag,
                                     floatPrefix,
                                     {{"facility", "LCLS"}, {"instrument", "CXI"}, {"subsystem", "BSAS"}},
                                     {floatPrefix + "0000", floatPrefix + "0001"});

    ASSERT_EQ(columns.size(), 2u);
    for (const auto& [pvName, buckets] : columns)
    {
        const auto currentBuckets = filterCurrentReaderBuckets(buckets, readerName, sourceTag);
        ASSERT_FALSE(currentBuckets.empty()) << "No queried buckets returned for current reader for " << pvName;
        for (const auto& bucket : currentBuckets)
        {
            const auto& metadata = metadataFromValues(bucket);
            const auto& attrs = metadata.attributes();
            EXPECT_TRUE(hasAttribute(attrs, "facility", "LCLS"));
            EXPECT_TRUE(hasAttribute(attrs, "instrument", "CXI"));
            EXPECT_TRUE(hasAttribute(attrs, "subsystem", "BSAS"));
            EXPECT_TRUE(hasAttribute(attrs, "source", readerName));
            EXPECT_TRUE(hasAttribute(attrs, "test_run", sourceTag));
            EXPECT_EQ(metadata.provenance().source(), readerName);
        }
    }
}

TEST_F(ControllerHDF5ProvenanceTest, MissingProvenanceDoesNotEmitExtraAttributes)
{
    const std::string readerName = "bsas_noprov_test";
    const std::string sourceTag = readerName + "_run";
    const std::string floatPrefix = makeUniquePrefix("NOPROV_SIG_");
    const auto columns = runPipeline(readerName,
                                     sourceTag,
                                     floatPrefix,
                                     {},
                                     {floatPrefix + "0000", floatPrefix + "0001"});

    ASSERT_EQ(columns.size(), 2u);
    for (const auto& [pvName, buckets] : columns)
    {
        const auto currentBuckets = filterCurrentReaderBuckets(buckets, readerName, sourceTag);
        ASSERT_FALSE(currentBuckets.empty()) << "No queried buckets returned for current reader for " << pvName;
        for (const auto& bucket : currentBuckets)
        {
            const auto& metadata = metadataFromValues(bucket);
            const auto& attrs = metadata.attributes();
            EXPECT_FALSE(hasProvenanceAttributePrefix(attrs));
            EXPECT_TRUE(hasAttribute(attrs, "source", readerName));
            EXPECT_TRUE(hasAttribute(attrs, "test_run", sourceTag));
            EXPECT_EQ(metadata.provenance().source(), readerName);
        }
    }
}

// ---------------------------------------------------------------------------
// shardSlot end-to-end: HDF5BsasGen1Reader → MLDPWriter → gRPC column attributes
// ---------------------------------------------------------------------------

TEST(HDF5BsasGen1ReaderToMLDPWriterTest, ShardSlotAppearsInGrpcColumnAttributes)
{
    // Stand up a fake gRPC ingestion server on an OS-assigned port.
    CaptureIngestionService service;
    grpc::ServerBuilder     builder;
    int                     port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    // Generate a small HDF5 mock file — 3 float cols, 2 int cols, 5 rows.
    const auto tempDir  = std::filesystem::temp_directory_path() / "shard_slot_test";
    std::filesystem::create_directories(tempDir);
    const auto mockFile = (tempDir / "shard_slot.h5").string();

    BsasGen1HDF5Mock::Params params;
    params.numFloatCols = 3;
    params.numIntCols   = 2;
    params.numRows      = 5;
    params.baseEpoch    = static_cast<uint32_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) - 5);
    BsasGen1HDF5Mock::generate(mockFile, params);

    // Wire HDF5BsasGen1Reader → MLDPWriter via controller config.
    std::ostringstream yaml;
    yaml << "writer:\n"
         << "  mldp:\n"
         << "    - name: mldp_shard\n"
         << "      mldp-pool:\n"
         << "        provider-name: shard_test_provider\n"
         << "        ingestion-url: 127.0.0.1:" << port << "\n"
         << "        query-url: 127.0.0.1:" << port << "\n"
         << "        min-conn: 1\n"
         << "        max-conn: 1\n"
         << "reader:\n"
         << "  hdf5-bsas-gen1:\n"
         << "    - name: shard_reader\n"
         << "      file-path: " << mockFile << "\n"
         << "      chunk-size: 1000\n"
         << "      use-label-as-name: false\n";

    const auto config = makeConfigFromYaml(yaml.str());
    ASSERT_TRUE(config.valid()) << "Controller config invalid";

    auto controller = mldp_pvxs_driver::controller::MLDPPVXSController::create(config);
    ASSERT_TRUE(controller);
    controller->start();

    // Wait for at least (numFloatCols + numIntCols) = 5 column requests.
    const bool got_requests =
        waitForRequests(service.request_count, 5, std::chrono::milliseconds(5000));
    EXPECT_TRUE(got_requests) << "Timed out waiting for ingestion requests";

    controller->stop();
    server->Shutdown();
    std::filesystem::remove_all(tempDir);

    std::vector<dp::service::ingestion::IngestDataRequest> captured;
    {
        std::lock_guard<std::mutex> lk(service.mu);
        captured = service.captured;
    }
    ASSERT_FALSE(captured.empty()) << "No IngestDataRequest captured";

    const auto hasAttr =
        [](const google::protobuf::RepeatedPtrField<dp::service::common::Attribute>& attrs,
           const std::string& key) -> bool {
            for (const auto& a : attrs)
                if (a.name() == key) return true;
            return false;
        };

    // Every captured request must contain exactly one column, and that column
    // must carry a "shardSlot" attribute in its metadata.
    int checked = 0;
    for (const auto& req : captured)
    {
        const auto& df = req.ingestiondataframe();
        for (int i = 0; i < df.doublecolumns_size(); ++i)
        {
            EXPECT_TRUE(hasAttr(df.doublecolumns(i).metadata().attributes(), "shardSlot"))
                << "doublecolumns[" << i << "] missing shardSlot attribute";
            ++checked;
        }
        for (int i = 0; i < df.floatcolumns_size(); ++i)
        {
            EXPECT_TRUE(hasAttr(df.floatcolumns(i).metadata().attributes(), "shardSlot"))
                << "floatcolumns[" << i << "] missing shardSlot attribute";
            ++checked;
        }
        for (int i = 0; i < df.int32columns_size(); ++i)
        {
            EXPECT_TRUE(hasAttr(df.int32columns(i).metadata().attributes(), "shardSlot"))
                << "int32columns[" << i << "] missing shardSlot attribute";
            ++checked;
        }
    }
    EXPECT_GT(checked, 0) << "No scalar columns found in captured requests";
}
