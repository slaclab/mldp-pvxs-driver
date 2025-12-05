#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>

#include <type_traits>

#include "../config/test_config_helpers.h"

namespace mldp_pvxs_driver::controller {

using config::makeConfigFromYaml;
using util::bus::IEventBusPush;

namespace {

constexpr std::string_view kMinimalControllerConfig = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: test_provider
  url: 127.0.0.1:50051
  min_conn: 1
  max_conn: 1
reader: []
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

    ASSERT_NO_THROW({
        MLDPPVXSController controller(config);
        controller.start();
        controller.stop();
    });
}

} // namespace mldp_pvxs_driver::controller
