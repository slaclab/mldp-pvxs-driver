#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "../../../mock/sioc.h"
#include "../../../config/test_epics_reader_config_helpers.h"

#include <config/Config.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics/EpicsReader.h>
#include <util/bus/IEventBusPush.h>

namespace mldp_pvxs_driver::reader::impl::epics {

// Concrete mock implementation of IEventBusPush for testing
class MockEventBusPush : public mldp_pvxs_driver::util::bus::IEventBusPush
{
public:
    // Store received events for verification
    std::vector<MockEventBusPush::EventValue> received_events;

    bool push(MockEventBusPush::EventValue data_value) override
    {
        // Since the actual event type is unspecified, we'll use a string representation
        // In a real implementation, this would handle the actual event type
        received_events.push_back(data_value);
        return true;
    }

    // Method to get the number of events received
    size_t event_count() const
    {
        return received_events.size();
    }

    // Method to get the last event received
    std::shared_ptr<DataValue> last_event() const
    {
        if (received_events.empty())
        {
            return nullptr;
        }
        return received_events.back();
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
    // reader = std::dynamic_pointer_cast<mldp_pvxs_driver::reader::impl::epics::EpicsReader>(std::shared_ptr<mldp_pvxs_driver::reader::Reader>(reader_ptr));

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

// Test run method with timeout
TEST_F(EpicsReaderTest, RunWithTimeout)
{
    // This test would need to verify that the run method processes updates
    // and respects timeout parameter

    // Verify that run method returned within timeout and processed updates
}

// Test destructor ensures proper cleanup
TEST_F(EpicsReaderTest, DestructorCleansUp)
{
    // Verify worker thread is properly joined
    // This would require checking internal state or using mocks

    // Verify cleanup occurred - this would need to verify that running_ is false
    // and worker thread has been joined
}

} // namespace mldp_pvxs_driver::reader::impl::epics
