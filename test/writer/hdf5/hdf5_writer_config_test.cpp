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

#include <writer/hdf5/HDF5WriterConfig.h>

#include "../../config/test_config_helpers.h"

using mldp_pvxs_driver::config::makeConfigFromYaml;
using namespace mldp_pvxs_driver::writer;

TEST(HDF5WriterConfigTest, ParsesAllFields)
{
    auto cfg = makeConfigFromYaml(R"(
name: my-writer
base-path: /data/hdf5
max-file-age-s: 7200
max-file-size-mb: 1024
flush-interval-ms: 500
compression-level: 4
)");

    auto result = HDF5WriterConfig::parse(cfg);

    EXPECT_EQ(result.name, "my-writer");
    EXPECT_EQ(result.basePath, "/data/hdf5");
    EXPECT_EQ(result.maxFileAge, std::chrono::seconds(7200));
    EXPECT_EQ(result.maxFileSizeMB, uint64_t(1024));
    EXPECT_EQ(result.flushInterval, std::chrono::milliseconds(500));
    EXPECT_EQ(result.compressionLevel, 4);
}

TEST(HDF5WriterConfigTest, DefaultsWhenOptionalFieldsOmitted)
{
    auto cfg = makeConfigFromYaml(R"(
name: minimal-writer
base-path: /tmp/hdf5
)");

    auto result = HDF5WriterConfig::parse(cfg);

    EXPECT_EQ(result.name, "minimal-writer");
    EXPECT_EQ(result.basePath, "/tmp/hdf5");
    EXPECT_EQ(result.maxFileAge, std::chrono::seconds(3600));
    EXPECT_EQ(result.maxFileSizeMB, uint64_t(512));
    EXPECT_EQ(result.flushInterval, std::chrono::milliseconds(1000));
    EXPECT_EQ(result.compressionLevel, 0);
}

TEST(HDF5WriterConfigTest, ThrowsOnMissingName)
{
    auto cfg = makeConfigFromYaml(R"(
base-path: /data/hdf5
max-file-age-s: 3600
)");

    EXPECT_THROW(HDF5WriterConfig::parse(cfg), HDF5WriterConfig::Error);
}

TEST(HDF5WriterConfigTest, ThrowsOnMissingBasePath)
{
    auto cfg = makeConfigFromYaml(R"(
name: my-writer
max-file-age-s: 3600
)");

    EXPECT_THROW(HDF5WriterConfig::parse(cfg), HDF5WriterConfig::Error);
}

TEST(HDF5WriterConfigTest, ParsesCompressionLevel)
{
    auto cfg = makeConfigFromYaml(R"(
name: compressed-writer
base-path: /data/compressed
compression-level: 6
)");

    auto result = HDF5WriterConfig::parse(cfg);

    EXPECT_EQ(result.compressionLevel, 6);
}
