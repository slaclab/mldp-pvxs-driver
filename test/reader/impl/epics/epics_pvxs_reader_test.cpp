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

#include "../../../config/test_config_helpers.h"
#include "../../../mock/sioc.h"

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <metrics/MetricsSnapshot.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics/EpicsPVXSReader.h>
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
            size_t total_values = 0;
            for (const auto& [_, values] : batch.values)
            {
                total_values += values.size();
            }
            const auto source = batch.root_source.empty() ? std::string("unknown") : batch.root_source;
            const prometheus::Labels tags{{"source", source}};
            metrics_->incrementBusPushes(static_cast<double>(total_values), tags);
            metrics_->incrementBusPayloadBytes(0.0, tags);
            metrics_->setBusPayloadBytesPerSecond(0.0, tags);
        }
        received_events.push_back(std::move(batch));
        return true;
    }

    std::vector<SourceInfo> querySourcesInfo(const std::set<std::string>&) override
    {
        return {};
    }

    std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>> querySourcesData(
        const std::set<std::string>&,
        const mldp_pvxs_driver::util::bus::QuerySourcesDataOptions&) override
    {
        return std::nullopt;
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
            for (const auto& [_, values] : batch.values)
            {
                total += values.size();
            }
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
        if (batch.values.empty())
        {
            return nullptr;
        }
        const auto& first_entry = *batch.values.begin();
        if (first_entry.second.empty() || first_entry.second.front() == nullptr)
        {
            return nullptr;
        }
        return &first_entry.second.front()->data_value;
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

const DataFrame* findLatestDataFrameForSource(const MockEventBusPush& bus, const std::string& source)
{
    std::lock_guard<std::mutex> lock(bus.mutex);
    for (auto it = bus.received_events.rbegin(); it != bus.received_events.rend(); ++it)
    {
        const auto vit = it->values.find(source);
        if (vit == it->values.end() || vit->second.empty())
        {
            continue;
        }
        const auto& ev = vit->second.back();
        if (!ev)
        {
            continue;
        }
        return &ev->data_value;
    }
    return nullptr;
}

bool hasColumnWithName(const DataFrame& df, const std::string& name)
{
    for (int i = 0; i < df.stringcolumns_size(); ++i)
        if (df.stringcolumns(i).name() == name) return true;
    for (int i = 0; i < df.int32columns_size(); ++i)
        if (df.int32columns(i).name() == name) return true;
    for (int i = 0; i < df.int64columns_size(); ++i)
        if (df.int64columns(i).name() == name) return true;
    for (int i = 0; i < df.floatcolumns_size(); ++i)
        if (df.floatcolumns(i).name() == name) return true;
    for (int i = 0; i < df.doublecolumns_size(); ++i)
        if (df.doublecolumns(i).name() == name) return true;
    for (int i = 0; i < df.boolcolumns_size(); ++i)
        if (df.boolcolumns(i).name() == name) return true;
    return false;
}

size_t countEventsForSource(const MockEventBusPush& bus, const std::string& source)
{
    std::lock_guard<std::mutex> lock(bus.mutex);
    size_t                      total = 0;
    for (const auto& batch : bus.received_events)
    {
        const auto it = batch.values.find(source);
        if (it == batch.values.end())
        {
            continue;
        }
        total += it->second.size();
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

    const int max_wait_ms = 5000;
    int       waited_ms = 0;
    size_t    counter_events = 0;
    bool      matched_metrics = false;
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
    const std::string yaml = R"(
name: epics_1
pvs:
  - name: test:counter
  - name: test:voltage
  - name: test:status
  - name: test:waveform
  - name: test:table
)";

    const auto cfg = makeConfigFromYaml(yaml);
    auto       reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg);
    ASSERT_NE(reader_ptr, nullptr);

    const std::set<std::string> expected{
        "test:counter",
        "test:voltage",
        "test:status",
        "test:waveform",
        "test:table",
    };

    std::set<std::string> seen;
    const int             max_wait_ms = 5000;
    int                   waited_ms = 0;
    while (seen != expected && waited_ms < max_wait_ms)
    {
        {
            std::lock_guard<std::mutex> lock(mock_bus->mutex);
            for (const auto& batch : mock_bus->received_events)
            {
                for (const auto& [source, _] : batch.values)
                {
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

    // test:counter (NTScalar<Int32>) -> int32 column named by source PV
    {
        const auto df = findLatestDataFrameForSource(*mock_bus, "test:counter");
        ASSERT_NE(df, nullptr);
        ASSERT_GT(df->int32columns_size(), 0);
        EXPECT_EQ(df->int32columns(0).name(), "test:counter");
        EXPECT_GT(df->int32columns(0).values(0), 0);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    }

    // test:voltage (NTScalar<Float64>) -> double column named by source PV
    {
        const auto df = findLatestDataFrameForSource(*mock_bus, "test:voltage");
        ASSERT_NE(df, nullptr);
        ASSERT_GT(df->doublecolumns_size(), 0);
        EXPECT_EQ(df->doublecolumns(0).name(), "test:voltage");
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    }

    // test:status (NTScalar<String>) -> string column named by source PV
    {
        const auto df = findLatestDataFrameForSource(*mock_bus, "test:status");
        ASSERT_NE(df, nullptr);
        ASSERT_GT(df->stringcolumns_size(), 0);
        EXPECT_EQ(df->stringcolumns(0).name(), "test:status");
        ASSERT_GT(df->stringcolumns(0).values_size(), 0);
        const auto& s = df->stringcolumns(0).values(0);
        EXPECT_TRUE(s == "OK" || s == "WARNING" || s == "FAULT");
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    }

    // test:waveform (NTScalar<Float64A>) -> double array column named by source PV
    // The waveform is a single sample (array of doubles) at one timestamp,
    // so it maps to a DoubleArrayColumn rather than individual scalar rows.
    {
        const auto df = findLatestDataFrameForSource(*mock_bus, "test:waveform");
        ASSERT_NE(df, nullptr);
        ASSERT_GT(df->doublearraycolumns_size(), 0);
        EXPECT_EQ(df->doublearraycolumns(0).name(), "test:waveform");
        EXPECT_EQ(df->doublearraycolumns(0).values_size(), 256);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    }

    // test:table (NTTable) -> sub-columns "value.deviceIDs" and "value.pressure"
    {
        const auto df = findLatestDataFrameForSource(*mock_bus, "test:table");
        ASSERT_NE(df, nullptr);
        EXPECT_FALSE(hasColumnWithName(*df, "value.labels"));
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));

        const dp::service::common::StringColumn* deviceIDsCol = nullptr;
        const dp::service::common::DoubleColumn* pressureCol = nullptr;
        for (int i = 0; i < df->stringcolumns_size(); ++i)
            if (df->stringcolumns(i).name() == "value.deviceIDs") deviceIDsCol = &df->stringcolumns(i);
        for (int i = 0; i < df->doublecolumns_size(); ++i)
            if (df->doublecolumns(i).name() == "value.pressure") pressureCol = &df->doublecolumns(i);

        ASSERT_NE(deviceIDsCol, nullptr);
        ASSERT_NE(pressureCol, nullptr);
        EXPECT_EQ(deviceIDsCol->values_size(), 3);
        EXPECT_EQ(pressureCol->values_size(), 3);
    }
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
        if (countEventsForSource(*mock_bus, "PV_NAME_A_DOUBLE_VALUE") >= 3 &&
            countEventsForSource(*mock_bus, "PV_NAME_B_STRING_VALUE") >= 3)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }

    EXPECT_GE(countEventsForSource(*mock_bus, "PV_NAME_A_DOUBLE_VALUE"), 3u);
    EXPECT_GE(countEventsForSource(*mock_bus, "PV_NAME_B_STRING_VALUE"), 3u);

    // Verify no timestamp columns leaked as sources
    EXPECT_EQ(countEventsForSource(*mock_bus, "secondsPastEpoch"), 0u);
    EXPECT_EQ(countEventsForSource(*mock_bus, "nanoseconds"), 0u);

    // Collect events for each column across all batches
    std::vector<IDataBus::EventValue> ampl, stat;
    {
        std::lock_guard<std::mutex> lock(mock_bus->mutex);
        for (const auto& batch : mock_bus->received_events)
        {
            if (auto it = batch.values.find("PV_NAME_A_DOUBLE_VALUE"); it != batch.values.end())
            {
                ampl.insert(ampl.end(), it->second.begin(), it->second.end());
            }
            if (auto it = batch.values.find("PV_NAME_B_STRING_VALUE"); it != batch.values.end())
            {
                stat.insert(stat.end(), it->second.begin(), it->second.end());
            }
        }
    }

    ASSERT_GE(ampl.size(), 3u);
    ASSERT_GE(stat.size(), 3u);

    // Per-row timestamps are shared across columns.
    for (size_t i = 0; i < 3; ++i)
    {
        ASSERT_NE(ampl[i], nullptr);
        ASSERT_NE(stat[i], nullptr);
        EXPECT_EQ(ampl[i]->epoch_seconds, stat[i]->epoch_seconds);
        EXPECT_EQ(ampl[i]->nanoseconds, stat[i]->nanoseconds);
        EXPECT_GT(ampl[i]->epoch_seconds, 0u);
    }

    ASSERT_GT(ampl[0]->data_value.doublecolumns_size(), 0);
    ASSERT_GT(ampl[0]->data_value.doublecolumns(0).values_size(), 0);
    EXPECT_DOUBLE_EQ(ampl[0]->data_value.doublecolumns(0).values(0), 1.0);

    ASSERT_GT(stat[0]->data_value.stringcolumns_size(), 0);
    ASSERT_GT(stat[0]->data_value.stringcolumns(0).values_size(), 0);
    EXPECT_EQ(stat[0]->data_value.stringcolumns(0).values(0), "OK");
}
