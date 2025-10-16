#include "gtest/gtest.h"

#include "mldp_pvxs_driver.h"

#include "mock/dp.h"
#include "mock/sioc.h"

TEST(pvxs_mldp_driver, counter) {
	const std::string dpServerAddress{"localhost:8080"};

	DPIngestionServer dp{dpServerAddress, [](const std::string& pvData) {
#if 1
		std::cout << pvData << std::endl;
#endif
	}};
	PVServer pv;

	const auto channel = grpc::CreateChannel(dpServerAddress, grpc::InsecureChannelCredentials());;

	PVXSDPIngestionDriver driver{"test_provider", channel, {
		"test:counter",
		"test:voltage",
		"test:status",
		"test:waveform",
		"test:table",
	}, {}};
	ASSERT_TRUE(driver);
	driver.run();
}
