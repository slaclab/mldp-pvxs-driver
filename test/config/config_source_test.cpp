#include <gtest/gtest.h>

#include <config/ConfigSource.h>

#include "test_config_helpers.h"

#include <filesystem>
#include <fstream>

namespace mldp_pvxs_driver::config {

TEST(ConfigSourceTest, LoadsDottedAssignmentWhenSourceIsNotAFile)
{
    const auto cfg = loadMergedConfigSources({"metrics.endpoint=0.0.0.0:9464"});

    ASSERT_TRUE(cfg.hasChild("metrics"));
    const auto metrics = cfg.subConfig("metrics");
    ASSERT_EQ(metrics.size(), 1u);
    EXPECT_EQ(metrics.front().get("endpoint"), "0.0.0.0:9464");
}

TEST(ConfigSourceTest, MergesFileAndDottedAssignmentsInOrder)
{
    const auto tempPath = std::filesystem::temp_directory_path() / "mldp_config_source_test.yaml";
    {
        std::ofstream out(tempPath);
        out << "metrics:\n"
               "  endpoint: 127.0.0.1:9464\n"
               "writer:\n"
               "  mldp:\n"
               "    - name: mldp_main\n";
    }

    const auto cfg = loadMergedConfigSources(
        {
            tempPath.string(),
            "metrics.endpoint=0.0.0.0:9464",
            "reader.hdf5-bsas-gen1.name=bsas_reader",
        });

    const auto metrics = cfg.subConfig("metrics");
    ASSERT_EQ(metrics.size(), 1u);
    EXPECT_EQ(metrics.front().get("endpoint"), "0.0.0.0:9464");

    const auto writer = cfg.subConfig("writer");
    ASSERT_EQ(writer.size(), 1u);
    EXPECT_TRUE(writer.front().hasChild("mldp"));

    const auto reader = cfg.subConfig("reader");
    ASSERT_EQ(reader.size(), 1u);
    EXPECT_TRUE(reader.front().hasChild("hdf5-bsas-gen1"));

    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST(ConfigSourceTest, MultipleDottedAssignmentsAccumulateIntoOneConfig)
{
    const auto cfg = loadMergedConfigSources(
        {
            "reader.hdf5-bsas-gen1[0].name=first",
            "reader.hdf5-bsas-gen1[0].file-path=/tmp/second.h5",
        });

    const auto reader = cfg.subConfig("reader");
    ASSERT_EQ(reader.size(), 1u);
    const auto items = reader.front().subConfig("hdf5-bsas-gen1");
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items.front().get("name"), "first");
    EXPECT_EQ(items.front().get("file-path"), "/tmp/second.h5");
}

TEST(ConfigSourceTest, RejectsNonFileWhenNotValidDottedAssignment)
{
    EXPECT_THROW(loadMergedConfigSources({"metrics: { endpoint: 0.0.0.0:9464 }"}), std::runtime_error);
}

TEST(ConfigSourceTest, RejectsExistingDirectoryAsConfigSource)
{
    const auto tempDir = std::filesystem::temp_directory_path() / "mldp_config_source_dir_test";
    std::filesystem::create_directories(tempDir);

    EXPECT_THROW(loadMergedConfigSources({tempDir.string()}), std::runtime_error);

    std::error_code ec;
    std::filesystem::remove(tempDir, ec);
}

} // namespace mldp_pvxs_driver::config

TEST(ConfigNonMapNodeTest, LookupsOnNullOrScalarNodeReturnDefaults)
{
    const auto root = mldp_pvxs_driver::config::makeConfigFromYaml(
        "empty:\nscalar: 5\nlist:\n  - a\n");
    for (const char* key : {"empty", "scalar", "list"})
    {
        const auto subs = root.subConfig(key);
        ASSERT_FALSE(subs.empty()) << key;
        const auto& node = subs.front();
        EXPECT_FALSE(node.hasChild("x")) << key;
        EXPECT_EQ(node.get("x", "def"), "def") << key;
        EXPECT_EQ(node.getInt("x", 7), 7) << key;
        EXPECT_TRUE(node.getBool("x", true)) << key;
        EXPECT_DOUBLE_EQ(node.getDouble("x", 1.5), 1.5) << key;
        EXPECT_TRUE(node.subConfig("x").empty()) << key;
        EXPECT_FALSE(node.isSequence("x")) << key;
    }
}
