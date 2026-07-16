//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////
#ifdef BUILD_PYTHON_PROCESSOR

#include <gtest/gtest.h>

#include <enricher/PythonEnricher.h>
#include <enricher/detail/PythonEnricherTestHooks.h>

#include "config/test_config_helpers.h"

#include <filesystem>
#include <fstream>

namespace mldp_pvxs_driver::enricher {
namespace {

using config::makeConfigFromYaml;
using util::bus::ConfigurationActivationPayload;
using util::bus::ConfigurationPayload;
using util::bus::DataBatch;
using util::bus::DataColumn;
using util::bus::EventBatchStruct;
using util::bus::SourceMetadataPayload;
using util::bus::TimeSeriesPayload;

class PythonEnricherTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        script_path_ = std::filesystem::temp_directory_path() / "mldp_python_enricher_payload_test.py";
        std::ofstream script(script_path_);
        script << "def enrich(batch):\n"
                  "    return {'metadata': {'seen_payload_type': batch['payload_type']}}\n";
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(script_path_, error);
    }

    std::filesystem::path script_path_;
};

TEST_F(PythonEnricherTest, ReceivesAndUpdatesEveryPayloadType)
{
    PythonEnricher enricher(makeConfigFromYaml("script-path: " + script_path_.string()));
    std::vector<std::pair<EventBatchStruct, std::string>> batches;
    DataColumn column{.name = "PV:ONE", .values = std::vector<double>{1.0}};
    DataBatch frame{.columns = {std::move(column)}};
    batches.emplace_back(EventBatchStruct{.payload = TimeSeriesPayload{.frames = {std::move(frame)}}}, "time-series");
    batches.emplace_back(EventBatchStruct{.payload = SourceMetadataPayload{}}, "source-metadata");
    batches.emplace_back(EventBatchStruct{.payload = ConfigurationPayload{}}, "configuration");
    batches.emplace_back(EventBatchStruct{.payload = ConfigurationActivationPayload{}}, "configuration-activation");

    for (auto& [batch, expected_type] : batches)
    {
        ASSERT_TRUE(enricher.run(batch));
        EXPECT_EQ(expected_type, batch.metadata.at("seen_payload_type"));
    }
}

TEST_F(PythonEnricherTest, NoneDropsAndInvalidReturnRejectsPayload)
{
    {
        std::ofstream script(script_path_);
        script << "def enrich(batch):\n    return None\n";
    }
    PythonEnricher dropper(makeConfigFromYaml("script-path: " + script_path_.string()));
    EventBatchStruct batch{.payload = SourceMetadataPayload{}};
    EXPECT_FALSE(dropper.run(batch));

    {
        std::ofstream script(script_path_);
        script << "def enrich(batch):\n    return ['not', 'a', 'dict']\n";
    }
    PythonEnricher invalid(makeConfigFromYaml("script-path: " + script_path_.string()));
    EXPECT_FALSE(invalid.run(batch));
}

TEST_F(PythonEnricherTest, BatchDictionaryAllocationFailureDoesNotCallPythonOrCrash)
{
    PythonEnricher enricher(makeConfigFromYaml("script-path: " + script_path_.string()));
    EventBatchStruct batch{.payload = SourceMetadataPayload{}};
    detail::failNextPythonConversion(detail::PythonConversionFailure::batchDictionary);

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const bool enriched = enricher.run(batch);
    const auto standard_error = testing::internal::GetCapturedStderr();
    const auto standard_output = testing::internal::GetCapturedStdout();

    EXPECT_FALSE(enriched);
    EXPECT_NE(std::string::npos, standard_error.find("MemoryError"));
    EXPECT_TRUE(standard_output.empty());
    EXPECT_TRUE(enricher.run(batch));
}

TEST_F(PythonEnricherTest, MetadataConversionFailureReleasesDictionary)
{
    PythonEnricher enricher(makeConfigFromYaml("script-path: " + script_path_.string()));
    EventBatchStruct batch{.metadata = {{"key", "value"}}, .payload = SourceMetadataPayload{}};
    const auto cleanup_count = detail::metadataDictionaryCleanupCount();
    detail::failNextPythonConversion(detail::PythonConversionFailure::metadataString);

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const bool enriched = enricher.run(batch);
    const auto standard_error = testing::internal::GetCapturedStderr();
    const auto standard_output = testing::internal::GetCapturedStdout();

    EXPECT_FALSE(enriched);
    EXPECT_NE(std::string::npos, standard_error.find("Python enricher failed"));
    EXPECT_TRUE(standard_output.empty());
    EXPECT_EQ(cleanup_count + 1, detail::metadataDictionaryCleanupCount());
}

} // namespace
} // namespace mldp_pvxs_driver::enricher

#endif // BUILD_PYTHON_PROCESSOR
