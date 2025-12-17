#include "sioc.h"

#include <chrono>
#include <cmath>

#include <pvxs/nt.h>

using namespace pvxs;

PVServer::PVServer()
{
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
    nt::NTTable tableBuilder;
    tableBuilder.add_column(TypeCode::String, "deviceIDs");
    tableBuilder.add_column(TypeCode::Float64, "pressure");
    auto tableType = tableBuilder.build();
    m_pvTable.open(tableType.create());
    m_server.addPV("test:table", m_pvTable);

    m_pvBsasTable = server::SharedPV::buildReadonly();
    nt::NTTable bsasTableBuilder;
    bsasTableBuilder.add_column(TypeCode::Float64, "PV_NAME_A_DOUBLE_VALUE");
    bsasTableBuilder.add_column(TypeCode::String, "PV_NAME_B_STRING_VALUE");
    auto bsasTableType = bsasTableBuilder.build();
    // BSAS adds per-row timestamps as additional top-level arrays.
    bsasTableType += {
        Member(TypeCode::UInt64A, "secondsPastEpoch"),
        Member(TypeCode::UInt64A, "nanoseconds"),
    };
    m_pvBsasTable.open(bsasTableType.create());
    m_server.addPV("test:bsas_table", m_pvBsasTable);

    m_updateThreadActive = true;
    m_updateThread = std::thread([this]
                                 {
                                     int    counter = 0;
                                     double time = 0.0;

                                     while (m_updateThreadActive)
                                     {
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
                                             static constexpr const char* STATUSES[]{"OK", "WARNING", "FAULT"};
                                             auto                         pv = m_pvStatus.fetch();
                                             pv["value"] = STATUSES[counter % 3];
                                             pv["timeStamp.secondsPastEpoch"] = seconds;
                                             pv["timeStamp.nanoseconds"] = nanos;
                                             m_pvStatus.post(pv);
                                         }
                                         {
                                             auto                 pv = m_pvWaveform.fetch();
                                             shared_array<double> wave(256);
                                             for (int i = 0; i < wave.size(); i++)
                                             {
                                                 wave[i] = 1.5 + std::sin(time * i);
                                             }
                                             pv["value"] = wave.freeze();
                                             pv["timeStamp.secondsPastEpoch"] = seconds;
                                             pv["timeStamp.nanoseconds"] = nanos;
                                             m_pvWaveform.post(pv);
                                         }
                                         {
                                             auto pv = m_pvTable.fetch();
                                             pv["labels"] = pvxs::shared_array<const std::string>{"deviceIDs", "pressure"};
                                             pv["value.deviceIDs"] = pvxs::shared_array<const std::string>{"Device A", "Device B", "Device C"};
                                             pv["value.pressure"] = pvxs::shared_array<const double>{1.5 * std::cos(time), 1.5 * std::cos(time + 2.0), 1.5 * std::cos(time + 4.0)};
                                             pv["timeStamp.secondsPastEpoch"] = seconds;
                                             pv["timeStamp.nanoseconds"] = nanos;
                                             m_pvTable.post(pv);
                                         }
                                         {
                                             // BSAS-style NTTable: per-row timestamp arrays + sampled PV columns.
                                             auto pv = m_pvBsasTable.fetch();
                                             pv["labels"] = pvxs::shared_array<const std::string>{"PV_NAME_A_DOUBLE_VALUE", "PV_NAME_B_STRING_VALUE"};
                                             pv["value.PV_NAME_A_DOUBLE_VALUE"] = pvxs::shared_array<const double>{1.0, 2.0, 3.0};
                                             pv["value.PV_NAME_B_STRING_VALUE"] = pvxs::shared_array<const std::string>{"OK", "WARNING", "FAULT"};
                                             pvxs::shared_array<uint64_t> secArr(3);
                                             pvxs::shared_array<uint64_t> nanoArr(3);
                                             for (size_t i = 0; i < 3; ++i)
                                             {
                                                 secArr[i] = seconds;
                                                 nanoArr[i] = nanos + i;
                                             }
                                             pv["secondsPastEpoch"] = secArr.freeze();
                                             pv["nanoseconds"] = nanoArr.freeze();

                                             m_pvBsasTable.post(pv);
                                         }

                                         time += 0.5;
                                         std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                     }
                                 });

    m_server.start();
}

PVServer::~PVServer()
{
    m_updateThreadActive = false;
    if (m_updateThread.joinable())
    {
        m_updateThread.join();
    }
    m_server.stop();
}
