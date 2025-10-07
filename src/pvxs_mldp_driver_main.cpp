#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "pvxs_mldp_driver.h"

#define RYML_SINGLE_HDR_DEFINE_NOW
#include "rapidyaml-0.10.0.hpp"

namespace {

enum Failure {
	FAIL_OK,
	FAIL_UNKNOWN,
	FAIL_FILE_NOT_FOUND,
	FAIL_FILE_NOT_READABLE,
	FAIL_CONFIG_MALFORMED,
};

[[nodiscard]] std::string readFile(const std::string& location, Failure& failure) {
	std::ostringstream contents;
	{
		std::ifstream file{location};
		if (file.fail()) {
			failure = FAIL_FILE_NOT_FOUND;
			return "";
		}
		file >> contents.rdbuf();
		if (file.fail() && !file.eof()) {
			failure = FAIL_FILE_NOT_READABLE;
			return "";
		}
	}
	failure = FAIL_OK;
	return contents.str();
}

PVXSDPIngestionDriverLogger g_logger{
	.error = [](const std::string& error) { std::cerr << error + '\n'; },
};

std::unique_ptr<PVXSDPIngestionDriver> g_driver = nullptr;

} // namespace

int main(int argc, char** argv) {
	const auto exitHandler = [](int) { if (g_driver) g_driver->stop(); };
	std::signal(SIGINT, exitHandler);
	std::signal(SIGTERM, exitHandler);

	if (argc < 1) [[unlikely]] {
		return FAIL_UNKNOWN;
	}

	std::string configLocation;
	if (argc < 2) {
		configLocation = std::filesystem::path{argv[0]}.replace_extension().string();
	} else {
		configLocation = argv[1];
	}

	Failure failure = FAIL_OK;
	#define READ_FILE_OR_FAIL(name) \
		::readFile(name, failure); \
		if (failure != FAIL_OK) \
			return failure

	std::string configContentsBuf = READ_FILE_OR_FAIL(configLocation);
	const auto configTree = ryml::parse_in_place(c4::to_substr(configContentsBuf));
	const auto configTreeRoot = configTree.rootref();

	std::string serverAddress;
	if (!configTreeRoot.has_child("server_address")) {
		return FAIL_CONFIG_MALFORMED;
	}
	configTreeRoot["server_address"] >> serverAddress;

	std::shared_ptr<grpc::Channel> channel;
	if (configTreeRoot.has_child("credentials")) {
		const auto credentialsTree = configTree["credentials"];
		if (!credentialsTree.is_map()) {
			std::string credentialsType;
			credentialsTree >> credentialsType;
			if (credentialsType == "ssl") {
				channel = grpc::CreateChannel(credentialsType, grpc::SslCredentials({}));
			} else if (credentialsType == "none") {
				channel = grpc::CreateChannel(credentialsType, grpc::InsecureChannelCredentials());
			} else {
				return FAIL_CONFIG_MALFORMED;
			}
		} else {
			grpc::SslCredentialsOptions credentialsOptions;
			if (credentialsTree.has_child("pem_cert_chain")) {
				credentialsTree["pem_cert_chain"] >> credentialsOptions.pem_cert_chain;
				credentialsOptions.pem_cert_chain = READ_FILE_OR_FAIL(credentialsOptions.pem_cert_chain);
			}
			if (credentialsTree.has_child("pem_root_certs")) {
				credentialsTree["pem_root_certs"] >> credentialsOptions.pem_root_certs;
				credentialsOptions.pem_root_certs = READ_FILE_OR_FAIL(credentialsOptions.pem_root_certs);
			}
			if (credentialsTree.has_child("pem_private_key")) {
				credentialsTree["pem_private_key"] >> credentialsOptions.pem_private_key;
				credentialsOptions.pem_private_key = READ_FILE_OR_FAIL(credentialsOptions.pem_private_key);
			}
			channel = grpc::CreateChannel(serverAddress, grpc::SslCredentials(credentialsOptions));
		}
	} else {
		channel = grpc::CreateChannel(serverAddress, grpc::InsecureChannelCredentials());
	}

	std::vector<std::string> pvsToMonitor;
	if (!configTreeRoot.has_child("monitor_pvs")) {
		return FAIL_CONFIG_MALFORMED;
	}
	if (const auto pvs = configTreeRoot["monitor_pvs"]; pvs.is_seq()) {
		for (const auto monitoredPV : pvs) {
			std::string pvName;
			monitoredPV >> pvName;
			pvsToMonitor.push_back(pvName);
		}
	} else {
		return FAIL_CONFIG_MALFORMED;
	}

	std::string providerName;
	if (!configTreeRoot.has_child("provider_name")) {
		return FAIL_CONFIG_MALFORMED;
	}
	configTreeRoot["provider_name"] >> providerName;

	g_driver = std::make_unique<PVXSDPIngestionDriver>(providerName, channel, pvsToMonitor);
	if (!g_driver || !*g_driver) {
		g_logger.error("Failed to register provider " + providerName + '!');
		return FAIL_UNKNOWN;
	}
	g_driver->setLogger(g_logger);

	g_driver->run();
	return FAIL_OK;
}
