#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include "../../../mock/sioc.h"
#include "../../../config/test_config_helpers.h"

#include <config/Config.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics/EpicsReader.h>
#include <util/bus/IEventBusPush.h>

namespace mldp_pvxs_driver::reader::impl::epics {

// Concrete mock implementation of IEventBusPush for testing
class MockEventBusPush : public mldp_pvxs_driver::util::bus::IEventBusPush
{
public:
    using EventBatch = mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch;

    // Store received events for verification
    std::vector<EventBatch> received_events;

    bool push(EventBatch batch) override
    {
        received_events.push_back(std::move(batch));
        return true;
    }

    // Method to get the number of events received
    size_t event_count() const
    {
        return received_events.size();
    }

    size_t total_value_count() const
    {
        size_t total = 0;
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
        if (received_events.empty())
        {
            return nullptr;
        }
        return &received_events.back();
    }

    // Method to get the last event's DataValue pointer
    std::shared_ptr<DataValue> last_event() const
    {
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
        return first_entry.second.front()->data_value;
    }

    // Method to clear events
    void clear_events()
    {
        received_events.clear();
    }
};

// Test fixture
class EpicsReaderTest : public ::testing::Test
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
    std::shared_ptr<PVServer> mock_pv_server;
};

// Test constructor and name through factory
TEST_F(EpicsReaderTest, ConstructorAndName)
{
    const std::string yaml = R"(
name: epics_1
)";

    const auto cfg = config::makeConfigFromYaml(yaml);

    // Use ReaderFactory to create EpicsReader instance (as shown in test/driver.cpp)
    auto reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics", mock_bus, cfg);
    // reader = std::dynamic_pointer_cast<mldp_pvxs_driver::reader::impl::epics::EpicsReader>(std::shared_ptr<mldp_pvxs_driver::reader::Reader>(reader_ptr));

    ASSERT_NE(reader_ptr, nullptr);

    // Verify name is set correctly (assuming default or configurable name)
    EXPECT_EQ(reader_ptr->name(), "epics_1");
}

// Test addPV method
TEST_F(EpicsReaderTest, MonitorPVOnConfiguration)
{
        const std::string yaml = R"(
name: epics_1
pvs:
  - name: test:counter
)";
    const auto cfg = config::makeConfigFromYaml(yaml);

    // Use ReaderFactory to create EpicsReader instance (as shown in test/driver.cpp)
    auto reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics", mock_bus, cfg);
 
    // check if is not null
    ASSERT_NE(reader_ptr, nullptr);

    // Verify name is set correctly (assuming default or configurable name)
    EXPECT_EQ(reader_ptr->name(), "epics_1");

    // wiat for the reception of the first event
    const int max_wait_ms = 5000;
    int       waited_ms  = 0;
    while (mock_bus->event_count() == 0 && waited_ms < max_wait_ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }   
    EXPECT_GT(mock_bus->event_count(), 0) << "No events received within timeout";
}

TEST_F(EpicsReaderTest, NTTableRowTimestampSplitsToPerColumnSources)
{
    const std::string yaml = R"(
name: epics_1
pvs:
  - name: test:bsas_table
    option:
      type: nttable-rowts
)";

    const auto cfg = config::makeConfigFromYaml(yaml);
    auto       reader_ptr = mldp_pvxs_driver::reader::ReaderFactory::create("epics", mock_bus, cfg);
    ASSERT_NE(reader_ptr, nullptr);

    const int max_wait_ms = 5000;
    int       waited_ms = 0;
    while (mock_bus->event_count() == 0 && waited_ms < max_wait_ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waited_ms += 100;
    }

    const auto* batch = mock_bus->last_batch();
    ASSERT_NE(batch, nullptr);
    ASSERT_FALSE(batch->values.empty());

    ASSERT_TRUE(batch->values.count("PV_NAME_A_DOUBLE_VALUE")) << "Expected AMPL column as a source";
    ASSERT_TRUE(batch->values.count("PV_NAME_B_STRING_VALUE")) << "Expected STAT column as a source";
    EXPECT_FALSE(batch->values.count("secondsPastEpoch"));
    EXPECT_FALSE(batch->values.count("nanoseconds"));

    const auto& ampl = batch->values.at("PV_NAME_A_DOUBLE_VALUE");
    const auto& stat = batch->values.at("PV_NAME_B_STRING_VALUE");
    ASSERT_EQ(ampl.size(), 3u);
    ASSERT_EQ(stat.size(), 3u);

    // Per-row timestamps are shared across columns.
    for (size_t i = 0; i < 3; ++i)
    {
        ASSERT_NE(ampl[i], nullptr);
        ASSERT_NE(stat[i], nullptr);
        EXPECT_EQ(ampl[i]->epoch_seconds, stat[i]->epoch_seconds);
        EXPECT_EQ(ampl[i]->nanoseconds, stat[i]->nanoseconds);
        EXPECT_GT(ampl[i]->epoch_seconds, 0u);
    }

    ASSERT_NE(ampl[0]->data_value, nullptr);
    ASSERT_TRUE(ampl[0]->data_value->has_doublevalue());
    EXPECT_DOUBLE_EQ(ampl[0]->data_value->doublevalue(), 1.0);

    ASSERT_NE(stat[0]->data_value, nullptr);
    ASSERT_TRUE(stat[0]->data_value->has_stringvalue());
    EXPECT_EQ(stat[0]->data_value->stringvalue(), "OK");
}

} // namespace mldp_pvxs_driver::reader::impl::epics
