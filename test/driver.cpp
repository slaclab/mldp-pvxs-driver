#include <gtest/gtest.h>

#include <mldp_pvxs_driver.h>

#include "mock/sioc.h"

TEST(pvxs_mldp_driver, counter) {
	const std::string dpServerAddress{"dp-ingestion:50051"};

	PVServer pv;

	// ReSharper disable once CppTooWideScope
	const std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(dpServerAddress, grpc::InsecureChannelCredentials());

	{
		PVXSDPIngestionDriver driver{"test_provider", channel, {
			"test:counter",
			"test:voltage",
			"test:status",
			"test:waveform",
			"test:table",
		}, {
			.logger = {
				.error = [](const std::string& message) {
					// Will log an error and print the error message
					EXPECT_STREQ(message.c_str(), "");
				},
			},
		}};
		ASSERT_TRUE(driver);
		driver.run(10);
	}
}
