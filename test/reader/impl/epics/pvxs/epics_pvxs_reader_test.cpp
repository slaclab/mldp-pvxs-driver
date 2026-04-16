#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../../../config/test_config_helpers.h"
#include "../../../../mock/sioc.h"
#include "../shared/epics_typed_pv_test_utils.h"

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <metrics/MetricsSnapshot.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics/pvxs/EpicsPVXSReader.h>
#include <util/bus/IDataBus.h>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::util::bus::IDataBus;
using namespace mldp_pvxs_driver::reader::impl::epics;

// Concrete mock implementation of IDataBus for testing
class MockEventBusPush : public IDataBus
{
public:
    using EventBatch = IDataBus::EventBatch;

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
        if (metrics_)
        {
            const size_t             total_values = batch.frames.size();
            const auto               source = batch.root_source.empty() ? std::string("unknown") : batch.root_source;
            const prometheus::Labels tags{{"source", source}};
            metrics_->incrementBusPushes(static_cast<double>(total_values), tags);
            metrics_->incrementBusPayloadBytes(0.0, tags);
            metrics_->setBusPayloadBytesPerSecond(0.0, tags);
        }
        received_events.push_back(std::move(batch));
        return true;
    }

    // Method to get the number of events received
    size_t event_count() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return received_events.size();
    }

    size_t total_value_count() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t                      total = 0;
        for (const auto& batch : received_events)
        {
            total += batch.frames.size();
        }
        return total;
    }

    const EventBatch* last_batch() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (received_events.empty())
        {
            return nullptr;
        }
        return &received_events.back();
    }

    std::optional<EventBatch> last_batch_copy() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (received_events.empty())
        {
            return std::nullopt;
        }
        return received_events.back();
    }

    // Method to get the last event's DataFrame pointer
    const dp::service::common::DataFrame* last_event() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (received_events.empty())
        {
            return nullptr;
        }
        const auto& batch = received_events.back();
        if (batch.frames.empty())
        {
            return nullptr;
        }
        return &batch.frames.front();
    }

    // Method to clear events
    void clear_events()
    {
        std::lock_guard<std::mutex> lock(mutex);
        received_events.clear();
    }

    void set_metrics(std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics)
    {
        std::lock_guard<std::mutex> lock(mutex);
        metrics_ = std::move(metrics);
    }

private:
    std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics_;
};

namespace {

using DataFrame = dp::service::common::DataFrame;

std::optional<std::string> frameSource(const DataFrame& frame)
{
    if (frame.doublecolumns_size() > 0)
        return frame.doublecolumns(0).name();
    if (frame.floatcolumns_size() > 0)
        return frame.floatcolumns(0).name();
    if (frame.int32columns_size() > 0)
        return frame.int32columns(0).name();
    if (frame.int64columns_size() > 0)
        return frame.int64columns(0).name();
    if (frame.boolcolumns_size() > 0)
        return frame.boolcolumns(0).name();
    if (frame.stringcolumns_size() > 0)
        return frame.stringcolumns(0).name();
    if (frame.datacolumns_size() > 0)
        return frame.datacolumns(0).name();
    if (frame.doublearraycolumns_size() > 0)
        return frame.doublearraycolumns(0).name();
    if (frame.floatarraycolumns_size() > 0)
        return frame.floatarraycolumns(0).name();
    if (frame.int32arraycolumns_size() > 0)
        return frame.int32arraycolumns(0).name();
    if (frame.int64arraycolumns_size() > 0)
        return frame.int64arraycolumns(0).name();
    if (frame.boolarraycolumns_size() > 0)
        return frame.boolarraycolumns(0).name();
    return std::nullopt;
}

const DataFrame* findLatestDataFrameForSource(const MockEventBusPush& bus, const std::string& source)
{
    std::lock_guard<std::mutex> lock(bus.mutex);
    for (auto it = bus.received_events.rbegin(); it != bus.received_events.rend(); ++it)
    {
        for (auto fit = it->frames.rbegin(); fit != it->frames.rend(); ++fit)
        {
            const auto src = frameSource(*fit);
            if (src.has_value() && *src == source)
            {
                return &(*fit);
            }
        }
    }
    return nullptr;
}

size_t countEventsForSource(const MockEventBusPush& bus, const std::string& source)
{
    std::lock_guard<std::mutex> lock(bus.mutex);
    size_t                      total = 0;
    for (const auto& batch : bus.received_events)
    {
        for (const auto& frame : batch.frames)
        {
            const auto src = frameSource(frame);
            if (src.has_value() && *src == source)
            {
                ++total;
            }
        }
    }
    return total;
}

std::optional<long long> findReaderPushesForPv(const mldp_pvxs_driver::metrics::MetricsData& snapshot, const std::string& pv)
{
    for (const auto& reader : snapshot.readers)
    {
        if (reader.pv_name == pv)
        {
            return reader.pushes;
        }
    }
    return std::nullopt;
}

} // namespace

// Test fixture
class EpicsPVXSReaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create mock bus
        mock_bus = std::make_shared<MockEventBusPush>();
        // Create mock PV server
        mock_pv_server = std::make_shared<PVServer>();
    }

    void TearDown() override
    {
        // Cleanup if necessary
        mock_pv_server.reset();
        mock_bus.reset();
    }

    // EpicsReader instance under test
    std::shared_ptr<MockEventBusPush> mock_bus;
    std::shared_ptr<PVServer>         mock_pv_server;
};

// Test constructor and name through factory
TEST_F(EpicsPVXSReaderTest, ConstructorAndName)
{
    const std::string yaml = R"(
name: epics_1
)";

    const auto cfg = makeConfigFromYaml(yaml);

    // Use ReaderFactory to create EpicsReader instance (as shown in test/driver.cpp)
    auto reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg);
    // reader = std::dynamic_pointer_cast<mldp_pvxs_driver::reader::impl::epics::EpicsReader>(std::shared_ptr<mldp_pvxs_driver::reader::Reader>(reader_ptr));

    ASSERT_NE(reader_ptr, nullptr);

    // Verify name is set correctly (assuming default or configurable name)
    EXPECT_EQ(reader_ptr->name(), "epics_1");
}

// Test addPV method
TEST_F(EpicsPVXSReaderTest, MonitorPVOnConfiguration)
{
    const std::string yaml = R"(
name: epics_1
pvs:
  - name: test:counter
)";
    const auto        cfg = makeConfigFromYaml(yaml);

    // Use ReaderFactory to create EpicsReader instance (as shown in test/driver.cpp)
    auto reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg);

    // check if is not null
    ASSERT_NE(reader_ptr, nullptr);

    // Verify name is set correctly (assuming default or configurable name)
    EXPECT_EQ(reader_ptr->name(), "epics_1");

    // wiat for the reception of the first event
    const int max_wait_ms = 5000;
    int       waited_ms = 0;
    while (mock_bus->event_count() == 0 && waited_ms < max_wait_ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }
    EXPECT_GT(mock_bus->event_count(), 0) << "No events received within timeout";
}

TEST_F(EpicsPVXSReaderTest, MonitorPVOnConfigurationEpicsBase)
{
    const std::string yaml = R"(
name: epics_base_1
pvs:
  - name: test:counter
)";
    const auto        cfg = makeConfigFromYaml(yaml);

    auto reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics-base", mock_bus, cfg);

    ASSERT_NE(reader_ptr, nullptr);
    EXPECT_EQ(reader_ptr->name(), "epics_base_1");

    const int max_wait_ms = 5000;
    int       waited_ms = 0;
    while (mock_bus->event_count() == 0 && waited_ms < max_wait_ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }
    EXPECT_GT(mock_bus->event_count(), 0) << "No events received within timeout (epics-base)";
}

TEST_F(EpicsPVXSReaderTest, CounterPVEmitsMultipleEvents)
{
    const std::string yaml = R"(
name: epics_1
pvs:
  - name: test:counter
)";
    const auto        cfg = makeConfigFromYaml(yaml);
    auto              metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(
        mldp_pvxs_driver::metrics::MetricsConfig());
    mock_bus->set_metrics(metrics);
    auto reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg, metrics);
    ASSERT_NE(reader_ptr, nullptr);

    const int                                  max_wait_ms = 5000;
    int                                        waited_ms = 0;
    size_t                                     counter_events = 0;
    bool                                       matched_metrics = false;
    mldp_pvxs_driver::metrics::MetricsSnapshot snapshotter;
    while (waited_ms < max_wait_ms)
    {
        counter_events = countEventsForSource(*mock_bus, "test:counter");
        const auto snapshot = snapshotter.getSnapshot(*metrics);
        const auto pushes = findReaderPushesForPv(snapshot, "test:counter");
        matched_metrics = pushes.has_value() && counter_events >= 2 &&
                          pushes.value() == static_cast<long long>(counter_events);
        if (matched_metrics)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }

    EXPECT_GE(counter_events, 2u) << "Expected multiple events for test:counter";
    EXPECT_TRUE(matched_metrics) << "Expected metrics snapshot pushes to match received events for test:counter";
}

TEST_F(EpicsPVXSReaderTest, SimulatedPVsProduceEventsAndExpectedTypes)
{
    const std::string yaml = epics_typed_pv_test_utils::buildTypedCoverageYaml("epics_1");

    const auto cfg = makeConfigFromYaml(yaml);
    auto       reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg);
    ASSERT_NE(reader_ptr, nullptr);

    const auto expected = epics_typed_pv_test_utils::allTypedCoveragePvsSet();

    std::set<std::string> seen;
    const int             max_wait_ms = 10000;
    int                   waited_ms = 0;
    while (seen != expected && waited_ms < max_wait_ms)
    {
        {
            std::lock_guard<std::mutex> lock(mock_bus->mutex);
            for (const auto& batch : mock_bus->received_events)
            {
                for (const auto& frame : batch.frames)
                {
                    const auto src = frameSource(frame);
                    if (!src.has_value())
                    {
                        continue;
                    }
                    const auto& source = *src;
                    if (expected.count(source))
                    {
                        seen.insert(source);
                    }
                }
            }
        }

        if (seen == expected)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }

    if (seen != expected)
    {
        std::ostringstream oss;
        oss << "Missing PVs:";
        for (const auto& pv : expected)
        {
            if (!seen.count(pv))
            {
                oss << " " << pv;
            }
        }
        FAIL() << oss.str();
    }

    epics_typed_pv_test_utils::assertTypedCoverageDataFrames(
        [this](const std::string& pv)
        {
            return findLatestDataFrameForSource(*mock_bus, pv);
        });
}

TEST_F(EpicsPVXSReaderTest, NTTableRowTimestampSplitsToPerColumnSources)
{
    const std::string yaml = R"(
name: epics_1
pvs:
  - name: test:bsas_table
    option:
      type: slac-bsas-table
)";

    const auto cfg = makeConfigFromYaml(yaml);
    auto       reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg);
    ASSERT_NE(reader_ptr, nullptr);

    // With streaming push, each column arrives as a separate push() call.
    const int max_wait_ms = 5000;
    int       waited_ms = 0;
    while (waited_ms < max_wait_ms)
    {
        if (countEventsForSource(*mock_bus, "PV_A") >= 3 &&
            countEventsForSource(*mock_bus, "PV_B") >= 3)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }

    EXPECT_GE(countEventsForSource(*mock_bus, "PV_A"), 3u);
    EXPECT_GE(countEventsForSource(*mock_bus, "PV_B"), 3u);

    // Verify no timestamp columns leaked as sources
    EXPECT_EQ(countEventsForSource(*mock_bus, "secondsPastEpoch"), 0u);
    EXPECT_EQ(countEventsForSource(*mock_bus, "nanoseconds"), 0u);

    // Root source remains the root PV for metrics/correlation even when
    // ingestion sources are split by BSAS column.
    bool saw_root_for_column_batch = false;
    {
        std::lock_guard<std::mutex> lock(mock_bus->mutex);
        for (const auto& batch : mock_bus->received_events)
        {
            bool has_column = false;
            for (const auto& frame : batch.frames)
            {
                const auto src = frameSource(frame);
                if (src == "PV_A" || src == "PV_B")
                {
                    has_column = true;
                    break;
                }
            }
            if (has_column && batch.root_source == "test:bsas_table")
            {
                saw_root_for_column_batch = true;
                break;
            }
        }
    }
    EXPECT_TRUE(saw_root_for_column_batch);

    // Collect events for each column across all batches
    std::vector<DataFrame> ampl, stat;
    {
        std::lock_guard<std::mutex> lock(mock_bus->mutex);
        for (const auto& batch : mock_bus->received_events)
        {
            for (const auto& frame : batch.frames)
            {
                const auto src = frameSource(frame);
                if (src == "PV_A")
                {
                    ampl.push_back(frame);
                }
                if (src == "PV_B")
                {
                    stat.push_back(frame);
                }
            }
        }
    }

    ASSERT_GE(ampl.size(), 3u);
    ASSERT_GE(stat.size(), 3u);

    // Per-row timestamps are shared across columns.
    for (size_t i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(ampl[i].has_datatimestamps());
        ASSERT_TRUE(stat[i].has_datatimestamps());
        ASSERT_GT(ampl[i].datatimestamps().timestamplist().timestamps_size(), 0);
        ASSERT_GT(stat[i].datatimestamps().timestamplist().timestamps_size(), 0);
        EXPECT_EQ(ampl[i].datatimestamps().timestamplist().timestamps(0).epochseconds(),
                  stat[i].datatimestamps().timestamplist().timestamps(0).epochseconds());
        EXPECT_EQ(ampl[i].datatimestamps().timestamplist().timestamps(0).nanoseconds(),
                  stat[i].datatimestamps().timestamplist().timestamps(0).nanoseconds());
    }

    // PV_A: Float64 column — values are 1.0+sin(time) based, just check type
    ASSERT_GT(ampl[0].doublecolumns_size(), 0);
    ASSERT_GT(ampl[0].doublecolumns(0).values_size(), 0);

    // PV_B: Int32 column (was String in old mock)
    ASSERT_GT(stat[0].int32columns_size(), 0);
    ASSERT_GT(stat[0].int32columns(0).values_size(), 0);
}
