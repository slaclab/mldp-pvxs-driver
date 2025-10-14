#pragma once

#include <atomic>
#include <thread>

#include <pvxs/server.h>
#include <pvxs/sharedpv.h>

class PVServer {
public:
	PVServer();

	~PVServer();

private:
	pvxs::server::Server m_server;
	std::thread m_updateThread;
	std::atomic<bool> m_updateThreadActive = false;

	pvxs::server::SharedPV m_pvCounter; // Int32
	pvxs::server::SharedPV m_pvVoltage; // Float64
	pvxs::server::SharedPV m_pvStatus; // String
	pvxs::server::SharedPV m_pvWaveform; // Float64A
	pvxs::server::SharedPV m_pvTable; // Struct
};
