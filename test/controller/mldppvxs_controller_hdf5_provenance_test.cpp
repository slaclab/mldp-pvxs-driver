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

#include "../common/MldpQueryTestUtils.h"
#include "../config/test_config_helpers.h"
#include "../mock/BsasGen1HDF5Mock.h"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
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

std::string buildShardSlotYaml(const fs::path&    hdf5Path,
                               const std::string& readerName,
                               const std::string& dbPath,
                               int                numShards)
{
    std::ostringstream yaml;
    yaml << "enrichers:\n"
         << "  sharding:\n"
         << "    type: shard-slot\n"
         << "    num-shards: " << numShards << "\n"
         << "    db-path: " << dbPath << "\n"
         << "writer:\n"
         << "  mldp:\n"
         << "    - name: mldp_shard\n"
         << "      enrichers: [sharding]\n"
         << "      mldp-pool:\n"
         << "        provider-name: shard-test-provider\n"
         << "        ingestion-url: dp-ingestion:50051\n"
         << "        query-url: dp-query:50052\n"
         << "        min-conn: 1\n"
         << "        max-conn: 1\n"
         << "reader:\n"
         << "  hdf5-bsas-gen1:\n"
         << "    - name: " << readerName << "\n"
         << "      file-path: " << hdf5Path.string() << "\n"
         << "      chunk-size: 1000\n"
         << "      use-label-as-name: false\n";
    return yaml.str();
}

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
// shardSlot end-to-end via real MLDP services:
// HDF5BsasGen1Reader → shard-slot enricher → MLDPWriter (dp-ingestion:50051)
// Results verified by querying back from dp-query:50052.
// ---------------------------------------------------------------------------

TEST_F(ControllerHDF5ProvenanceTest, ShardSlotAppearsInStoredColumnAttributes)
{
    const std::string readerName  = makeUniquePrefix("shard_reader_");
    const std::string floatPrefix = makeUniquePrefix("SHARD_FLOAT_");
    const std::string intPrefix   = makeUniquePrefix("SHARD_INT_");
    const std::string dbPath      = (tempDir_ / "shard_slot.db").string();

    // 3 float + 2 int = 5 columns, all with unique names.
    BsasGen1HDF5Mock::Params params;
    params.numFloatCols  = 3;
    params.numIntCols    = 2;
    params.numRows       = 5;
    params.baseEpoch     = static_cast<uint32_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) - 5);
    params.floatColPrefix = floatPrefix;
    params.intColPrefix   = intPrefix;
    BsasGen1HDF5Mock::generate(mockFile_.string(), params);

    // Build list of expected PV names (must match what the reader emits).
    std::vector<std::string> pvNames;
    for (std::size_t i = 0; i < params.numFloatCols; ++i)
    {
        std::ostringstream oss;
        oss << floatPrefix << std::setfill('0') << std::setw(4) << i;
        pvNames.push_back(oss.str());
    }
    for (std::size_t i = 0; i < params.numIntCols; ++i)
    {
        std::ostringstream oss;
        oss << intPrefix << std::setfill('0') << std::setw(2) << i;
        pvNames.push_back(oss.str());
    }

    const auto config = makeConfigFromYaml(
        buildShardSlotYaml(mockFile_, readerName, dbPath, 6));
    ASSERT_TRUE(config.valid());

    controller_ = mldp_pvxs_driver::controller::MLDPPVXSController::create(config);
    ASSERT_TRUE(controller_);
    controller_->start();
    std::this_thread::sleep_for(kStartupDelay);

    const auto columns = queryAndCollectColumns(pvNames, kQueryTimeout, kLookbackWindow);
    ASSERT_TRUE(columns.has_value()) << "Failed to query columns from dp-query:50052";

    controller_->stop();
    controller_.reset();

    // Every returned bucket must carry a "shardSlot" attribute.
    for (const auto& pvName : pvNames)
    {
        const auto it = columns->find(pvName);
        if (it == columns->end()) continue;
        for (const auto& bucket : it->second)
        {
            const auto& attrs = metadataFromValues(bucket).attributes();
            EXPECT_TRUE(hasAttribute(attrs, "shardSlot", "") || [&] {
                for (const auto& a : attrs)
                    if (a.name() == "shardSlot") return true;
                return false;
            }()) << "Column '" << pvName << "' missing shardSlot attribute";
        }
    }
}

// ---------------------------------------------------------------------------
// 6-shard distribution + SQLite persistence:
// 12 columns (6 float + 6 int) → num-shards=6 → all 6 buckets hit.
// Slot in DB must match slot stored by dp-query.
// ---------------------------------------------------------------------------
TEST_F(ControllerHDF5ProvenanceTest, ShardSlotDistributedAcross6ShardsAndPersistedToDb)
{
    const std::string readerName  = makeUniquePrefix("shard6_reader_");
    const std::string floatPrefix = makeUniquePrefix("SHARD6_FLOAT_");
    const std::string intPrefix   = makeUniquePrefix("SHARD6_INT_");
    const std::string dbPath      = (tempDir_ / "shard6_test.db").string();

    // 6 float + 6 int = 12 columns; round-robin across 6 shards → 2 per bucket.
    BsasGen1HDF5Mock::Params params;
    params.numFloatCols   = 6;
    params.numIntCols     = 6;
    params.numRows        = 5;
    params.baseEpoch      = static_cast<uint32_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) - 5);
    params.floatColPrefix = floatPrefix;
    params.intColPrefix   = intPrefix;
    BsasGen1HDF5Mock::generate(mockFile_.string(), params);

    std::vector<std::string> pvNames;
    for (std::size_t i = 0; i < params.numFloatCols; ++i)
    {
        std::ostringstream oss;
        oss << floatPrefix << std::setfill('0') << std::setw(4) << i;
        pvNames.push_back(oss.str());
    }
    for (std::size_t i = 0; i < params.numIntCols; ++i)
    {
        std::ostringstream oss;
        oss << intPrefix << std::setfill('0') << std::setw(2) << i;
        pvNames.push_back(oss.str());
    }

    const auto config = makeConfigFromYaml(
        buildShardSlotYaml(mockFile_, readerName, dbPath, 6));
    ASSERT_TRUE(config.valid());

    controller_ = mldp_pvxs_driver::controller::MLDPPVXSController::create(config);
    ASSERT_TRUE(controller_);
    controller_->start();
    std::this_thread::sleep_for(kStartupDelay);

    const auto columns = queryAndCollectColumns(pvNames, kQueryTimeout, kLookbackWindow);
    ASSERT_TRUE(columns.has_value()) << "Failed to query columns from dp-query:50052";

    controller_->stop();
    controller_.reset();

    // A. Collect (column_name → shardSlot string) from returned buckets.
    std::unordered_map<std::string, std::string> querySlots;
    for (const auto& pvName : pvNames)
    {
        const auto it = columns->find(pvName);
        if (it == columns->end()) continue;
        for (const auto& bucket : it->second)
        {
            for (const auto& attr : metadataFromValues(bucket).attributes())
            {
                if (attr.name() == "shardSlot")
                {
                    querySlots[pvName] = attr.value();
                    break;
                }
            }
        }
    }
    ASSERT_FALSE(querySlots.empty()) << "No shardSlot attributes found in query results";

    for (const auto& [col, slot] : querySlots)
        EXPECT_FALSE(slot.empty()) << "Column '" << col << "' has empty shardSlot";

    // B. All 6 shard buckets must be represented.
    constexpr int kNumShards = 6;
    std::array<bool, kNumShards> bucket_hit{};
    bucket_hit.fill(false);
    for (const auto& [col, slot_str] : querySlots)
    {
        if (slot_str.empty()) continue;
        const auto slot_val = static_cast<uint32_t>(std::stoul(slot_str));
        for (int k = 0; k < kNumShards; ++k)
        {
            const auto lower = (65536u * static_cast<uint32_t>(k)) / kNumShards;
            const auto upper = (65536u * static_cast<uint32_t>(k + 1)) / kNumShards - 1;
            if (slot_val >= lower && slot_val <= upper)
            {
                bucket_hit[static_cast<std::size_t>(k)] = true;
                break;
            }
        }
    }
    for (int k = 0; k < kNumShards; ++k)
        EXPECT_TRUE(bucket_hit[static_cast<std::size_t>(k)])
            << "Shard bucket " << k << " has no column assigned";

    // C. SQLite DB slot must match the slot returned by dp-query.
    //    Controller (and enricher instance) is already stopped — DB file is closed.
    sqlite3* db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr))
        << "Cannot open shard DB: " << dbPath;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT source_name, slot FROM shard_slots", -1, &stmt, nullptr);
    int db_row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        ++db_row_count;
        const std::string db_col  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto        db_slot = static_cast<uint32_t>(sqlite3_column_int(stmt, 1));

        const auto it = querySlots.find(db_col);
        if (it == querySlots.end() || it->second.empty())
            continue;

        const auto query_slot = static_cast<uint32_t>(std::stoul(it->second));
        EXPECT_EQ(db_slot, query_slot)
            << "DB slot " << db_slot << " != query slot " << query_slot
            << " for column '" << db_col << "'";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    EXPECT_GT(db_row_count, 0) << "No rows found in shard DB";
}
