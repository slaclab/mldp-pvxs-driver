#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "mldp_pvxs_driver.h"

#define RYML_SINGLE_HDR_DEFINE_NOW
#include "rapidyaml-0.10.0.hpp"

namespace {

enum MLDPPVXSDriverError : int {
	MLDP_PVXS_DRIVER_ERROR_OK,
	MLDP_PVXS_DRIVER_ERROR_UNKNOWN,
	MLDP_PVXS_DRIVER_ERROR_FILE_NOT_FOUND,
	MLDP_PVXS_DRIVER_ERROR_FILE_NOT_READABLE,
	MLDP_PVXS_DRIVER_ERROR_CONFIG_MALFORMED,
};

[[nodiscard]] std::string readFile(const std::string& location, MLDPPVXSDriverError& failure) {
	std::ostringstream contents;
	{
		std::ifstream file{location};
		if (file.fail()) {
			failure = MLDP_PVXS_DRIVER_ERROR_FILE_NOT_FOUND;
			return "";
		}
		file >> contents.rdbuf();
		if (file.fail() && !file.eof()) {
			failure = MLDP_PVXS_DRIVER_ERROR_FILE_NOT_READABLE;
			return "";
		}
	}
	failure = MLDP_PVXS_DRIVER_ERROR_OK;
	return contents.str();
}

PVXSDPIngestionDriverLogger g_logger;

std::unique_ptr<PVXSDPIngestionDriver> g_driver = nullptr;

} // namespace

int main(int argc, char** argv) {
	g_logger.info = [](const std::string& info) { std::cout << info + '\n'; };
	g_logger.error = [](const std::string& error) { std::cerr << error + '\n'; };

	const auto exitHandler = [](int) { if (g_driver) { g_driver->stop(); g_driver.reset(nullptr); } };
	std::signal(SIGINT, exitHandler);
	std::signal(SIGTERM, exitHandler);

	std::string configLocation;
	if (argc < 2) {
		configLocation = std::filesystem::path{argv[0]}.replace_extension(".yml").string();
	} else {
		configLocation = argv[1];
	}

	auto failure = MLDP_PVXS_DRIVER_ERROR_OK;
	const auto readFileWrapper = [&failure](const std::string& name) {
		auto data = ::readFile(name, failure);
		if (failure != MLDP_PVXS_DRIVER_ERROR_OK) {
			g_logger.error("Failed to read file at " + name);
		}
		return data;
	};

	auto configContentsBuf = readFileWrapper(configLocation);
	if (failure != MLDP_PVXS_DRIVER_ERROR_OK) {
		return failure;
	}
	const auto configTree = ryml::parse_in_place(c4::to_substr(configContentsBuf));
	const auto configTreeRoot = configTree.rootref();

	std::string serverAddress;
	if (!configTreeRoot.has_child("server_address")) {
		g_logger.error("No server address set in config.");
		return MLDP_PVXS_DRIVER_ERROR_CONFIG_MALFORMED;
	}
	configTreeRoot["server_address"] >> serverAddress;

	std::shared_ptr<grpc::Channel> channel;
	if (configTreeRoot.has_child("credentials")) {
		if (const auto credentialsTree = configTree["credentials"]; !credentialsTree.is_map()) {
			std::string credentialsType;
			credentialsTree >> credentialsType;
			if (credentialsType == "ssl") {
				channel = grpc::CreateChannel(credentialsType, grpc::SslCredentials({}));
			} else if (credentialsType == "none") {
				channel = grpc::CreateChannel(credentialsType, grpc::InsecureChannelCredentials());
			} else {
				g_logger.error("Invalid value set for credentials in config.");
				return MLDP_PVXS_DRIVER_ERROR_CONFIG_MALFORMED;
			}
		} else {
			grpc::SslCredentialsOptions credentialsOptions;
			if (credentialsTree.has_child("pem_cert_chain")) {
				credentialsTree["pem_cert_chain"] >> credentialsOptions.pem_cert_chain;
				credentialsOptions.pem_cert_chain = readFileWrapper(credentialsOptions.pem_cert_chain);
				if (failure != MLDP_PVXS_DRIVER_ERROR_OK) {
					return failure;
				}
			}
			if (credentialsTree.has_child("pem_root_certs")) {
				credentialsTree["pem_root_certs"] >> credentialsOptions.pem_root_certs;
				credentialsOptions.pem_root_certs = readFileWrapper(credentialsOptions.pem_root_certs);
				if (failure != MLDP_PVXS_DRIVER_ERROR_OK) {
					return failure;
				}
			}
			if (credentialsTree.has_child("pem_private_key")) {
				credentialsTree["pem_private_key"] >> credentialsOptions.pem_private_key;
				credentialsOptions.pem_private_key = readFileWrapper(credentialsOptions.pem_private_key);
				if (failure != MLDP_PVXS_DRIVER_ERROR_OK) {
					return failure;
				}
			}
			channel = grpc::CreateChannel(serverAddress, grpc::SslCredentials(credentialsOptions));
		}
	} else {
		channel = grpc::CreateChannel(serverAddress, grpc::InsecureChannelCredentials());
	}

	std::vector<std::string> pvsToMonitor;
	if (!configTreeRoot.has_child("monitor_pvs")) {
		g_logger.error("No PVs to monitor set in config.");
		return MLDP_PVXS_DRIVER_ERROR_CONFIG_MALFORMED;
	}
	if (const auto pvs = configTreeRoot["monitor_pvs"]; pvs.is_seq()) {
		for (const auto monitoredPV : pvs) {
			std::string pvName;
			monitoredPV >> pvName;
			pvsToMonitor.push_back(pvName);
		}
	} else {
		g_logger.error("Monitor PVs field in config is not a list of values.");
		return MLDP_PVXS_DRIVER_ERROR_CONFIG_MALFORMED;
	}

	std::string providerName;
	if (!configTreeRoot.has_child("provider_name")) {
		g_logger.error("No provider name set in config.");
		return MLDP_PVXS_DRIVER_ERROR_CONFIG_MALFORMED;
	}
	configTreeRoot["provider_name"] >> providerName;

	g_driver = std::make_unique<PVXSDPIngestionDriver>(providerName, channel, pvsToMonitor, PVXSDPIngestionDriver::Options{
		.logger = g_logger,
	});
	if (!g_driver || !*g_driver) {
		g_logger.error("Failed to register provider " + providerName + '!');
		return MLDP_PVXS_DRIVER_ERROR_UNKNOWN;
	}

	g_driver->run(-1);
	return MLDP_PVXS_DRIVER_ERROR_OK;
}
