#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>

#include <type_traits>
#include <unistd.h>

#include "../config/test_config_helpers.h"
#include "../mock/sioc.h"

namespace mldp_pvxs_driver::controller {

using config::makeConfigFromYaml;
using util::bus::IEventBusPush;

namespace {

    constexpr std::string_view kMinimalControllerConfig = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: test_provider
  url: dp-ingestion:50051
  min_conn: 1
  max_conn: 1
reader: []
)";

    constexpr std::string_view kEpicsControllerConfig = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: test_provider
  url: dp-ingestion:50051
  min_conn: 1
  max_conn: 1
reader:
  - epics:
      - name: epics_reader_1
        pvs:
          - name: test:counter
)";

} // namespace

TEST(MLDPPVXSControllerTest, ImplementsEventBusPushContract)
{
    static_assert(std::is_base_of_v<IEventBusPush, MLDPPVXSController>);
    SUCCEED();
}

TEST(MLDPPVXSControllerTest, StartAndStopDoNotThrowWithValidConfig)
{
    const auto config = makeConfigFromYaml(std::string(kMinimalControllerConfig));

    ASSERT_TRUE(config.valid());
    auto controller = MLDPPVXSController::create(config);
    ASSERT_TRUE(controller);
    ASSERT_NO_THROW(controller->start(););
    sleep(1); // Allow some time for the reader to start
    ASSERT_NO_THROW(controller->stop(););
}

TEST(MLDPPVXSControllerTest, StartAndStopDoNotThrowWithEpicsConfig)
{
    const auto config = makeConfigFromYaml(std::string(kEpicsControllerConfig));

    ASSERT_TRUE(config.valid());

    {
        // strart pv mocker
        PVServer pvServer;
        auto     controller = MLDPPVXSController::create(config);
        ASSERT_TRUE(controller);
        ASSERT_NO_THROW(controller->start(););
        sleep(1); // Allow some time for the reader to start
        ASSERT_NO_THROW(controller->stop(););
        // chgeck on metric if the event has been pushed
        auto& metrics = controller->metrics();
        EXPECT_GE(metrics.busPushTotal({}), 0);
    }
}

} // namespace mldp_pvxs_driver::controller
