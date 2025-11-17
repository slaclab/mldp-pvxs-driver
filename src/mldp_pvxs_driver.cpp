// ReSharper disable CppParameterMayBeConst

#include "mldp_pvxs_driver.h"

#include <chrono>
#include <thread>

#include "BS_thread_pool.hpp"
#include "common.grpc.pb.h"

using namespace std::chrono_literals;

PVXSDPIngestionDriver::PVXSDPIngestionDriver(std::string providerName, const std::shared_ptr<grpc::Channel>& channel, const std::vector<std::string>& pvNames, const Options& options)
	: m_logger{options.logger}
	, m_stub{dp::service::ingestion::DpIngestionService::NewStub(channel, options.grpcOptions)}
	, m_providerName{std::move(providerName)}
	, m_pvaContext{options.pvaContext}
{
	dp::service::ingestion::RegisterProviderRequest request;
	request.set_providername(m_providerName);
	request.set_description(providerDesc());

	grpc::ClientContext context;
	dp::service::ingestion::RegisterProviderResponse response;
	if (const auto status = m_stub->registerProvider(&context, request, &response); !status.ok()) {
		logError("gRPC call failed: " + status.error_message());
		return;
	}
	if (response.has_registrationresult()) {
		m_providerID = response.registrationresult().providerid();
		logInfo("Registered ingestion provider " + m_providerName + " with ID " + m_providerID);
	} else if (response.has_exceptionalresult()) {
		logError("Registration of " + m_providerName + " ingestion provider failed: " + response.exceptionalresult().message());
		return;
	} else {
		// Should never be here
		logError("Registration of " + m_providerName + " ingestion provider failed: no response!");
		return;
	}

	for (const auto& pv : pvNames) {
		m_pvaSubscriptions.push(m_pvaContext.monitor(pv)
			.event([this](const pvxs::client::Subscription& s) {
				m_pvaWorkqueue.push(s.shared_from_this());
			})
			.exec());
	}
}

const std::string& PVXSDPIngestionDriver::providerID() const {
	return m_providerID;
}

const std::string& PVXSDPIngestionDriver::providerName() const {
	return m_providerName;
}

std::string PVXSDPIngestionDriver::providerDesc() const {
	return m_providerName + " PVXS DP Ingestion Driver";
}

PVXSDPIngestionDriver::operator bool() const {
	return m_stub && !m_providerID.empty();
}

void PVXSDPIngestionDriver::convertPVToProtoValue(const pvxs::Value& pvValue, DataValue* protoValue) {
	const auto typeArraySetter = [protoValue](const auto& pvArray, auto setter) {
		auto* protoArray = protoValue->mutable_arrayvalue();
		for (const auto& i : pvArray) {
			auto* element = protoArray->add_datavalues();
			(*element.*setter)(i);
		}
	};
	const auto structSetter = [](const pvxs::Value& structValue, DataValue* protoStruct) {
		auto* structure = protoStruct->mutable_structurevalue();
		for (const auto& member : structValue.ichildren()) {
			if (!member.valid()) {
				continue;
			}
			auto* field = structure->add_fields();
			field->set_name(structValue.nameOf(member));
			convertPVToProtoValue(member, field->mutable_value());
		}
	};

	switch (pvValue.type().code) {
		case pvxs::TypeCode::Bool: protoValue->set_booleanvalue(pvValue.as<bool>()); break;
		case pvxs::TypeCode::BoolA: typeArraySetter(pvValue.as<pvxs::shared_array<const bool>>(), &DataValue::set_booleanvalue); break;
		case pvxs::TypeCode::Int8:
		case pvxs::TypeCode::Int16:
		case pvxs::TypeCode::Int32: protoValue->set_intvalue(pvValue.as<int32_t>()); break;
		case pvxs::TypeCode::Int64: protoValue->set_longvalue(pvValue.as<int64_t>()); break;
		case pvxs::TypeCode::UInt8:
		case pvxs::TypeCode::UInt16:
		case pvxs::TypeCode::UInt32: protoValue->set_uintvalue(pvValue.as<uint32_t>()); break;
		case pvxs::TypeCode::UInt64: protoValue->set_ulongvalue(pvValue.as<uint64_t>()); break;
		case pvxs::TypeCode::Int8A:
		case pvxs::TypeCode::Int16A:
		case pvxs::TypeCode::Int32A: typeArraySetter(pvValue.as<pvxs::shared_array<const int32_t>>(), &DataValue::set_intvalue); break;
		case pvxs::TypeCode::Int64A: typeArraySetter(pvValue.as<pvxs::shared_array<const int64_t>>(), &DataValue::set_longvalue); break;
		case pvxs::TypeCode::UInt8A:
		case pvxs::TypeCode::UInt16A:
		case pvxs::TypeCode::UInt32A: typeArraySetter(pvValue.as<pvxs::shared_array<const uint32_t>>(), &DataValue::set_uintvalue); break;
		case pvxs::TypeCode::UInt64A: typeArraySetter(pvValue.as<pvxs::shared_array<const uint64_t>>(), &DataValue::set_ulongvalue); break;
		case pvxs::TypeCode::Float32: protoValue->set_floatvalue(pvValue.as<float>()); break;
		case pvxs::TypeCode::Float64: protoValue->set_doublevalue(pvValue.as<double>()); break;
		case pvxs::TypeCode::Float32A: typeArraySetter(pvValue.as<pvxs::shared_array<const float>>(), &DataValue::set_floatvalue); break;
		case pvxs::TypeCode::Float64A: typeArraySetter(pvValue.as<pvxs::shared_array<const double>>(), &DataValue::set_doublevalue); break;
		case pvxs::TypeCode::String: protoValue->set_stringvalue(pvValue.as<std::string>()); break;
		case pvxs::TypeCode::StringA: typeArraySetter(pvValue.as<pvxs::shared_array<const std::string>>(), static_cast<void(DataValue::*)(const std::string&)>(&DataValue::set_stringvalue)); break;
		case pvxs::TypeCode::Struct:
		case pvxs::TypeCode::Union:
		case pvxs::TypeCode::Any: structSetter(pvValue, protoValue); break;
		case pvxs::TypeCode::StructA:
		case pvxs::TypeCode::UnionA:
		case pvxs::TypeCode::AnyA: {
			const auto pvArray = pvValue.as<pvxs::shared_array<const pvxs::Value>>();
			for (const auto& i : pvArray) {
				structSetter(i, protoValue->mutable_arrayvalue()->add_datavalues());
			}
			break;
		}
		case pvxs::TypeCode::Null: protoValue->set_stringvalue("null"); break;
	}
}

void PVXSDPIngestionDriver::ingestPVValue(const std::string& pvName, const pvxs::Value& pvValue, int currentRetryCount) {
	dp::service::ingestion::IngestDataRequest request;
	request.set_providerid(m_providerID);
	request.set_clientrequestid("pv_" + pvName + "_" + std::to_string(m_requestCount++));
	request.add_tags(pvName);

	auto* dataFrame = request.mutable_ingestiondataframe();
	auto* timestamps = dataFrame->mutable_datatimestamps();
	auto* timestampList = timestamps->mutable_timestamplist();
	auto* ts = timestampList->add_timestamps();

	if (pvValue.type().kind() == pvxs::Kind::Compound) {
		bool setEpoch = false;
		if (const auto timestampField = pvValue["timeStamp"]; timestampField.valid()) {
			if (const auto secondsField = timestampField["secondsPastEpoch"]; secondsField.valid()) {
				ts->set_epochseconds(secondsField.as<uint64_t>());
				setEpoch = true;
			}
			if (const auto nanosecondsField = timestampField["nanoseconds"]; nanosecondsField.valid()) {
				ts->set_nanoseconds(nanosecondsField.as<uint64_t>());
			}
		}
		if (!setEpoch) {
			// Fallback to make sure timestamp is always set
			const auto now = std::chrono::system_clock::now().time_since_epoch();
			ts->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(now).count());
		}
	}

	auto* column = dataFrame->add_datacolumns();
	column->set_name(pvName);

	auto* dataValue = column->add_datavalues();
	try {
		convertPVToProtoValue(pvValue, dataValue);
	} catch (const std::exception& e) {
		logError("Could not convert PV with name " + pvName + " to a proto value: " + e.what());
		return;
	}

	grpc::ClientContext context;
	dp::service::ingestion::IngestDataResponse response;
	if (const auto status = m_stub->ingestData(&context, request, &response); !status.ok()) {
		// We are on our own thread, and it's OK to do this.
		// We have to recreate the data frame, so it's easier to call this function again.
		static constexpr int MAX_RETRIES = 3;
		if (currentRetryCount < MAX_RETRIES) {
			ingestPVValue(pvName, pvValue, currentRetryCount + 1);
		} else {
			logError("Ingestion failed for " + pvName + ": " + status.error_message());
		}
	}
}

void PVXSDPIngestionDriver::run(int timeout) {
	const auto start = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch());
	while (!m_interrupted) {
		auto sub = m_pvaWorkqueue.pop();
		try {
			pvxs::Value update = sub->pop();
			if (!update) {
				continue;
			}
			static BS::light_thread_pool pool;
			pool.detach_task([this, name = std::string{sub->name()}, update_ = std::move(update)] {
				ingestPVValue(name, update_);
			});
		} catch (const pvxs::client::RemoteError& e) {
			logError("Server error when reading PV " + sub->name() + ':' + e.what());
		}
		m_pvaWorkqueue.push(sub);

		if (timeout > 0) {
			if (
				const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()) - start;
				elapsed.count() > timeout
			) {
				m_interrupted = true;
			}
		}
	}
	m_interrupted = false;
}

void PVXSDPIngestionDriver::stop() {
	m_interrupted = true;
}

void PVXSDPIngestionDriver::logInfo(const std::string& info) const {
	if (m_logger.info) {
		// If this is using std::cout, it is already thread-safe
		m_logger.info(info);
	}
}

void PVXSDPIngestionDriver::logError(const std::string& error) const {
	if (m_logger.error) {
		// If this is using std::cerr, it is already thread-safe
		m_logger.error(error);
	}
}
