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

#include <reader/impl/epics_ds/EpicsDSMetadataReader.h>
#include <util/bus/IDataBus.h>

#include "../../../config/test_config_helpers.h"
#include "../../../mock/MockDataBus.h"
#include "../../../mock/MockDSServer.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics_ds::EpicsDSMetadataReader;
using mldp_pvxs_driver::test::mock::MockDataBus;
using mldp_pvxs_driver::test::mock::MockDSServer;
using mldp_pvxs_driver::util::bus::IDataBus;
using mldp_pvxs_driver::util::bus::SourceMetadataPayload;
using mldp_pvxs_driver::util::bus::asSourceMetadata;
using mldp_pvxs_driver::util::bus::isSourceMetadata;

// ============================================================================
// Helper
// ============================================================================

static bool waitForSnapshot(const std::shared_ptr<MockDataBus>& bus,
                            size_t                              count,
                            std::chrono::milliseconds           timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (bus->snapshot().size() >= count)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return bus->snapshot().size() >= count;
}

// ============================================================================
// Fixture
// ============================================================================

class EpicsDSMetadataReaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mockServer = std::make_unique<MockDSServer>("test:ds:unit");
        bus        = std::make_shared<MockDataBus>();

        auto cfg = makeConfigFromYaml(R"yaml(
name: test-ds-reader
service: test:ds:unit
query: "%"
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.0
)yaml");

        reader = std::make_unique<EpicsDSMetadataReader>(bus, nullptr, cfg);

        ASSERT_TRUE(waitForSnapshot(bus, 1, std::chrono::milliseconds(5000)));

        snapshot = bus->snapshot();
        ASSERT_EQ(snapshot.size(), 1u);

        payload = &asSourceMetadata(snapshot[0]);
    }

    std::unique_ptr<MockDSServer>          mockServer;
    std::shared_ptr<MockDataBus>           bus;
    std::unique_ptr<EpicsDSMetadataReader> reader;
    std::vector<IDataBus::EventBatch>      snapshot;
    const SourceMetadataPayload*           payload{nullptr};
};

// ============================================================================
// Tests
// ============================================================================

// Verifies the reader pushes exactly one batch in single-shot mode and the
// payload contains all 30 rows from the built-in MockDSServer dataset.
TEST_F(EpicsDSMetadataReaderTest, SingleShotPushesSingleBatch)
{
    EXPECT_EQ(snapshot.size(), 1u);
    ASSERT_TRUE(isSourceMetadata(snapshot[0]));
    EXPECT_EQ(payload->size(), 30u);
}

// Verifies that attribute values for a known row match the built-in dataset.
TEST_F(EpicsDSMetadataReaderTest, PayloadContainsCorrectAttributes)
{
    ASSERT_TRUE(payload->count("VPIO:IN20:111:PRES") > 0);
    const auto& entry = payload->at("VPIO:IN20:111:PRES");
    EXPECT_EQ(entry.attributes.at("hostName"), "cpu-li20-vac1");
    EXPECT_EQ(entry.attributes.at("owner"), "vacuum");
}

// Verifies that the tags column is split into a vector<string> and contains
// the expected tags for a known row.
TEST_F(EpicsDSMetadataReaderTest, TagsColumnParsedCorrectly)
{
    ASSERT_TRUE(payload->count("BPMS:IN20:221:X") > 0);
    const auto& entry = payload->at("BPMS:IN20:221:X");
    ASSERT_TRUE(entry.tags.has_value());

    const auto& tags = entry.tags.value();
    EXPECT_NE(std::find(tags.begin(), tags.end(), "physics"), tags.end());
    EXPECT_NE(std::find(tags.begin(), tags.end(), "bpm"), tags.end());
}

// Verifies that the source-name column is not duplicated inside attributes.
TEST_F(EpicsDSMetadataReaderTest, ChannelNameNotInAttributes)
{
    ASSERT_TRUE(payload->count("BPMS:IN20:221:X") > 0);
    const auto& entry = payload->at("BPMS:IN20:221:X");
    EXPECT_EQ(entry.attributes.count("channelName"), 0u);
}

// Verifies that the tags column is not duplicated inside attributes.
TEST_F(EpicsDSMetadataReaderTest, TagsColumnNotInAttributes)
{
    ASSERT_TRUE(payload->count("BPMS:IN20:221:X") > 0);
    const auto& entry = payload->at("BPMS:IN20:221:X");
    EXPECT_EQ(entry.attributes.count("tags"), 0u);
}
