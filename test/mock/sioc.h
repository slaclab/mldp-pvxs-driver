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
	std::atomic<bool> m_updateThreadActive;

	pvxs::server::SharedPV m_pvCounter;
	int m_counter = 0;
};
