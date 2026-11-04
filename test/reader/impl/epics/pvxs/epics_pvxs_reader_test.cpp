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
using mldp_pvxs_driver::util::bus::DataBatch;
using namespace mldp_pvxs_driver::reader::impl::epics;
using mldp_pvxs_driver::util::bus::asTimeSeries;
using mldp_pvxs_driver::util::bus::getRootSourceName;

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
            const size_t             total_values = asTimeSeries(batch).frames.size();
            const auto               source = getRootSourceName(batch).empty() ? std::string("unknown") : getRootSourceName(batch);
            const prometheus::Labels tags{{"source", source}};
            metrics_->incrementWriterPushes(static_cast<double>(total_values), tags);
            metrics_->incrementWriterPayloadBytes(0.0, tags);
            metrics_->setWriterPayloadBytesPerSecond(0.0, tags);
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
            total += asTimeSeries(batch).frames.size();
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

    // Method to get the last event's DataBatch pointer
    const DataBatch* last_event() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (received_events.empty())
        {
            return nullptr;
        }
        const auto& batch = received_events.back();
        if (asTimeSeries(batch).frames.empty())
        {
            return nullptr;
        }
        return &asTimeSeries(batch).frames.front();
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

std::optional<std::string> frameSource(const DataBatch& batch)
{
    if (!batch.columns.empty())
        return batch.columns[0].name;
    if (!batch.enum_columns.empty())
        return batch.enum_columns[0].name;
    return std::nullopt;
}

const DataBatch* findLatestDataFrameForSource(const MockEventBusPush& bus, const std::string& source)
{
    std::lock_guard<std::mutex> lock(bus.mutex);
    for (auto it = bus.received_events.rbegin(); it != bus.received_events.rend(); ++it)
    {
        const auto& frames = asTimeSeries(*it).frames;
        for (auto fit = frames.rbegin(); fit != frames.rend(); ++fit)
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
        for (const auto& frame : asTimeSeries(batch).frames)
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
                for (const auto& frame : asTimeSeries(batch).frames)
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

TEST_F(EpicsPVXSReaderTest, ReaderDataBytesMetricsIncrementedAfterEvent)
{
    const std::string yaml = R"(
name: epics_1
pvs:
  - name: test:counter
)";
    const auto cfg     = makeConfigFromYaml(yaml);
    auto       metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(
        mldp_pvxs_driver::metrics::MetricsConfig());
    auto reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg, metrics);
    ASSERT_NE(reader_ptr, nullptr);

    const prometheus::Labels source_tag{{"source", "test:counter"}};
    const int                max_wait_ms = 5000;
    int                      waited_ms   = 0;
    bool                     got_bytes   = false;
    while (waited_ms < max_wait_ms)
    {
        if (metrics->readerDataBytesTotal(source_tag) > 0.0)
        {
            got_bytes = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }

    EXPECT_TRUE(got_bytes) << "readerDataBytesTotal never incremented for test:counter";
    EXPECT_GT(metrics->readerDataBytesTotal(source_tag), 0.0);
    EXPECT_GE(metrics->readerDataBytesPerSecond(source_tag), 0.0);
}

// Verify reader-level metadata and per-PV metadata overrides are merged into EventBatch.metadata.
// Reader config uses YAML key "metadata" for both reader-level and per-PV blocks.
// Per-PV keys win over reader-level keys on conflict.
TEST_F(EpicsPVXSReaderTest, StaticAndPerPvMetadataMergedIntoEventBatch)
{
    const std::string yaml = R"(
name: epics_meta_reader
metadata:
  facility: lcls
  subsystem: bpms
pvs:
  - name: test:counter
    metadata:
      signal_type: scalar
      subsystem: override_bpms
)";
    const auto cfg     = makeConfigFromYaml(yaml);
    auto       reader  = mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg);
    ASSERT_NE(reader, nullptr);

    const int max_wait_ms = 5000;
    int       waited_ms   = 0;
    while (mock_bus->event_count() == 0 && waited_ms < max_wait_ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }
    ASSERT_GT(mock_bus->event_count(), 0u) << "No events received within timeout";

    // All batches for test:counter must carry the merged metadata map.
    std::lock_guard<std::mutex> lock(mock_bus->mutex);
    bool found = false;
    for (const auto& batch : mock_bus->received_events)
    {
        if (getRootSourceName(batch) != "test:counter")
            continue;
        found = true;
        // Reader-level key not overridden by per-PV.
        EXPECT_EQ(batch.metadata.count("facility"), 1u);
        EXPECT_EQ(batch.metadata.at("facility"), "lcls");
        // Per-PV key not present at reader level.
        EXPECT_EQ(batch.metadata.count("signal_type"), 1u);
        EXPECT_EQ(batch.metadata.at("signal_type"), "scalar");
        // Per-PV key overrides reader-level key with same name.
        EXPECT_EQ(batch.metadata.count("subsystem"), 1u);
        EXPECT_EQ(batch.metadata.at("subsystem"), "override_bpms");
    }
    EXPECT_TRUE(found) << "No batch with root_source=test:counter received";
}

// BSAS NTTables moved to the epics-pvxs-bsas-gen1 / -gen2 readers
// (see epics_pvxs_bsas_gen1_reader_test.cpp / epics_pvxs_bsas_gen2_reader_test.cpp);
// the scalar reader now rejects them at configuration time.
TEST_F(EpicsPVXSReaderTest, SlacBsasTableOptionIsRejected)
{
    const auto cfg = makeConfigFromYaml(R"(
name: epics_1
pvs:
  - name: test:bsas_table
    option:
      type: slac-bsas-table
)");
    try
    {
        mldp_pvxs_driver::reader::ReaderFactory::create("epics-pvxs", mock_bus, cfg);
        FAIL() << "slac-bsas-table under epics-pvxs must be rejected";
    }
    catch (const EpicsReaderConfig::Error& e)
    {
        EXPECT_NE(std::string(e.what()).find("epics-pvxs-bsas-gen1"), std::string::npos) << e.what();
    }
}
