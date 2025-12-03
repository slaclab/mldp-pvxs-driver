#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

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
    std::vector<std::string> received_events;

    bool push(/* Event evt */) override
    {
        // Since the actual event type is unspecified, we'll use a string representation
        // In a real implementation, this would handle the actual event type
        received_events.push_back("event_data");
        return true;
    }

    // Method to get the number of events received
    size_t event_count() const
    {
        return received_events.size();
    }

    // Method to get the last event received
    std::string last_event() const
    {
        if (received_events.empty())
        {
            return std::string{};
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

        // Add the required PVs as specified
        std::vector<std::string> pv_names = {
            "test:counter",
            "test:voltage",
            "test:status",
            "test:waveform",
            "test:table"};

        // Configure test_config with PVs
        // This would involve setting up the configuration to include these PVs
        // Since we don't have the exact Config API, we'll assume it can be configured with PV names
        // This is a placeholder - actual implementation would depend on Config's interface

        // Create mock bus
        mock_bus = std::make_shared<MockEventBusPush>();
    }
    // EpicsReader instance under test
    std::shared_ptr<MockEventBusPush> mock_bus;
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
    EXPECT_EQ(reader_ptr->name(), "epics");
}

// Test addPV method
TEST_F(EpicsReaderTest, AddPV)
{
    // This test would need to verify that PVs are properly added
    // Since addPV is called internally during construction, we need to verify
    // the PVs were registered with the PVA context

    // For now, we'll verify that the reader has the expected PVs
    // This would require adding a getter method to EpicsReader or using mocks

    // Placeholder test - actual implementation would need access to internal state
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
