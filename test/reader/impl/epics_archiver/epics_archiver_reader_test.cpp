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

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../../config/test_config_helpers.h"

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics_archiver/EpicsArchiverReader.h>
#include <reader/impl/epics_archiver/EpicsArchiverReaderConfig.h>
#include <util/bus/IEventBusPush.h>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using namespace mldp_pvxs_driver::reader::impl::epics_archiver;

// Concrete mock implementation of IEventBusPush for testing
class MockEventBusPush : public mldp_pvxs_driver::util::bus::IEventBusPush
{
public:
    using EventBatch = mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch;

    explicit MockEventBusPush(std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics = nullptr)
        : metrics_(std::move(metrics))
    {
    }

    // Store received events for verification
    std::vector<EventBatch> received_events;
    mutable std::mutex      mutex;

    bool push(EventBatch batch) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        received_events.push_back(std::move(batch));
        return true;
    }

    size_t event_count() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return received_events.size();
    }

private:
    std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics_;
};

// ============================================================================
// EpicsArchiverReaderConfig Tests
// ============================================================================

class EpicsArchiverReaderConfigTest : public ::testing::Test
{
};

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

    auto cfg = makeConfigFromYaml(yaml);
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
}

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

TEST_F(EpicsArchiverReaderConfigTest, EmptyPvsIsValid)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs: []
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    // Empty PV list is valid (can be populated at runtime or in a later update)
    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.startDate(), "2026-01-01T00:00:00Z");
    EXPECT_FALSE(config.endDate().has_value());
    EXPECT_EQ(config.pvNames().size(), 0);
}

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

TEST_F(EpicsArchiverReaderConfigTest, DefaultConstructor)
{
    EpicsArchiverReaderConfig config;
    EXPECT_FALSE(config.valid());
}

TEST_F(EpicsArchiverReaderConfigTest, SinglePvConfiguration)
{
    const std::string yaml = R"(
        name: single-pv-reader
        hostname: "localhost:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "SLAC:GUNB:ELEC:LTU1:630:EPICS_PV"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.pvNames().size(), 1);
    EXPECT_EQ(config.pvNames()[0], "SLAC:GUNB:ELEC:LTU1:630:EPICS_PV");
    EXPECT_FALSE(config.endDate().has_value());
}

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

TEST_F(EpicsArchiverReaderConfigTest, EndDateIsOptional)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.startDate(), "2026-01-01T00:00:00Z");
    EXPECT_FALSE(config.endDate().has_value());
}

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

    auto cfg = makeConfigFromYaml(yaml);
    EpicsArchiverReaderConfig config(cfg);

    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.startDate(), "2026-01-01T00:00:00Z");
    ASSERT_TRUE(config.endDate().has_value());
    EXPECT_EQ(*config.endDate(), "2026-01-01T12:00:00Z");
}

// ============================================================================
// EpicsArchiverReader Tests
// ============================================================================

class EpicsArchiverReaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus_ = std::make_shared<MockEventBusPush>();
    }

    std::shared_ptr<MockEventBusPush> bus_;
};

TEST_F(EpicsArchiverReaderTest, ReaderInstantiation)
{
    const std::string yaml = R"(
        name: test-archiver
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
          - name: "PV2"
    )";

    auto cfg = makeConfigFromYaml(yaml);

    // Should not throw
    auto reader = std::make_unique<EpicsArchiverReader>(bus_, nullptr, cfg);

    EXPECT_EQ(reader->name(), "test-archiver");
}

TEST_F(EpicsArchiverReaderTest, InvalidConfigurationThrows)
{
    const std::string yaml = R"(
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);

    // Should throw because name is missing
    EXPECT_THROW(
        auto reader = std::make_unique<EpicsArchiverReader>(bus_, nullptr, cfg),
        EpicsArchiverReaderConfig::Error);
}

TEST_F(EpicsArchiverReaderTest, ReaderNameAccessor)
{
    const std::string yaml = R"(
        name: my-archiver-reader
        hostname: "archiver.example.com:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus_, nullptr, cfg);

    EXPECT_EQ(reader->name(), "my-archiver-reader");
}

TEST_F(EpicsArchiverReaderTest, MultipleReaderInstances)
{
    const std::string yaml1 = R"(
        name: archiver1
        hostname: "archiver1.example.com:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    const std::string yaml2 = R"(
        name: archiver2
        hostname: "archiver2.example.com:11200"
        start_date: "2026-01-02T00:00:00Z"
        pvs:
          - name: "PV2"
    )";

    auto cfg1 = makeConfigFromYaml(yaml1);
    auto cfg2 = makeConfigFromYaml(yaml2);

    auto reader1 = std::make_unique<EpicsArchiverReader>(bus_, nullptr, cfg1);
    auto reader2 = std::make_unique<EpicsArchiverReader>(bus_, nullptr, cfg2);

    EXPECT_EQ(reader1->name(), "archiver1");
    EXPECT_EQ(reader2->name(), "archiver2");
}

TEST_F(EpicsArchiverReaderTest, LargeNumberOfPVs)
{
    // Create a YAML config with many PVs
    std::string yaml = R"(name: multi-pv-reader
hostname: "archiver.slac.stanford.edu:11200"
start_date: "2026-01-01T00:00:00Z"
pvs:
)";

    for (int i = 0; i < 50; ++i)
    {
        yaml += "  - name: \"PV_" + std::to_string(i) + "\"\n";
    }

    auto cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus_, nullptr, cfg);

    EXPECT_EQ(reader->name(), "multi-pv-reader");
}

TEST_F(EpicsArchiverReaderTest, ReaderWithMetrics)
{
    const std::string yaml = R"(
        name: test-archiver-with-metrics
        hostname: "archiver.slac.stanford.edu:11200"
        start_date: "2026-01-01T00:00:00Z"
        pvs:
          - name: "PV1"
    )";

    auto cfg = makeConfigFromYaml(yaml);
    // Metrics requires a MetricsConfig, so we pass nullptr for this test
    auto reader = std::make_unique<EpicsArchiverReader>(bus_, nullptr, cfg);

    EXPECT_EQ(reader->name(), "test-archiver-with-metrics");
}
