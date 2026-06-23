#include <gtest/gtest.h>

#include <config/ConfigSource.h>

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

} // namespace mldp_pvxs_driver::config
