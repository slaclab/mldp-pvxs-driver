#include "sioc.h"

#include <chrono>

#include <pvxs/nt.h>

using namespace pvxs;

PVServer::PVServer()
		: m_updateThreadActive(false) {
	m_server = server::Config::fromEnv().build();

	m_pvCounter = server::SharedPV::buildReadonly();
	m_pvCounter.open(nt::NTScalar{TypeCode::Int32}.create());
	m_server.addPV("test:counter", m_pvCounter);

	m_updateThreadActive = true;
	m_updateThread = std::thread([this] {
		while (m_updateThreadActive) {
			const auto now = std::chrono::system_clock::now();
			const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

			auto pvCounter = m_pvCounter.fetch();
			pvCounter["value"] = ++m_counter;
			pvCounter["timeStamp.secondsPastEpoch"] = timestamp / 1'000'000'000;
			pvCounter["timeStamp.nanoseconds"] = timestamp % 1'000'000'000;
			m_pvCounter.post(pvCounter);

			std::this_thread::sleep_for(std::chrono::milliseconds(250));
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
