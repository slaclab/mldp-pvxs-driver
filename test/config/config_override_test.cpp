#include <gtest/gtest.h>

#include <config/ConfigOverride.h>
#include <config/validate.h>

#include "test_config_helpers.h"

namespace mldp_pvxs_driver::config {

TEST(ConfigOverrideTest, ParseRejectsMissingEquals)
{
    EXPECT_THROW(parseConfigOverride("metrics.endpoint"), ConfigOverrideError);
}

TEST(ConfigOverrideTest, ParseAcceptsEmptyValue)
{
    const auto overrideSpec = parseConfigOverride("name=");
    EXPECT_EQ(overrideSpec.path, "name");
    EXPECT_TRUE(overrideSpec.value.empty());
}

TEST(ConfigOverrideTest, OverridesExistingScalarValue)
{
    auto cfg = makeConfigFromYaml(R"(
metrics:
  endpoint: 127.0.0.1:9464
)");

    applyConfigOverrides(cfg, {"metrics.endpoint=0.0.0.0:9464"});

    ASSERT_TRUE(cfg.hasChild("metrics"));
    const auto metrics = cfg.subConfig("metrics");
    ASSERT_EQ(metrics.size(), 1u);
    EXPECT_EQ(metrics.front().get("endpoint"), "0.0.0.0:9464");
}

TEST(ConfigOverrideTest, CreatesMissingFinalScalarFieldWhenParentExists)
{
    auto cfg = makeConfigFromYaml(R"(
reader:
  hdf5-bsas-gen1:
    - name: bsas_reader
)");

    applyConfigOverrides(cfg, {"reader.hdf5-bsas-gen1[0].file-path=/tmp/new-file.h5"});

    const auto readerRoot = cfg.subConfig("reader");
    ASSERT_EQ(readerRoot.size(), 1u);
    const auto hdf5Readers = readerRoot.front().subConfig("hdf5-bsas-gen1");
    ASSERT_EQ(hdf5Readers.size(), 1u);
    EXPECT_EQ(hdf5Readers.front().get("file-path"), "/tmp/new-file.h5");
}

TEST(ConfigOverrideTest, CreatesMissingIntermediatePathAsNestedMaps)
{
    auto cfg = makeConfigFromYaml(R"(
metrics:
  endpoint: 127.0.0.1:9464
)");

    applyConfigOverrides(cfg, {"metrics.extra.endpoint=0.0.0.0:9464"});

    const auto metrics = cfg.subConfig("metrics");
    ASSERT_EQ(metrics.size(), 1u);
    const auto extra = metrics.front().subConfig("extra");
    ASSERT_EQ(extra.size(), 1u);
    EXPECT_EQ(extra.front().get("endpoint"), "0.0.0.0:9464");
}

TEST(ConfigOverrideTest, RejectsOutOfRangeSequenceIndex)
{
    auto cfg = makeConfigFromYaml(R"(
reader:
  hdf5-bsas-gen1:
    - name: bsas_reader
)");

    EXPECT_THROW(
        applyConfigOverrides(cfg, {"reader.hdf5-bsas-gen1[2].file-path=/tmp/new-file.h5"}),
        ConfigOverrideError);
}

TEST(ConfigOverrideTest, LastOverrideWins)
{
    auto cfg = makeConfigFromYaml(R"(
metrics:
  endpoint: 127.0.0.1:9464
)");

    applyConfigOverrides(cfg,
                         {
                             "metrics.endpoint=0.0.0.0:9464",
                             "metrics.endpoint=localhost:9000",
                         });

    const auto metrics = cfg.subConfig("metrics");
    ASSERT_EQ(metrics.size(), 1u);
    EXPECT_EQ(metrics.front().get("endpoint"), "localhost:9000");
}

TEST(ConfigOverrideTest, ImplicitlyDescendsThroughSingleSequenceEntry)
{
    auto cfg = makeConfigFromYaml(R"(
reader:
  hdf5-bsas-gen1:
    - name: bsas_reader
)");

    applyConfigOverrides(cfg, {"reader.hdf5-bsas-gen1.file-path=/tmp/no-index.h5"});

    const auto readerRoot = cfg.subConfig("reader");
    ASSERT_EQ(readerRoot.size(), 1u);
    const auto hdf5Readers = readerRoot.front().subConfig("hdf5-bsas-gen1");
    ASSERT_EQ(hdf5Readers.size(), 1u);
    EXPECT_EQ(hdf5Readers.front().get("file-path"), "/tmp/no-index.h5");
}

TEST(ConfigOverrideTest, RejectsImplicitSequenceTraversalWhenMultipleEntriesExist)
{
    auto cfg = makeConfigFromYaml(R"(
reader:
  hdf5-bsas-gen1:
    - name: reader_a
    - name: reader_b
)");

    EXPECT_THROW(
        applyConfigOverrides(cfg, {"reader.hdf5-bsas-gen1.file-path=/tmp/ambiguous.h5"}),
        ConfigOverrideError);
}

#ifdef MLDP_PVXS_HDF5_ENABLED
TEST(ConfigOverrideTest, OverrideEnablesHdf5BsasValidation)
{
    auto cfg = makeConfigFromYaml(R"(
writer:
  mldp:
    - name: mldp_main
      mldp-pool:
        provider-name: test_provider
        ingestion-url: dp-ingestion:50051
        query-url: dp-query:50052
        min-conn: 1
        max-conn: 1
reader:
  hdf5-bsas-gen1:
    - name: bsas_reader
)");

    auto before = validateConfig(cfg);
    bool missingFilePath = false;
    for (const auto& diag : before)
    {
        if (diag.severity == ConfigDiagnostic::Severity::ERROR &&
            diag.field_path == "reader.hdf5-bsas-gen1[0].file-path")
        {
            missingFilePath = true;
        }
    }
    EXPECT_TRUE(missingFilePath);

    applyConfigOverrides(cfg, {"reader.hdf5-bsas-gen1.file-path=/tmp/bsas.h5"});

    auto after = validateConfig(cfg);
    for (const auto& diag : after)
    {
        EXPECT_FALSE(
            diag.severity == ConfigDiagnostic::Severity::ERROR &&
            diag.field_path == "reader.hdf5-bsas-gen1[0].file-path");
    }
}
#endif

} // namespace mldp_pvxs_driver::config
