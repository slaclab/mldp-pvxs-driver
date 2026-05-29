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
#include <thread>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics_ds::EpicsDSMetadataReader;
using mldp_pvxs_driver::test::mock::DsRow;
using mldp_pvxs_driver::test::mock::MockDataBus;
using mldp_pvxs_driver::test::mock::MockDSServer;
using mldp_pvxs_driver::util::bus::asSourceMetadata;
using mldp_pvxs_driver::util::bus::isSourceMetadata;

namespace {

static bool waitForMinBatches(const std::shared_ptr<MockDataBus>& bus,
                               size_t                              minCount,
                               std::chrono::milliseconds           timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (bus->snapshot().size() >= minCount)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return bus->snapshot().size() >= minCount;
}

} // namespace

// TEST 1 — reader pushes at least one batch with the expected payload size and
// a spot-checked entry.
TEST(EpicsDSMetadataIntegrationTest, DsMetadataReaderPushesPayloadToBus)
{
    MockDSServer mockServer("test:ds");
    auto         bus = std::make_shared<MockDataBus>();

    const auto cfg = makeConfigFromYaml(R"(
name: test-ds-reader
service: test:ds
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.0
)");

    EpicsDSMetadataReader reader(bus, nullptr, cfg);

    ASSERT_TRUE(waitForMinBatches(bus, 1, std::chrono::milliseconds(5000)));

    const auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);

    const auto& batch = batches.front();
    ASSERT_TRUE(isSourceMetadata(batch));

    const auto& payload = asSourceMetadata(batch);
    EXPECT_EQ(payload.size(), 30u);
    ASSERT_EQ(payload.count("VPIO:IN20:111:PRES"), 1u);
    EXPECT_EQ(payload.at("VPIO:IN20:111:PRES").attributes.at("hostName"), "cpu-li20-vac1");
}

// TEST 2 — attributes map contains the correct values and excludes the
// source-name column and the tags column.
TEST(EpicsDSMetadataIntegrationTest, DsMetadataPayloadAttributesCorrect)
{
    MockDSServer mockServer("test:ds1");
    auto         bus = std::make_shared<MockDataBus>();

    const auto cfg = makeConfigFromYaml(R"(
name: test-ds-reader-1
service: test:ds1
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.0
)");

    EpicsDSMetadataReader reader(bus, nullptr, cfg);

    ASSERT_TRUE(waitForMinBatches(bus, 1, std::chrono::milliseconds(5000)));

    const auto  snapped = bus->snapshot();
    const auto& payload = asSourceMetadata(snapped.front());
    ASSERT_EQ(payload.count("BPMS:IN20:221:X"), 1u);

    const auto& entry = payload.at("BPMS:IN20:221:X");
    EXPECT_EQ(entry.attributes.at("owner"),      "diagnostics");
    EXPECT_EQ(entry.attributes.at("recordType"), "ai");
    EXPECT_EQ(entry.attributes.at("pvStatus"),   "Active");

    // Source-name column is the map key; must not also appear as an attribute.
    EXPECT_EQ(entry.attributes.count("channelName"), 0u);
    // Tags column is parsed into the tags field; must not appear as an attribute.
    EXPECT_EQ(entry.attributes.count("tags"), 0u);
}

// TEST 3 — comma-separated tags are split correctly into the tags vector.
TEST(EpicsDSMetadataIntegrationTest, DsMetadataTagsParsed)
{
    MockDSServer mockServer("test:ds-tags");
    auto         bus = std::make_shared<MockDataBus>();

    const auto cfg = makeConfigFromYaml(R"(
name: test-ds-reader-tags
service: test:ds-tags
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.0
)");

    EpicsDSMetadataReader reader(bus, nullptr, cfg);

    ASSERT_TRUE(waitForMinBatches(bus, 1, std::chrono::milliseconds(5000)));

    const auto  snapped = bus->snapshot();
    const auto& payload = asSourceMetadata(snapped.front());

    // BPMS:IN20:221:X mock tags: physics,bpm,fast,survey
    ASSERT_EQ(payload.count("BPMS:IN20:221:X"), 1u);
    const auto& entryX = payload.at("BPMS:IN20:221:X");
    ASSERT_TRUE(entryX.tags.has_value());
    const auto& tagsX = entryX.tags.value();
    EXPECT_NE(std::find(tagsX.begin(), tagsX.end(), "physics"), tagsX.end());
    EXPECT_NE(std::find(tagsX.begin(), tagsX.end(), "bpm"),     tagsX.end());
    EXPECT_NE(std::find(tagsX.begin(), tagsX.end(), "fast"),    tagsX.end());
    EXPECT_NE(std::find(tagsX.begin(), tagsX.end(), "survey"),  tagsX.end());

    // BPMS:IN20:221:TMIT mock tags: physics,bpm,fast  (no survey)
    ASSERT_EQ(payload.count("BPMS:IN20:221:TMIT"), 1u);
    const auto& entryTmit = payload.at("BPMS:IN20:221:TMIT");
    ASSERT_TRUE(entryTmit.tags.has_value());
    const auto& tagsTmit = entryTmit.tags.value();
    EXPECT_NE(std::find(tagsTmit.begin(), tagsTmit.end(), "physics"), tagsTmit.end());
    EXPECT_NE(std::find(tagsTmit.begin(), tagsTmit.end(), "bpm"),     tagsTmit.end());
    EXPECT_NE(std::find(tagsTmit.begin(), tagsTmit.end(), "fast"),    tagsTmit.end());
    EXPECT_EQ(std::find(tagsTmit.begin(), tagsTmit.end(), "survey"),  tagsTmit.end());
}

// TEST 4 — periodic re-fetch produces multiple batches each carrying the full
// 30-row dataset.
TEST(EpicsDSMetadataIntegrationTest, DsMetadataRescanPeriodicRefetch)
{
    MockDSServer mockServer("test:ds2");
    auto         bus = std::make_shared<MockDataBus>();

    const auto cfg = makeConfigFromYaml(R"(
name: test-ds-reader-2
service: test:ds2
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.2
)");

    EpicsDSMetadataReader reader(bus, nullptr, cfg);

    ASSERT_TRUE(waitForMinBatches(bus, 2, std::chrono::milliseconds(5000)));

    const auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 2u);

    for (const auto& batch : batches)
    {
        ASSERT_TRUE(isSourceMetadata(batch));
        EXPECT_EQ(asSourceMetadata(batch).size(), 30u);
    }
}

// TEST 5 — an attribute change made between scans is reflected in the next
// batch pushed to the bus.
TEST(EpicsDSMetadataIntegrationTest, DsMetadataUpdatedAttributeReflectedOnRescan)
{
    MockDSServer mockServer("test:ds3");
    auto         bus = std::make_shared<MockDataBus>();

    const auto cfg = makeConfigFromYaml(R"(
name: test-ds-reader-3
service: test:ds3
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.2
)");

    EpicsDSMetadataReader reader(bus, nullptr, cfg);

    ASSERT_TRUE(waitForMinBatches(bus, 1, std::chrono::milliseconds(5000)));

    {
        const auto  snapped = bus->snapshot();
        const auto& payload = asSourceMetadata(snapped.front());
        ASSERT_EQ(payload.count("BPMS:IN20:221:X"), 1u);
        EXPECT_EQ(payload.at("BPMS:IN20:221:X").attributes.at("pvStatus"), "Active");
    }

    mockServer.updateAttribute("BPMS:IN20:221:X", "pvStatus", "Inactive");

    ASSERT_TRUE(waitForMinBatches(bus, 2, std::chrono::milliseconds(5000)));

    const auto  snapped2 = bus->snapshot();
    const auto& payload  = asSourceMetadata(snapped2.back());
    ASSERT_EQ(payload.count("BPMS:IN20:221:X"), 1u);
    EXPECT_EQ(payload.at("BPMS:IN20:221:X").attributes.at("pvStatus"), "Inactive");
}

// TEST 6 — a row added between scans appears in the subsequent batch.
TEST(EpicsDSMetadataIntegrationTest, DsMetadataNewRowAppearsOnRescan)
{
    MockDSServer mockServer("test:ds4");
    auto         bus = std::make_shared<MockDataBus>();

    const auto cfg = makeConfigFromYaml(R"(
name: test-ds-reader-4
service: test:ds4
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.2
)");

    EpicsDSMetadataReader reader(bus, nullptr, cfg);

    ASSERT_TRUE(waitForMinBatches(bus, 1, std::chrono::milliseconds(5000)));

    {
        const auto  snapped = bus->snapshot();
        const auto& payload = asSourceMetadata(snapped.front());
        EXPECT_EQ(payload.count("NEW:PV:TEST:X"), 0u);
        EXPECT_EQ(payload.size(), 30u);
    }

    mockServer.addRow({{"channelName", "NEW:PV:TEST:X"},
                       {"hostName",    "cpu-test"},
                       {"owner",       "test"},
                       {"pvStatus",    "Active"},
                       {"recordType",  "ai"},
                       {"recordDesc",  "New test PV"},
                       {"archived",    "false"},
                       {"archiveRate", "0"},
                       {"tags",        "test"}});

    ASSERT_TRUE(waitForMinBatches(bus, 2, std::chrono::milliseconds(5000)));

    const auto  snapped2 = bus->snapshot();
    const auto& payload  = asSourceMetadata(snapped2.back());
    EXPECT_EQ(payload.count("NEW:PV:TEST:X"), 1u);
    EXPECT_EQ(payload.size(), 31u);
}

// TEST 7 — a row removed between scans is absent from the subsequent batch.
TEST(EpicsDSMetadataIntegrationTest, DsMetadataRemovedRowAbsentOnRescan)
{
    MockDSServer mockServer("test:ds5");
    auto         bus = std::make_shared<MockDataBus>();

    const auto cfg = makeConfigFromYaml(R"(
name: test-ds-reader-5
service: test:ds5
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.2
)");

    EpicsDSMetadataReader reader(bus, nullptr, cfg);

    ASSERT_TRUE(waitForMinBatches(bus, 1, std::chrono::milliseconds(5000)));

    {
        const auto  snapped = bus->snapshot();
        const auto& payload = asSourceMetadata(snapped.front());
        EXPECT_EQ(payload.count("VPIO:IN20:111:PRES"), 1u);
        EXPECT_EQ(payload.size(), 30u);
    }

    mockServer.removeRow("VPIO:IN20:111:PRES");

    ASSERT_TRUE(waitForMinBatches(bus, 2, std::chrono::milliseconds(5000)));

    const auto  snapped2 = bus->snapshot();
    const auto& payload  = asSourceMetadata(snapped2.back());
    EXPECT_EQ(payload.count("VPIO:IN20:111:PRES"), 0u);
    EXPECT_EQ(payload.size(), 29u);
}

// TEST 8 — a tags update made between scans is reflected in the subsequent
// batch.
TEST(EpicsDSMetadataIntegrationTest, DsMetadataTagsUpdateReflectedOnRescan)
{
    MockDSServer mockServer("test:ds6");
    auto         bus = std::make_shared<MockDataBus>();

    const auto cfg = makeConfigFromYaml(R"(
name: test-ds-reader-6
service: test:ds6
timeout-sec: 5.0
source-name-column: channelName
tags-column: tags
rescan-interval-sec: 0.2
)");

    EpicsDSMetadataReader reader(bus, nullptr, cfg);

    ASSERT_TRUE(waitForMinBatches(bus, 1, std::chrono::milliseconds(5000)));

    {
        const auto  snapped = bus->snapshot();
        const auto& payload = asSourceMetadata(snapped.front());
        ASSERT_EQ(payload.count("BPMS:IN20:221:X"), 1u);
        const auto& entry = payload.at("BPMS:IN20:221:X");
        ASSERT_TRUE(entry.tags.has_value());
        const auto& tags = entry.tags.value();
        EXPECT_NE(std::find(tags.begin(), tags.end(), "physics"), tags.end());
        EXPECT_NE(std::find(tags.begin(), tags.end(), "bpm"),     tags.end());
        EXPECT_EQ(std::find(tags.begin(), tags.end(), "golden"),  tags.end());
    }

    mockServer.updateTags("BPMS:IN20:221:X", "physics,bpm,fast,survey,golden");

    ASSERT_TRUE(waitForMinBatches(bus, 2, std::chrono::milliseconds(5000)));

    const auto  snapped2 = bus->snapshot();
    const auto& payload  = asSourceMetadata(snapped2.back());
    ASSERT_EQ(payload.count("BPMS:IN20:221:X"), 1u);
    const auto& entry = payload.at("BPMS:IN20:221:X");
    ASSERT_TRUE(entry.tags.has_value());
    const auto& tags = entry.tags.value();
    EXPECT_NE(std::find(tags.begin(), tags.end(), "golden"), tags.end());
}
