// ReSharper disable CppParameterMayBeConst

#include "mldp_pvxs_driver.h"

#include <chrono>
#include <iostream>
#include <thread>

#include "common.grpc.pb.h"

using namespace std::chrono_literals;

PVXSDPIngestionDriver::PVXSDPIngestionDriver(std::string providerName, const std::shared_ptr<grpc::Channel>& channel, const std::vector<std::string>& pvNames, const grpc::StubOptions& options, const pvxs::client::Context& pvaContext)
	: m_logger{}
	, m_stub{dp::service::ingestion::DpIngestionService::NewStub(channel, options)}
	, m_providerName{std::move(providerName)}
	, m_requestCount{0}
	, m_pvaContext{pvaContext}
	, m_interrupted{false}
{
	if (const auto registerProvider = [this] {
		dp::service::ingestion::RegisterProviderRequest request;
		request.set_providername(m_providerName);
		request.set_description(providerDesc());

		grpc::ClientContext context;
		dp::service::ingestion::RegisterProviderResponse response;
		if (const auto status = m_stub->registerProvider(&context, request, &response); status.ok()) {
			if (response.has_registrationresult()) {
				m_providerID = response.registrationresult().providerid();
				std::cout << "Registered ingestion provider " << m_providerName << " with ID " << m_providerID << std::endl;
				return true;
			}
			if (response.has_exceptionalresult()) {
				std::cerr << "Registration of " << m_providerName << " ingestion provider failed: " << response.exceptionalresult().message() << std::endl;
				return false;
			}
		} else {
			std::cerr << "gRPC call failed: " << status.error_message() << std::endl;
			return false;
		}
		return false;
	}; !registerProvider()) {
		m_stub.reset();
		m_providerID = "";
	} else {
		for (const auto& pv : pvNames) {
			m_pvaSubscriptions.push(m_pvaContext.monitor(pv)
				.event([this](pvxs::client::Subscription& s) {
					m_pvaWorkqueue.push(s.shared_from_this());
				})
				.exec());
		}
	}
}

void PVXSDPIngestionDriver::setLogger(const PVXSDPIngestionDriverLogger& logger) {
	m_logger = logger;
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

bool PVXSDPIngestionDriver::convertPVToProtoValue(const pvxs::Value& pvValue, DataValue* protoValue) {
	try {
		const auto typeArraySetter = [protoValue](const auto& pvArray, auto setter) {
			auto* protoArray = protoValue->mutable_arrayvalue();
			for (const auto& i : pvArray) {
				auto* element = protoArray->add_datavalues();
				(*element.*setter)(i);
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
			case pvxs::TypeCode::Union: {
				auto* structure = protoValue->mutable_structurevalue();
				for (const auto& member : pvValue.ichildren()) {
					if (!member.valid()) {
						continue;
					}
					auto* field = structure->add_fields();
					field->set_name(pvValue.nameOf(member));
					if (!convertPVToProtoValue(member, field->mutable_value())) {
						return false;
					}
				}
				break;
			}
			case pvxs::TypeCode::Any:
				// todo(dp): pvxs Any value
				break;
			case pvxs::TypeCode::StructA:
			case pvxs::TypeCode::UnionA: {
				// todo(dp): pvxs array of structs
				break;
			}
			case pvxs::TypeCode::AnyA:
				// todo(dp): pvxs array of Any
				break;
			case pvxs::TypeCode::Null:
				// todo(dp): is there something else we can set? check proto
				protoValue->set_stringvalue("null");
				break;
		}

		return true;
	} catch (const std::exception&) {
		// This will be logged later
		return false;
	}
}

bool PVXSDPIngestionDriver::ingestPVValue(const std::string& pvName, const pvxs::Value& pvValue) {
	dp::service::ingestion::IngestDataRequest request;
	request.set_providerid(m_providerID);
	request.set_clientrequestid("pv_" + pvName + "_" + std::to_string(m_requestCount++));
	request.add_tags(pvName); // todo(dp): is this correct?

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

	if (auto* dataValue = column->add_datavalues(); !convertPVToProtoValue(pvValue, dataValue)) {
		logError("Could not convert PV with name " + pvName + " to a proto value!");
		return false;
	}

	// todo(driver): use async gRPC calls here
	std::thread{[this, pvName, request = std::move(request)] {
		grpc::ClientContext context;
		dp::service::ingestion::IngestDataResponse response;
		static constexpr int RETRY_COUNT = 3;
		int retry = 0;
		grpc::Status status;
		while (!(status = m_stub->ingestData(&context, request, &response)).ok() && ++retry < RETRY_COUNT) {}
		if (!status.ok()) {
			logError("Ingestion failed for " + pvName + ": " + status.error_message());
		}
	}}.detach();

	return true;
}

void PVXSDPIngestionDriver::run() {
	while (!m_interrupted) {
		if (m_pvaWorkqueue.size() == 0) {
			std::this_thread::sleep_for(100ms);
			continue;
		}
		auto sub = m_pvaWorkqueue.pop();
		try {
			pvxs::Value update = sub->pop();
			if (!update) {
				continue;
			}
			ingestPVValue(sub->name(), update);
		} catch (const pvxs::client::RemoteError& e) {
			logError("Server error when reading PV " + sub->name() + ':' + e.what());
		}
		m_pvaWorkqueue.push(sub);
	}
	m_interrupted = false;
}

void PVXSDPIngestionDriver::stop() {
	m_interrupted = true;
}

void PVXSDPIngestionDriver::logError(const std::string& error) const {
	if (m_logger.error) {
		// If this is using std::cerr, it is already thread-safe
		m_logger.error(error);
	}
}
