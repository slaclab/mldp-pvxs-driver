//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// Tests for the epics-pvxs-bsas-gen1 reader against the deterministic BSAS
// tables published by the mock IOC (test/mock/BsasTablePV, test/mock/sioc.cpp).

#include <gtest/gtest.h>

#include "../../../../config/test_config_helpers.h"
#include "../../../../mock/sioc.h"
#include "bsas_table_test_utils.h"

#include <metrics/Metrics.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics/pvxs/EpicsPVXSBsasGen1Reader.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace bsas_table_test_utils;
using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::ReaderFactory;
using mldp_pvxs_driver::reader::impl::epics::EpicsPVXSBsasGen1Reader;

namespace {

constexpr const char* kReaderType = "epics-pvxs-bsas-gen1";

/// Row timestamps BsasTablePV publishes for a group with @p rows rows.
void expectMockRowTimestamps(const DataBatch& frame, std::size_t rows)
{
    ASSERT_EQ(frame.timestamps.size(), rows);
    const auto seconds = frame.timestamps[0].epoch_seconds;
    EXPECT_GT(seconds, static_cast<uint64_t>(BsasTablePV::kBaseSeconds));
    for (std::size_t r = 0; r < rows; ++r)
    {
        EXPECT_EQ(frame.timestamps[r].epoch_seconds, seconds);
        EXPECT_EQ(frame.timestamps[r].nanoseconds, r * BsasTablePV::kRowNanosStep);
    }
}

/// Mock update counter of a group, recovered from its row timestamp.
int counterOf(const DataBatch& frame)
{
    return static_cast<int>(frame.timestamps.at(0).epoch_seconds - BsasTablePV::kBaseSeconds);
}

} // namespace

class EpicsPVXSBsasGen1ReaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus = std::make_shared<RecordingBus>();
        server = std::make_shared<PVServer>();
    }

    void TearDown() override
    {
        reader.reset();
        server.reset();
        bus.reset();
    }

    void start(const std::string& yaml, std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics = nullptr)
    {
        reader = ReaderFactory::create(kReaderType, bus, makeConfigFromYaml(yaml), metrics);
        ASSERT_NE(reader, nullptr);
    }

    /// Wait for @p count complete groups with data for @p pv and return them.
    std::vector<Group> waitForDataGroups(const std::string& pv, std::size_t count)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto groups = dataGroupsFor(bus->snapshot(), pv);
            if (groups.size() >= count)
            {
                return groups;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return dataGroupsFor(bus->snapshot(), pv);
    }

    std::shared_ptr<RecordingBus>                     bus;
    std::shared_ptr<PVServer>                         server;
    std::shared_ptr<mldp_pvxs_driver::reader::Reader> reader;
};

TEST_F(EpicsPVXSBsasGen1ReaderTest, FactoryCreatesReaderWithName)
{
    start(R"(
name: bsas_gen1
)");
    EXPECT_EQ(reader->name(), "bsas_gen1");
    EXPECT_NE(std::dynamic_pointer_cast<EpicsPVXSBsasGen1Reader>(reader), nullptr);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, EmitsTabularBatchesWithAllColumnsAndExactValues)
{
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_table
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_table", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto& g = groups.front();

    // Default pvs-per-data-batch = 1 -> one DataBatch per column, PV columns only.
    EXPECT_EQ(g.columnNames(), (std::vector<std::string>{"PV_A", "PV_B", "PV_C"}));
    EXPECT_EQ(g.frames().size(), 3u);

    for (const auto& b : g.data)
    {
        const auto& ts = std::get<TimeSeriesPayload>(b.payload);
        EXPECT_TRUE(ts.is_tabular);
        EXPECT_FALSE(ts.end_of_batch_group);
        EXPECT_EQ(ts.root_source_name, "test:bsas_gen1_table");
        EXPECT_EQ(b.reader_name, "bsas_gen1");
        EXPECT_EQ(b.metadata.at("source"), "test:bsas_gen1_table");
    }

    const DataBatch* frameA = nullptr;
    const auto*      a = g.column("PV_A", &frameA);
    ASSERT_NE(a, nullptr);
    expectMockRowTimestamps(*frameA, 4);
    const int counter = counterOf(*frameA);

    ASSERT_TRUE(std::holds_alternative<std::vector<double>>(a->values));
    EXPECT_EQ(asDoubles(*a), (std::vector<double>{counter * 100.0, counter * 100.0 + 1, counter * 100.0 + 2, counter * 100.0 + 3}));

    const DataBatch* frameB = nullptr;
    const auto*      b = g.column("PV_B", &frameB);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(std::holds_alternative<std::vector<int32_t>>(b->values));
    EXPECT_EQ(asDoubles(*b)[0], counter * 100.0 + 1000.0);
    EXPECT_TRUE(sameTimestamps(*frameB, *frameA));

    const auto* c = g.column("PV_C");
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(std::holds_alternative<std::vector<float>>(c->values));
    EXPECT_FLOAT_EQ(static_cast<float>(asDoubles(*c)[3]), static_cast<float>(counter + 1.5));
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, TimestampColumnsAreNotEmittedAsData)
{
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_table
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_table", 1);
    ASSERT_GE(groups.size(), 1u);
    for (const auto& name : groups.front().columnNames())
    {
        EXPECT_NE(name, "secondsPastEpoch");
        EXPECT_NE(name, "nanoseconds");
    }
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, MarkerClosesEveryGroup)
{
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_table
)");
    ASSERT_TRUE(bus->waitForMarkers("test:bsas_gen1_table", 3));

    const auto groups = groupsFor(bus->snapshot(), "test:bsas_gen1_table");
    ASSERT_GE(groups.size(), 3u);
    for (const auto& g : groups)
    {
        const auto& ts = std::get<TimeSeriesPayload>(g.marker.payload);
        EXPECT_TRUE(ts.end_of_batch_group);
        EXPECT_TRUE(ts.is_tabular);
        EXPECT_TRUE(ts.frames.empty());
        EXPECT_EQ(ts.root_source_name, "test:bsas_gen1_table");
        EXPECT_EQ(g.marker.reader_name, "bsas_gen1");
        EXPECT_EQ(g.marker.metadata.at("source"), "test:bsas_gen1_table");
    }

    // Successive updates carry increasing counters.
    const auto data = dataGroupsFor(bus->snapshot(), "test:bsas_gen1_table");
    ASSERT_GE(data.size(), 2u);
    EXPECT_LT(counterOf(*data[0].frames().front()), counterOf(*data[1].frames().front()));
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, PvsPerDataBatchGroupsColumnsSharingTimestamps)
{
    start(R"(
name: bsas_gen1
pvs-per-data-batch: 4
pvs:
  - name: test:bsas_gen1_wide
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_wide", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto frames = groups.front().frames();

    // 10 columns, 4 per DataBatch -> 4 + 4 + 2.
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0]->columns.size(), 4u);
    EXPECT_EQ(frames[1]->columns.size(), 4u);
    EXPECT_EQ(frames[2]->columns.size(), 2u);
    for (const auto* f : frames)
    {
        expectMockRowTimestamps(*f, 4);
    }
    EXPECT_EQ(frames[0]->columns[0].name, "W00");
    EXPECT_EQ(frames[2]->columns[1].name, "W09");
    EXPECT_EQ(asDoubles(frames[2]->columns[1]), (std::vector<double>{9000, 9001, 9002, 9003}));
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, PvsPerDataBatchZeroPutsAllColumnsInOneDataBatch)
{
    start(R"(
name: bsas_gen1
pvs-per-data-batch: 0
pvs:
  - name: test:bsas_gen1_wide
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_wide", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto frames = groups.front().frames();
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0]->columns.size(), 10u);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, PerPvOptionsOverrideReaderLevel)
{
    start(R"(
name: bsas_gen1
pvs-per-data-batch: 1
pvs:
  - name: test:bsas_gen1_wide
    option:
      pvs-per-data-batch: 5
  - name: test:bsas_gen1_table
)");
    const auto wide = waitForDataGroups("test:bsas_gen1_wide", 1);
    const auto table = waitForDataGroups("test:bsas_gen1_table", 1);
    ASSERT_GE(wide.size(), 1u);
    ASSERT_GE(table.size(), 1u);
    EXPECT_EQ(wide.front().frames().size(), 2u);
    EXPECT_EQ(table.front().frames().size(), 3u);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, DataBatchesPerPushSplitsGroupIntoSeveralBusBatches)
{
    start(R"(
name: bsas_gen1
data-batches-per-push: 3
pvs:
  - name: test:bsas_gen1_wide
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_wide", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto& g = groups.front();

    // 10 DataBatches (1 column each), 3 per bus push -> 3 + 3 + 3 + 1.
    ASSERT_EQ(g.data.size(), 4u);
    EXPECT_EQ(std::get<TimeSeriesPayload>(g.data[0].payload).frames.size(), 3u);
    EXPECT_EQ(std::get<TimeSeriesPayload>(g.data[3].payload).frames.size(), 1u);
    EXPECT_EQ(g.columnNames().size(), 10u);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, DataBatchesPerPushZeroPushesOneBusBatch)
{
    start(R"(
name: bsas_gen1
data-batches-per-push: 0
pvs:
  - name: test:bsas_gen1_wide
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_wide", 1);
    ASSERT_GE(groups.size(), 1u);
    ASSERT_EQ(groups.front().data.size(), 1u);
    EXPECT_EQ(groups.front().frames().size(), 10u);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, DefaultRowsPerDataBatchIs120)
{
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_long
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_long", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto& g = groups.front();

    // 250 rows -> chunks of 120 + 120 + 10, row chunks in time order, columns in table order.
    EXPECT_EQ(g.columnNames(), (std::vector<std::string>{"L_A", "L_B", "L_A", "L_B", "L_A", "L_B"}));
    for (const char* name : {"L_A", "L_B"})
    {
        const auto chunks = g.chunks(name);
        ASSERT_EQ(chunks.size(), 3u) << name;
        const std::size_t sizes[] = {120, 120, 10};
        std::size_t       row = 0;
        for (std::size_t i = 0; i < 3; ++i)
        {
            const auto& [batch, col] = chunks[i];
            ASSERT_EQ(batch->timestamps.size(), sizes[i]);
            const auto values = asDoubles(*col);
            ASSERT_EQ(values.size(), sizes[i]);
            for (std::size_t r = 0; r < sizes[i]; ++r, ++row)
            {
                EXPECT_EQ(batch->timestamps[r].nanoseconds, row * BsasTablePV::kRowNanosStep);
                EXPECT_EQ(values[r], (std::string(name) == "L_A" ? 0.0 : 10000.0) + static_cast<double>(row));
            }
        }
    }
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, RowsPerDataBatchSplitsRowsWithMatchingTimestamps)
{
    start(R"(
name: bsas_gen1
rows-per-data-batch: 3
pvs-per-data-batch: 0
pvs:
  - name: test:bsas_gen1_table
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_table", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto frames = groups.front().frames();

    // 4 rows, 3 per DataBatch, all 3 PVs per DataBatch -> [rows 0-2], [row 3].
    ASSERT_EQ(frames.size(), 2u);
    ASSERT_EQ(frames[0]->timestamps.size(), 3u);
    ASSERT_EQ(frames[1]->timestamps.size(), 1u);
    EXPECT_EQ(frames[1]->timestamps[0].nanoseconds, 3 * BsasTablePV::kRowNanosStep);
    ASSERT_EQ(frames[0]->columns.size(), 3u);
    ASSERT_EQ(frames[1]->columns.size(), 3u);
    for (const auto* f : frames)
    {
        for (const auto& c : f->columns)
        {
            EXPECT_EQ(asDoubles(c).size(), f->timestamps.size()) << c.name;
        }
    }
    const int counter = counterOf(*frames[0]);
    EXPECT_EQ(asDoubles(frames[1]->columns[0]), (std::vector<double>{counter * 100.0 + 3}));
    EXPECT_TRUE(std::holds_alternative<std::vector<int32_t>>(frames[1]->columns[1].values));
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, RowsPerDataBatchZeroKeepsAllRows)
{
    start(R"(
name: bsas_gen1
rows-per-data-batch: 0
pvs:
  - name: test:bsas_gen1_long
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_long", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto frames = groups.front().frames();
    ASSERT_EQ(frames.size(), 2u);
    for (const auto* f : frames)
    {
        EXPECT_EQ(f->timestamps.size(), 250u);
        EXPECT_EQ(asDoubles(f->columns.at(0)).size(), 250u);
    }
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, RowAndPvLimitsCombineWithPushLimit)
{
    start(R"(
name: bsas_gen1
rows-per-data-batch: 100
pvs-per-data-batch: 2
data-batches-per-push: 2
pvs:
  - name: test:bsas_gen1_long
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_long", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto& g = groups.front();
    // 250 rows / 100 -> 3 DataBatches of [L_A, L_B]; 2 per push -> pushes of 2 + 1.
    ASSERT_EQ(g.data.size(), 2u);
    EXPECT_EQ(std::get<TimeSeriesPayload>(g.data[0].payload).frames.size(), 2u);
    EXPECT_EQ(std::get<TimeSeriesPayload>(g.data[1].payload).frames.size(), 1u);
    const auto frames = g.frames();
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[2]->timestamps.size(), 50u);
    EXPECT_EQ(frames[2]->columns.size(), 2u);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, ColumnBatchSizeIsRejected)
{
    EXPECT_ANY_THROW(ReaderFactory::create(kReaderType, bus, makeConfigFromYaml(R"(
name: bsas_gen1
column-batch-size: 50
)")));
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, AllNaNColumnsAreDropped)
{
    start(R"(
name: bsas_gen1
pvs-per-data-batch: 2
pvs:
  - name: test:bsas_gen1_nan
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_nan", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto frames = groups.front().frames();

    // NaN column does not count towards pvs-per-data-batch: one frame [OK_A, OK_C].
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(groups.front().columnNames(), (std::vector<std::string>{"OK_A", "OK_C"}));
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, AllNaNTableProducesMarkerOnly)
{
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_all_nan
)");
    ASSERT_TRUE(bus->waitForMarkers("test:bsas_gen1_all_nan", 2));
    for (const auto& g : groupsFor(bus->snapshot(), "test:bsas_gen1_all_nan"))
    {
        EXPECT_TRUE(g.data.empty());
    }
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, TableWithoutTimestampsRecordsErrorAndPushesMarker)
{
    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_no_ts
)",
          metrics);
    ASSERT_TRUE(bus->waitForMarkers("test:bsas_gen1_no_ts", 2));
    for (const auto& g : groupsFor(bus->snapshot(), "test:bsas_gen1_no_ts"))
    {
        EXPECT_TRUE(g.data.empty());
    }
    EXPECT_GE(metrics->readerErrorsTotal(prometheus::Labels{{"source", "test:bsas_gen1_no_ts"}}), 2.0);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, CustomTimestampFieldNames)
{
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_custom_ts
    option:
      tsSeconds: secs
      tsNanos: nsecs
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_custom_ts", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto& g = groups.front();
    EXPECT_EQ(g.columnNames(), (std::vector<std::string>{"PV_A", "PV_B"}));

    const DataBatch* frame = nullptr;
    const auto*      b = g.column("PV_B", &frame);
    ASSERT_NE(b, nullptr);
    expectMockRowTimestamps(*frame, 4);
    ASSERT_TRUE(std::holds_alternative<std::vector<int64_t>>(b->values));
    EXPECT_EQ(asDoubles(*b)[1], counterOf(*frame) * 2.0 + 1.0);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, DefaultTimestampNamesMissFromCustomTable)
{
    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_custom_ts
)",
          metrics);
    ASSERT_TRUE(bus->waitForMarkers("test:bsas_gen1_custom_ts", 2));
    EXPECT_TRUE(dataGroupsFor(bus->snapshot(), "test:bsas_gen1_custom_ts").empty());
    EXPECT_GE(metrics->readerErrorsTotal(prometheus::Labels{{"source", "test:bsas_gen1_custom_ts"}}), 2.0);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, StaticAndPerPvMetadataMerged)
{
    start(R"(
name: bsas_gen1
metadata:
  facility: lcls
  subsystem: bsas
pvs:
  - name: test:bsas_gen1_table
    metadata:
      subsystem: override
      line: hxr
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_table", 1);
    ASSERT_GE(groups.size(), 1u);
    std::vector<EventBatch> batches = groups.front().data;
    batches.push_back(groups.front().marker);
    for (const auto& b : batches)
    {
        EXPECT_EQ(b.metadata.at("facility"), "lcls");
        EXPECT_EQ(b.metadata.at("subsystem"), "override");
        EXPECT_EQ(b.metadata.at("line"), "hxr");
        EXPECT_EQ(b.metadata.at("source"), "test:bsas_gen1_table");
    }
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, MultiplePvsAreKeptApart)
{
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_table
  - name: test:bsas_gen1_wide
)");
    const auto table = waitForDataGroups("test:bsas_gen1_table", 1);
    const auto wide = waitForDataGroups("test:bsas_gen1_wide", 1);
    ASSERT_GE(table.size(), 1u);
    ASSERT_GE(wide.size(), 1u);
    EXPECT_EQ(table.front().columnNames().size(), 3u);
    EXPECT_EQ(wide.front().columnNames().size(), 10u);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, ParallelConversionKeepsColumnOrder)
{
    start(R"(
name: bsas_gen1
thread-pool: 4
pvs:
  - name: test:bsas_gen1_wide
)");
    const auto groups = waitForDataGroups("test:bsas_gen1_wide", 2);
    ASSERT_GE(groups.size(), 2u);
    for (const auto& g : groups)
    {
        const auto names = g.columnNames();
        ASSERT_EQ(names.size(), 10u);
        EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
    }
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, ReaderMetricsRecorded)
{
    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_table
)",
          metrics);
    ASSERT_TRUE(bus->waitForMarkers("test:bsas_gen1_table", 2));

    const prometheus::Labels tag{{"source", "test:bsas_gen1_table"}};
    EXPECT_GE(metrics->readerEventsTotal(prometheus::Labels{{"source", "test:bsas_gen1_table"}}), 1.0);
    EXPECT_GE(metrics->readerEventsReceivedTotal(prometheus::Labels{{"source", "test:bsas_gen1_table"}}), 1.0);
    EXPECT_GT(metrics->readerDataBytesTotal(tag), 0.0);
    EXPECT_EQ(metrics->readerErrorsTotal(prometheus::Labels{{"source", "test:bsas_gen1_table"}}), 0.0);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, RejectedPushIsCountedAndNotRetried)
{
    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    bus->setReject(true);
    start(R"(
name: bsas_gen1
pvs-per-data-batch: 0
pvs:
  - name: test:bsas_gen1_table
)",
          metrics);

    // Each update = 1 data batch + 1 marker, both rejected.
    ASSERT_TRUE(bus->waitForRejected(4));
    reader.reset(); // drain the pool so counters are stable
    EXPECT_EQ(bus->pushCalls(), bus->rejected());
    EXPECT_EQ(bus->pushCalls() % 2, 0u) << "every rejected push is attempted exactly once";
    EXPECT_GE(metrics->readerErrorsTotal(prometheus::Labels{{"source", "test:bsas_gen1_table"}}), static_cast<double>(bus->rejected()));
    EXPECT_TRUE(bus->snapshot().empty());
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, ReaderAcceptsDataAgainAfterBusRecovers)
{
    bus->setReject(true);
    start(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_table
)");
    ASSERT_TRUE(bus->waitForRejected(2));
    bus->setReject(false);
    const auto groups = waitForDataGroups("test:bsas_gen1_table", 1);
    EXPECT_GE(groups.size(), 1u);
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, OptionTypeIsRejected)
{
    EXPECT_ANY_THROW(ReaderFactory::create(kReaderType, bus, makeConfigFromYaml(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_table
    option:
      type: slac-bsas-table
)")));
}

TEST_F(EpicsPVXSBsasGen1ReaderTest, Gen2OnlyKeysAreRejected)
{
    EXPECT_ANY_THROW(ReaderFactory::create(kReaderType, bus, makeConfigFromYaml(R"(
name: bsas_gen1
pidField: pid
)")));
    EXPECT_ANY_THROW(ReaderFactory::create(kReaderType, bus, makeConfigFromYaml(R"(
name: bsas_gen1
pvs:
  - name: test:bsas_gen1_table
    option:
      valueSuffix: Val
)")));
}
