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

#include <chrono>
#include <filesystem>
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
