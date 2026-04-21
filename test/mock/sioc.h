#pragma once

#include "Gen1NTablePV.h"

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

    /// Sanitized field names for the CU-HXR Gen1 NTTable columns.
    const std::vector<std::string>& gen1CuHxrColumnNames() const { return m_cuHxr.fieldNames(); }

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
    pvxs::server::SharedPV m_pvBsasTable; // NTTable with per-row timestamps: PV_A (Float64), PV_B (Int32), PV_C (Float32), secondsPastEpoch (UInt32), nanoseconds (UInt32)
    Gen1NTablePV           m_cuHxr{   // CU-HXR — cu-hxr Gen1 BSAS NTTable
        "CU-HXR",
        MLDP_TEST_DATA_DIR "/signals.cu-hxr.prod"
    };
    std::vector<TypedPV>   m_typedPvs;
};
