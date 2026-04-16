#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <pvxs/data.h>
#include <pvxs/server.h>
#include <pvxs/sharedpv.h>

class PVServer
{
public:
    PVServer();

    ~PVServer();

private:
    struct TypedPV
    {
        std::string                                    name;
        pvxs::TypeCode                                 type;
        pvxs::server::SharedPV                         pv;
        std::function<void(pvxs::Value&, int, double)> update;
    };

    pvxs::server::Server m_server;
    std::thread          m_updateThread;
    std::atomic<bool>    m_updateThreadActive = false;

    pvxs::server::SharedPV m_pvCounter;   // Int32
    pvxs::server::SharedPV m_pvVoltage;   // Float64
    pvxs::server::SharedPV m_pvStatus;    // String
    pvxs::server::SharedPV m_pvWaveform;  // Float64A
    pvxs::server::SharedPV m_pvTable;     // Struct
    pvxs::server::SharedPV m_pvBsasTable; // NTTable with per-row timestamps
    std::vector<TypedPV>   m_typedPvs;
};
