#pragma once

#include <functional>
#include <thread>

#include "common.grpc.pb.h"
#include "ingestion.grpc.pb.h"

namespace dp::service::ingestion {

class DpIngestionServiceImpl final : public DpIngestionService::Service {
public:
	explicit DpIngestionServiceImpl(std::function<void(const std::string&)> ingestionCallback);

	grpc::Status registerProvider(grpc::ServerContext* context, const RegisterProviderRequest* request, RegisterProviderResponse* response) override;

	grpc::Status ingestData(grpc::ServerContext* context, const IngestDataRequest* request, IngestDataResponse* response) override;

	grpc::Status ingestDataStream(grpc::ServerContext* context, grpc::ServerReader<IngestDataRequest>* reader, IngestDataStreamResponse* response) override;

	grpc::Status ingestDataBidiStream(grpc::ServerContext* context, grpc::ServerReaderWriter<IngestDataResponse, IngestDataRequest>* stream) override;

	grpc::Status queryRequestStatus(grpc::ServerContext* context, const QueryRequestStatusRequest* request, QueryRequestStatusResponse* response) override;

	grpc::Status subscribeData(grpc::ServerContext* context, grpc::ServerReaderWriter<SubscribeDataResponse, SubscribeDataRequest>* stream) override;

private:
	int m_providerCounter = 0;
	std::function<void(const std::string&)> m_ingestionCallback;
};

} // namespace dp::service::ingestion

class DPIngestionServer {
public:
	explicit DPIngestionServer(const std::string& serverAddress, std::function<void(const std::string&)> ingestionCallback = nullptr);

private:
	std::thread m_updateThread;
};
