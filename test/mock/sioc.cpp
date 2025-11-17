#include "sioc.h"

#include <chrono>
#include <cmath>

#include <pvxs/nt.h>

using namespace pvxs;

PVServer::PVServer() {
	m_server = server::Config::fromEnv().build();

	m_pvCounter = server::SharedPV::buildReadonly();
	m_pvCounter.open(nt::NTScalar{TypeCode::Int32}.create());
	m_server.addPV("test:counter", m_pvCounter);

	m_pvVoltage = server::SharedPV::buildReadonly();
	m_pvVoltage.open(nt::NTScalar{TypeCode::Float64}.create());
	m_server.addPV("test:voltage", m_pvVoltage);

	m_pvStatus = server::SharedPV::buildReadonly();
	m_pvStatus.open(nt::NTScalar{TypeCode::String}.create());
	m_server.addPV("test:status", m_pvStatus);

	m_pvWaveform = server::SharedPV::buildReadonly();
	m_pvWaveform.open(nt::NTScalar{TypeCode::Float64A}.create());
	m_server.addPV("test:waveform", m_pvWaveform);

	m_pvTable = server::SharedPV::buildReadonly();
	auto tableType = nt::NTTable{}.build();
	tableType += {
		Member(TypeCode::StringA, "deviceIDs"),
		Member(TypeCode::Float64A, "pressure"),
	};
	m_pvTable.open(tableType.create());
	m_server.addPV("test:table", m_pvTable);

	m_updateThreadActive = true;
	m_updateThread = std::thread([this] {
		int counter = 0;
		double time = 0.0;

		while (m_updateThreadActive) {
			const auto now = std::chrono::system_clock::now();
			const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
			const auto seconds = timestamp / 1'000'000'000;
			const auto nanos = timestamp % 1'000'000'000;
			{
				auto pv = m_pvCounter.fetch();
				pv["value"] = ++counter;
				pv["timeStamp.secondsPastEpoch"] = seconds;
				pv["timeStamp.nanoseconds"] = nanos;
				m_pvCounter.post(pv);
			}
			{
				auto pv = m_pvVoltage.fetch();
				pv["value"] = 1.5 + std::sin(time);
				pv["timeStamp.secondsPastEpoch"] = seconds;
				pv["timeStamp.nanoseconds"] = nanos;
				m_pvVoltage.post(pv);
			}
			{
				static constexpr const char* STATUSES[] {"OK", "WARNING", "FAULT"};
				auto pv = m_pvStatus.fetch();
				pv["value"] = STATUSES[counter % 3];
				pv["timeStamp.secondsPastEpoch"] = seconds;
				pv["timeStamp.nanoseconds"] = nanos;
				m_pvStatus.post(pv);
			}
			{
				auto pv = m_pvWaveform.fetch();
				shared_array<double> wave(256);
				for (int i = 0; i < wave.size(); i++) {
					wave[i] = 1.5 + std::sin(time * i);
				}
				pv["value"] = wave.freeze();
				pv["timeStamp.secondsPastEpoch"] = seconds;
				pv["timeStamp.nanoseconds"] = nanos;
				m_pvWaveform.post(pv);
			}
			{
				auto pv = m_pvTable.fetch();
				pv["deviceIDs"] = pvxs::shared_array<const std::string>{"Device A", "Device B", "Device C"};
				pv["pressure"] = pvxs::shared_array<const double>{1.5 * std::cos(time), 1.5 * std::cos(time + 2.0), 1.5 * std::cos(time + 4.0)};
				pv["timeStamp.secondsPastEpoch"] = seconds;
				pv["timeStamp.nanoseconds"] = nanos;
				m_pvTable.post(pv);
			}

			time += 0.5;
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	});

	m_server.start();
}

PVServer::~PVServer() {
	m_updateThreadActive = false;
	if (m_updateThread.joinable()) {
		m_updateThread.join();
	}
	m_server.stop();
}
