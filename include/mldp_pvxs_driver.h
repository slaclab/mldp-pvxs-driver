#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <pvxs/client.h>
#include <pvxs/nt.h>

#include "ingestion.grpc.pb.h"

struct PVXSDPIngestionDriverLogger {
	void(*info)(const std::string& info) = nullptr;
	void(*error)(const std::string& error) = nullptr;
};

class PVXSDPIngestionDriver {
public:
	struct Options {
		const PVXSDPIngestionDriverLogger& logger = {};
		const grpc::StubOptions& grpcOptions = {};
		const pvxs::client::Context& pvaContext = pvxs::client::Context::fromEnv();
	};

	PVXSDPIngestionDriver(std::string providerName, const std::shared_ptr<grpc::Channel>& channel, const std::vector<std::string>& pvNames, const Options& options);

	[[nodiscard]] const std::string& providerID() const;

	[[nodiscard]] const std::string& providerName() const;

	[[nodiscard]] std::string providerDesc() const;

	[[nodiscard]] explicit operator bool() const;

	static void convertPVToProtoValue(const pvxs::Value& pvValue, DataValue* protoValue);

	void ingestPVValue(const std::string& pvName, const pvxs::Value& pvValue);

	void run();

	void stop();

protected:
	void logInfo(const std::string& info) const;

	void logError(const std::string& error) const;

	PVXSDPIngestionDriverLogger m_logger{};

	std::unique_ptr<dp::service::ingestion::DpIngestionService::Stub> m_stub;
	std::string m_providerID;
	std::string m_providerName;
	int m_requestCount = 0;

	pvxs::client::Context m_pvaContext;
	pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pvaSubscriptions;
	pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pvaWorkqueue;

	std::atomic<bool> m_interrupted = false;
};
