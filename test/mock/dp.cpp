#include "dp.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/server_builder.h>

using namespace dp::service::ingestion;

DpIngestionServiceImpl::DpIngestionServiceImpl(std::function<void(const std::string&)> ingestionCallback)
	: m_ingestionCallback(std::move(ingestionCallback)) {}

grpc::Status DpIngestionServiceImpl::registerProvider(grpc::ServerContext* context, const RegisterProviderRequest* request, RegisterProviderResponse* response) {
	std::cout << "[DP] registerProvider for provider: " << request->providername() << std::endl;

	auto* result = response->mutable_registrationresult();
	result->set_providerid("test-provider-" + std::to_string(++m_providerCounter));
	result->set_isnewprovider(true);
	result->set_providername(request->providername());

	std::cout << "[DP] Assigned provider ID: " << result->providerid() << std::endl;
	return grpc::Status::OK;
}

grpc::Status DpIngestionServiceImpl::ingestData(grpc::ServerContext* context, const IngestDataRequest* request, IngestDataResponse* response) {
	std::cout << "[DP] ingestData - Provider ID: " << request->providerid() << ", Request ID: " << request->clientrequestid() << std::endl;

	// Simulate failed requests
	if (rand() % 5 == 0) {
		return grpc::Status::CANCELLED;
	}

	response->set_providerid(request->providerid());
	response->set_clientrequestid(request->clientrequestid());

	m_ingestionCallback(request->ingestiondataframe().DebugString());
	return grpc::Status::OK;
}

grpc::Status DpIngestionServiceImpl::ingestDataStream(grpc::ServerContext* context, grpc::ServerReader<IngestDataRequest>* reader, IngestDataStreamResponse* response) {
	std::cout << "[DP] ingestDataStream - receiving stream..." << std::endl;

	IngestDataRequest request;
	int count = 0;
	while (reader->Read(&request)) {
		count++;
		std::cout << "[DP] Received request #" << count << " - Provider: " << request.providerid() << ", Request ID: " << request.clientrequestid() << std::endl;
	}

	std::cout << "[DP] Stream complete - processed " << count << " requests" << std::endl;
	return grpc::Status::OK;
}

grpc::Status DpIngestionServiceImpl::ingestDataBidiStream(grpc::ServerContext* context, grpc::ServerReaderWriter<IngestDataResponse, IngestDataRequest>* stream) {
	std::cout << "[DP] ingestDataBidiStream - bidirectional streaming active" << std::endl;

	IngestDataRequest request;
	int count = 0;
	while (stream->Read(&request)) {
		count++;
		std::cout << "[DP] Processing request #" << count << " - Provider: " << request.providerid() << std::endl;

		IngestDataResponse response;
		response.set_providerid(request.providerid());
		response.set_clientrequestid(request.clientrequestid());

		if (!stream->Write(response)) {
			std::cerr << "[DP] Failed to write response" << std::endl;
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	std::cout << "[DP] Bidirectional stream complete - processed " << count << " requests" << std::endl;
	return grpc::Status::OK;
}

grpc::Status DpIngestionServiceImpl::queryRequestStatus(grpc::ServerContext* context, const QueryRequestStatusRequest* request, QueryRequestStatusResponse* response) {
	// todo(mock): queries (not currently necessary)
	return grpc::Status::OK;
}

grpc::Status DpIngestionServiceImpl::subscribeData(grpc::ServerContext* context, grpc::ServerReaderWriter<SubscribeDataResponse, SubscribeDataRequest>* stream) {
	// todo(mock): subscriptions (not currently necessary)
	return grpc::Status::OK;
}

DPIngestionServer::DPIngestionServer(const std::string& serverAddress, std::function<void(const std::string&)> ingestionCallback) {
	m_updateThread = std::thread([serverAddress, ingestionCallback_ = std::move(ingestionCallback)] {
		DpIngestionServiceImpl service{std::move(ingestionCallback_)};

		grpc::ServerBuilder builder;
		builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
		builder.RegisterService(&service);

		const std::unique_ptr server{builder.BuildAndStart()};
		server->Wait();
	});
}
