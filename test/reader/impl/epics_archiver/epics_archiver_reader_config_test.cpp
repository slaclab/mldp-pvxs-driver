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
#include <reader/impl/epics_archiver/EpicsArchiverReaderConfig.h>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using namespace mldp_pvxs_driver::reader::impl::epics_archiver;

// ============================================================================
// EpicsArchiverReaderConfig Tests
// ============================================================================

class EpicsArchiverReaderConfigTest : public ::testing::Test
{
};

// Verifies a fully populated archiver reader YAML config parses into typed fields.
TEST_F(EpicsArchiverReaderConfigTest, ValidConfigurationParsing)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        end_date: "2026-01-02T00:00:00Z"
        pvs:
          - name: "PV1"
          - name: "PV2"
          - name: "PV3"
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.name(), "test-archiver");
    EXPECT_EQ(config.hostname(), "archiver.slac.stanford.edu:11200");
    EXPECT_EQ(config.startDate(), "2026-01-01T00:00:00Z");
    ASSERT_TRUE(config.endDate().has_value());
    EXPECT_EQ(*config.endDate(), "2026-01-02T00:00:00Z");
    EXPECT_EQ(config.pvNames().size(), 3);
    EXPECT_EQ(config.pvNames()[0], "PV1");
    EXPECT_EQ(config.pvNames()[1], "PV2");
    EXPECT_EQ(config.pvNames()[2], "PV3");
    EXPECT_TRUE(config.tlsVerifyPeer());
    EXPECT_TRUE(config.tlsVerifyHost());
}

// Verifies parsing fails when the required reader name is missing.
TEST_F(EpicsArchiverReaderConfigTest, MissingNameThrows)
{
    const std::string yaml = R"(
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}

// Verifies parsing fails when the required archiver hostname is missing.
TEST_F(EpicsArchiverReaderConfigTest, MissingHostnameThrows)
{
    const std::string yaml = R"(
        name: test-archiver
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}

// Verifies parsing fails when the required PV list is missing.
TEST_F(EpicsArchiverReaderConfigTest, MissingPvsThrows)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}

// Verifies an empty PV list is accepted as a valid configuration.
TEST_F(EpicsArchiverReaderConfigTest, EmptyPvsIsValid)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs: []
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    // Empty PV list is valid (can be populated at runtime or in a later update)
    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.startDate(), "2026-01-01T00:00:00Z");
    EXPECT_FALSE(config.endDate().has_value());
    EXPECT_EQ(config.pvNames().size(), 0);
}

// Verifies each PV entry must include a name field.
TEST_F(EpicsArchiverReaderConfigTest, PvWithoutNameThrows)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - {}
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}

// Verifies an empty reader name is rejected.
TEST_F(EpicsArchiverReaderConfigTest, EmptyNameThrows)
{
    const std::string yaml = R"(
        name: ""
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}

// Verifies an empty hostname string is rejected.
TEST_F(EpicsArchiverReaderConfigTest, EmptyHostnameThrows)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: ""
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}

// Verifies the default-constructed config starts invalid until parsed.
TEST_F(EpicsArchiverReaderConfigTest, DefaultConstructor)
{
    EpicsArchiverReaderConfig config;
    EXPECT_FALSE(config.valid());
}

// Verifies a configuration with a single PV parses correctly.
TEST_F(EpicsArchiverReaderConfigTest, SinglePvConfiguration)
{
    const std::string yaml = R"(
        name: single-pv-reader
        hostname: "localhost:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "SLAC:GUNB:ELEC:LTU1:630:EPICS_PV"
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.pvNames().size(), 1);
    EXPECT_EQ(config.pvNames()[0], "SLAC:GUNB:ELEC:LTU1:630:EPICS_PV");
    EXPECT_FALSE(config.endDate().has_value());
}

// Verifies parsing fails when the required start date is missing.
TEST_F(EpicsArchiverReaderConfigTest, MissingStartDateThrows)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}

// Verifies end_date is optional and absent values are represented as nullopt.
TEST_F(EpicsArchiverReaderConfigTest, EndDateIsOptional)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.startDate(), "2026-01-01T00:00:00Z");
    EXPECT_FALSE(config.endDate().has_value());
}

// Verifies start/end date aliases in camelCase are accepted for compatibility.
TEST_F(EpicsArchiverReaderConfigTest, AcceptsCamelCaseDateKeys)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        startDate: "2026-01-01T00:00:00Z"
        endDate: "2026-01-01T12:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.startDate(), "2026-01-01T00:00:00Z");
    ASSERT_TRUE(config.endDate().has_value());
    EXPECT_EQ(*config.endDate(), "2026-01-01T12:00:00Z");
}

// Verifies TLS peer/host verification defaults remain enabled.
TEST_F(EpicsArchiverReaderConfigTest, TlsVerificationDefaultsToEnabled)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_EQ(config.batchDurationSec(), 1L);
    EXPECT_EQ(config.fetchMode(), EpicsArchiverReaderConfig::FetchMode::HistoricalOnce);
    EXPECT_TRUE(config.tlsVerifyPeer());
    EXPECT_TRUE(config.tlsVerifyHost());
}

// Verifies periodic_tail mode parses required poll interval and defaults lookback to the same value.
TEST_F(EpicsArchiverReaderConfigTest, PeriodicTailDefaultsLookbackToPollInterval)
{
    const std::string yaml = R"(
        name: archiver-tail
        hostname: "localhost:11200"
        mode: "periodic_tail"
        poll_interval_sec: 5
        pvs:
          - name: "PV1"
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.fetchMode(), EpicsArchiverReaderConfig::FetchMode::PeriodicTail);
    EXPECT_EQ(config.pollIntervalSec(), 5L);
    EXPECT_EQ(config.lookbackSec(), 5L);
}

// Verifies periodic_tail mode rejects lookback windows larger than the poll interval.
TEST_F(EpicsArchiverReaderConfigTest, PeriodicTailRejectsLookbackLargerThanPollInterval)
{
    const std::string yaml = R"(
        name: archiver-tail
        hostname: "localhost:11200"
        mode: "periodic_tail"
        poll_interval_sec: 2
        lookback_sec: 3
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}

// Verifies batch_duration_sec is parsed when explicitly configured.
TEST_F(EpicsArchiverReaderConfigTest, AcceptsBatchDurationSec)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        batch_duration_sec: 3
        pvs:
          - name: "PV1"
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.batchDurationSec(), 3L);
}

// Verifies batch_duration_sec rejects zero and negative values.
TEST_F(EpicsArchiverReaderConfigTest, RejectsNonPositiveBatchDurationSec)
{
    const std::string yaml_zero = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        batch_duration_sec: 0
        pvs:
          - name: "PV1"
    )";

    auto cfg_zero = makeConfigFromYaml(yaml_zero);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg_zero), EpicsArchiverReaderConfig::Error);

    const std::string yaml_negative = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        batch_duration_sec: -1
        pvs:
          - name: "PV1"
    )";

    auto cfg_negative = makeConfigFromYaml(yaml_negative);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg_negative), EpicsArchiverReaderConfig::Error);
}

// Verifies TLS verification can be disabled explicitly when both flags are false.
TEST_F(EpicsArchiverReaderConfigTest, AllowsDisablingTlsVerificationExplicitly)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        tls_verify_peer: false
        tls_verify_host: false
        pvs:
          - name: "PV1"
    )";

    auto                      cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_FALSE(config.tlsVerifyPeer());
    EXPECT_FALSE(config.tlsVerifyHost());
}

// Verifies hostname verification cannot be enabled while peer verification is disabled.
TEST_F(EpicsArchiverReaderConfigTest, RejectsHostVerificationWithoutPeerVerification)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        tls_verify_peer: false
        tls_verify_host: true
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(EpicsArchiverReaderConfig config(cfg), EpicsArchiverReaderConfig::Error);
}
