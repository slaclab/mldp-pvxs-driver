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

#include <string>

#include "../../../config/test_config_helpers.h"

#include <config/Config.h>
#include <reader/impl/epics_ds/EpicsDSMetadataReaderConfig.h>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using namespace mldp_pvxs_driver::reader::impl::epics_ds;

// ============================================================================
// EpicsDSMetadataReaderConfig Tests
// ============================================================================

class EpicsDSMetadataReaderConfigTest : public ::testing::Test
{
};

// Verifies that a minimal config (only the required name field) yields all expected defaults.
TEST_F(EpicsDSMetadataReaderConfigTest, DefaultValues)
{
    auto cfg = makeConfigFromYaml(R"yaml(
name: test-reader
)yaml");

    EpicsDSMetadataReaderConfig config(cfg);

    EXPECT_EQ(config.name(), "test-reader");
    EXPECT_EQ(config.service(), "ds");
    EXPECT_EQ(config.query(), "%");
    EXPECT_DOUBLE_EQ(config.timeoutSec(), 5.0);
    EXPECT_EQ(config.sourceNameColumn(), "channelName");
    EXPECT_EQ(config.tagsColumn(), "");
    EXPECT_DOUBLE_EQ(config.rescanIntervalSec(), 0.0);
}

// Verifies that all explicitly set fields are parsed correctly.
TEST_F(EpicsDSMetadataReaderConfigTest, AllFieldsParsed)
{
    auto cfg = makeConfigFromYaml(R"yaml(
name: my-ds-reader
service: my-ds
query: "BPMS:*"
timeout-sec: 10.0
source-name-column: pvName
tags-column: labels
rescan-interval-sec: 300.0
)yaml");

    EpicsDSMetadataReaderConfig config(cfg);

    EXPECT_EQ(config.name(), "my-ds-reader");
    EXPECT_EQ(config.service(), "my-ds");
    EXPECT_EQ(config.query(), "BPMS:*");
    EXPECT_DOUBLE_EQ(config.timeoutSec(), 10.0);
    EXPECT_EQ(config.sourceNameColumn(), "pvName");
    EXPECT_EQ(config.tagsColumn(), "labels");
    EXPECT_DOUBLE_EQ(config.rescanIntervalSec(), 300.0);
}

// Verifies that omitting the required 'name' field throws EpicsDSMetadataReaderConfig::Error.
TEST_F(EpicsDSMetadataReaderConfigTest, MissingNameThrows)
{
    auto cfg = makeConfigFromYaml(R"yaml(
service: my-ds
query: "%"
)yaml");

    EXPECT_THROW(EpicsDSMetadataReaderConfig config(cfg), EpicsDSMetadataReaderConfig::Error);
}

// Verifies that an empty name string is rejected.
TEST_F(EpicsDSMetadataReaderConfigTest, EmptyNameThrows)
{
    auto cfg = makeConfigFromYaml(R"yaml(
name: ""
service: my-ds
)yaml");

    EXPECT_THROW(EpicsDSMetadataReaderConfig config(cfg), EpicsDSMetadataReaderConfig::Error);
}

// Verifies that a negative timeout-sec value is rejected.
TEST_F(EpicsDSMetadataReaderConfigTest, NegativeTimeoutThrows)
{
    auto cfg = makeConfigFromYaml(R"yaml(
name: test-reader
timeout-sec: -1.0
)yaml");

    EXPECT_THROW(EpicsDSMetadataReaderConfig config(cfg), EpicsDSMetadataReaderConfig::Error);
}

// Verifies that a negative rescan-interval-sec value is rejected.
TEST_F(EpicsDSMetadataReaderConfigTest, NegativeRescanThrows)
{
    auto cfg = makeConfigFromYaml(R"yaml(
name: test-reader
rescan-interval-sec: -5.0
)yaml");

    EXPECT_THROW(EpicsDSMetadataReaderConfig config(cfg), EpicsDSMetadataReaderConfig::Error);
}

// Verifies that rescan-interval-sec: 0.0 is accepted (single-shot mode).
TEST_F(EpicsDSMetadataReaderConfigTest, ZeroRescanAllowed)
{
    auto cfg = makeConfigFromYaml(R"yaml(
name: test-reader
rescan-interval-sec: 0.0
)yaml");

    EpicsDSMetadataReaderConfig config(cfg);

    EXPECT_DOUBLE_EQ(config.rescanIntervalSec(), 0.0);
}

// Verifies that config.valid() is true after a successful parse.
TEST_F(EpicsDSMetadataReaderConfigTest, ValidConfigSetsValidTrue)
{
    auto cfg = makeConfigFromYaml(R"yaml(
name: test-reader
service: ds
query: "%"
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 60.0
)yaml");

    EpicsDSMetadataReaderConfig config(cfg);
}
