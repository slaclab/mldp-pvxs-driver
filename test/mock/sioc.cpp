#include "sioc.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

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
    bsasTableBuilder.add_column(TypeCode::Float64, "PV_A");  // double PV
    bsasTableBuilder.add_column(TypeCode::Int32,   "PV_B");  // int32 PV
    bsasTableBuilder.add_column(TypeCode::Float32, "PV_C");  // float32 PV
    // NTTable columns are always arrays-of-scalars; add_column() takes the scalar element type.
    bsasTableBuilder.add_column(TypeCode::UInt32, "secondsPastEpoch");
    bsasTableBuilder.add_column(TypeCode::UInt32, "nanoseconds");
    auto bsasTableType = bsasTableBuilder.build();

    m_pvBsasTable.open(bsasTableType.create());
    m_server.addPV("test:bsas_table", m_pvBsasTable);

    m_pvBsasTableNan = server::SharedPV::buildReadonly();
    nt::NTTable nanTableBuilder;
    nanTableBuilder.add_column(TypeCode::Float64, "COL_A");  // valid
    nanTableBuilder.add_column(TypeCode::Float64, "COL_B");  // all-NaN
    nanTableBuilder.add_column(TypeCode::Float64, "COL_C");  // all-NaN
    nanTableBuilder.add_column(TypeCode::Float64, "COL_D");  // valid
    nanTableBuilder.add_column(TypeCode::Float64, "COL_E");  // all-NaN
    nanTableBuilder.add_column(TypeCode::Float64, "COL_F");  // valid
    nanTableBuilder.add_column(TypeCode::UInt32, "secondsPastEpoch");
    nanTableBuilder.add_column(TypeCode::UInt32, "nanoseconds");
    auto nanTableType = nanTableBuilder.build();
    m_pvBsasTableNan.open(nanTableType.create());
    m_server.addPV("test:bsas_table_nan", m_pvBsasTableNan);

    m_cuHxr.registerPV(m_server);

    addBsasTablePVs();

    const auto makeScalar = []<typename T>(auto valueFn)
    {
        return [valueFn](pvxs::Value& pv, int counter, double time)
        {
            pv["value"] = static_cast<T>(valueFn(counter, time));
        };
    };

    const auto makeArray = []<typename T>(auto valueFn)
    {
        return [valueFn](pvxs::Value& pv, int counter, double time)
        {
            shared_array<T> arr(4);
            for (size_t i = 0; i < arr.size(); ++i)
            {
                arr[i] = static_cast<T>(valueFn(counter, time, i));
            }
            pv["value"] = arr.freeze();
        };
    };

    const auto addTypedPV = [this](const std::string& name, TypeCode type,
                                   std::function<void(pvxs::Value&, int, double)> update)
    {
        m_typedPvs.push_back(TypedPV{name, type, server::SharedPV::buildReadonly(), update});
        auto& entry = m_typedPvs.back();
        entry.pv.open(nt::NTScalar{type}.create());
        m_server.addPV(name, entry.pv);
    };

    addTypedPV("test:bool", TypeCode::Bool,
               makeScalar.template operator()<bool>([](int counter, double)
                                                    {
                                                        return (counter % 2) == 0;
                                                    }));
    addTypedPV("test:int8", TypeCode::Int8,
               makeScalar.template operator()<int8_t>([](int counter, double)
                                                      {
                                                          return counter % 100;
                                                      }));
    addTypedPV("test:int16", TypeCode::Int16,
               makeScalar.template operator()<int16_t>([](int counter, double)
                                                       {
                                                           return counter % 32000;
                                                       }));
    addTypedPV("test:int32", TypeCode::Int32,
               makeScalar.template operator()<int32_t>([](int counter, double)
                                                       {
                                                           return counter;
                                                       }));
    addTypedPV("test:int64", TypeCode::Int64,
               makeScalar.template operator()<int64_t>([](int counter, double)
                                                       {
                                                           return static_cast<int64_t>(counter) * 1000;
                                                       }));
    addTypedPV("test:uint8", TypeCode::UInt8,
               makeScalar.template operator()<uint8_t>([](int counter, double)
                                                       {
                                                           return counter % 200;
                                                       }));
    addTypedPV("test:uint16", TypeCode::UInt16,
               makeScalar.template operator()<uint16_t>([](int counter, double)
                                                        {
                                                            return counter % 60000;
                                                        }));
    addTypedPV("test:uint32", TypeCode::UInt32,
               makeScalar.template operator()<uint32_t>([](int counter, double)
                                                        {
                                                            return static_cast<uint32_t>(counter);
                                                        }));
    addTypedPV("test:uint64", TypeCode::UInt64,
               makeScalar.template operator()<uint64_t>([](int counter, double)
                                                        {
                                                            return static_cast<uint64_t>(counter) * 1000u;
                                                        }));
    addTypedPV("test:float32", TypeCode::Float32,
               makeScalar.template operator()<float>([](int, double time)
                                                     {
                                                         return 1.25f + static_cast<float>(std::sin(time));
                                                     }));
    addTypedPV("test:float64", TypeCode::Float64,
               makeScalar.template operator()<double>([](int, double time)
                                                      {
                                                          return 1.5 + std::cos(time);
                                                      }));
    addTypedPV("test:string", TypeCode::String,
               makeScalar.template operator()<std::string>([](int counter, double)
                                                           {
                                                               return (counter % 2 == 0) ? std::string("ON") : std::string("OFF");
                                                           }));
    addTypedPV("test:bool_array", TypeCode::BoolA,
               makeArray.template operator()<bool>([](int counter, double, size_t i)
                                                   {
                                                       return ((counter + static_cast<int>(i)) % 2) == 0;
                                                   }));
    addTypedPV("test:int8_array", TypeCode::Int8A,
               makeArray.template operator()<int8_t>([](int counter, double, size_t i)
                                                     {
                                                         return static_cast<int8_t>(counter + static_cast<int>(i));
                                                     }));
    addTypedPV("test:int16_array", TypeCode::Int16A,
               makeArray.template operator()<int16_t>([](int counter, double, size_t i)
                                                      {
                                                          return static_cast<int16_t>(counter + static_cast<int>(i));
                                                      }));
    addTypedPV("test:int32_array", TypeCode::Int32A,
               makeArray.template operator()<int32_t>([](int counter, double, size_t i)
                                                      {
                                                          return static_cast<int32_t>(counter + static_cast<int>(i));
                                                      }));
    addTypedPV("test:int64_array", TypeCode::Int64A,
               makeArray.template operator()<int64_t>([](int counter, double, size_t i)
                                                      {
                                                          return static_cast<int64_t>(counter + static_cast<int>(i)) * 1000;
                                                      }));
    addTypedPV("test:uint8_array", TypeCode::UInt8A,
               makeArray.template operator()<uint8_t>([](int counter, double, size_t i)
                                                      {
                                                          return static_cast<uint8_t>(counter + static_cast<int>(i));
                                                      }));
    addTypedPV("test:uint16_array", TypeCode::UInt16A,
               makeArray.template operator()<uint16_t>([](int counter, double, size_t i)
                                                       {
                                                           return static_cast<uint16_t>(counter + static_cast<int>(i));
                                                       }));
    addTypedPV("test:uint32_array", TypeCode::UInt32A,
               makeArray.template operator()<uint32_t>([](int counter, double, size_t i)
                                                       {
                                                           return static_cast<uint32_t>(counter + static_cast<int>(i));
                                                       }));
    addTypedPV("test:uint64_array", TypeCode::UInt64A,
               makeArray.template operator()<uint64_t>([](int counter, double, size_t i)
                                                       {
                                                           return static_cast<uint64_t>(counter + static_cast<int>(i)) * 1000u;
                                                       }));
    addTypedPV("test:float32_array", TypeCode::Float32A,
               makeArray.template operator()<float>([](int, double time, size_t i)
                                                    {
                                                        return static_cast<float>(1.0 + std::sin(time + static_cast<double>(i)));
                                                    }));
    addTypedPV("test:float64_array", TypeCode::Float64A,
               makeArray.template operator()<double>([](int, double time, size_t i)
                                                     {
                                                         return 1.0 + std::cos(time + static_cast<double>(i));
                                                     }));
    addTypedPV("test:string_array", TypeCode::StringA,
               makeArray.template operator()<std::string>([](int, double, size_t i)
                                                          {
                                                              static const char* kValues[] = {"alpha", "beta", "gamma", "delta"};
                                                              return std::string(kValues[i % 4]);
                                                          }));

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
                                             pv["alarm.severity"] = 2;
                                             pv["alarm.status"] = 1;
                                             pv["alarm.message"] = "TEST_ALARM";
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
                                             // BSAS-style NTTable: per-row timestamp arrays + data columns.
                                             // PV_A (Float64), PV_B (Int32), PV_C (Float32)
                                             auto pv = m_pvBsasTable.fetch();
                                             pv["labels"] = pvxs::shared_array<const std::string>{"PV_A", "PV_B", "PV_C",
                                                                                                   "secondsPastEpoch", "nanoseconds"};
                                             pv["value.PV_A"] = pvxs::shared_array<const double>{1.0 + std::sin(time), 2.0 + std::sin(time), 3.0 + std::sin(time)};
                                             pvxs::shared_array<int32_t> pvBArr(3);
                                             for (size_t i = 0; i < 3; ++i) pvBArr[i] = static_cast<int32_t>(counter + static_cast<int>(i));
                                             pv["value.PV_B"] = pvBArr.freeze();
                                             pv["value.PV_C"] = pvxs::shared_array<const float>{1.25f + static_cast<float>(std::cos(time)),
                                                                                                  2.25f + static_cast<float>(std::cos(time)),
                                                                                                  3.25f + static_cast<float>(std::cos(time))};
                                             pvxs::shared_array<uint32_t> secArr(3);
                                             pvxs::shared_array<uint32_t> nanoArr(3);
                                             for (size_t i = 0; i < 3; ++i)
                                             {
                                                 secArr[i]  = static_cast<uint32_t>(seconds);
                                                 nanoArr[i] = static_cast<uint32_t>(nanos + i);
                                             }
                                             pv["value.secondsPastEpoch"] = secArr.freeze();
                                             pv["value.nanoseconds"]      = nanoArr.freeze();
                                             pv["timeStamp.secondsPastEpoch"] = seconds;
                                             pv["timeStamp.nanoseconds"]      = nanos;
                                             m_pvBsasTable.post(pv);
                                         }
                                         {
                                             constexpr size_t kRows = 3;
                                             const double nan = std::numeric_limits<double>::quiet_NaN();
                                             auto pv = m_pvBsasTableNan.fetch();
                                             pv["labels"] = pvxs::shared_array<const std::string>{
                                                 "COL_A", "COL_B", "COL_C", "COL_D", "COL_E", "COL_F",
                                                 "secondsPastEpoch", "nanoseconds"};
                                             pv["value.COL_A"] = pvxs::shared_array<const double>{1.0 + std::sin(time), 2.0, 3.0};
                                             pv["value.COL_B"] = pvxs::shared_array<const double>{nan, nan, nan};
                                             pv["value.COL_C"] = pvxs::shared_array<const double>{nan, nan, nan};
                                             pv["value.COL_D"] = pvxs::shared_array<const double>{4.0, 5.0 + std::cos(time), 6.0};
                                             pv["value.COL_E"] = pvxs::shared_array<const double>{nan, nan, nan};
                                             pv["value.COL_F"] = pvxs::shared_array<const double>{7.0, 8.0, 9.0 + std::sin(time)};
                                             pvxs::shared_array<uint32_t> secArr(kRows);
                                             pvxs::shared_array<uint32_t> nanoArr(kRows);
                                             for (size_t i = 0; i < kRows; ++i)
                                             {
                                                 secArr[i]  = static_cast<uint32_t>(seconds);
                                                 nanoArr[i] = static_cast<uint32_t>(nanos + i);
                                             }
                                             pv["value.secondsPastEpoch"] = secArr.freeze();
                                             pv["value.nanoseconds"]      = nanoArr.freeze();
                                             pv["timeStamp.secondsPastEpoch"] = seconds;
                                             pv["timeStamp.nanoseconds"]      = nanos;
                                             m_pvBsasTableNan.post(pv);
                                         }

                                         // update all the typed PVs using their configured update functions
                                         for (auto& entry : m_typedPvs)
                                         {
                                             auto pv = entry.pv.fetch();
                                             entry.update(pv, counter, time);
                                             pv["timeStamp.secondsPastEpoch"] = seconds;
                                             pv["timeStamp.nanoseconds"] = nanos;
                                             entry.pv.post(pv);
                                         }

                                         m_cuHxr.post(seconds, nanos, counter, time);

                                         for (auto& table : m_bsasTables)
                                         {
                                             table->post(counter);
                                         }

                                         time += 0.5;
                                         std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                     }
                                 });

    m_server.start();
}

void PVServer::addBsasTablePVs()
{
    using Column = BsasTablePV::Column;
    using Options = BsasTablePV::Options;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    const auto add = [this](std::string name, std::vector<Column> columns, Options options = {})
    {
        m_bsasTables.push_back(std::make_unique<BsasTablePV>(std::move(name), std::move(columns), std::move(options)));
        m_bsasTables.back()->registerPV(m_server);
    };

    // ---- Gen1 ---------------------------------------------------------------
    // Typed columns; value = counter * 100 + row (+ column offset).
    add("test:bsas_gen1_table",
        {
            {"PV_A", "", TypeCode::Float64, [](int c, size_t r) { return c * 100.0 + static_cast<double>(r); }},
            {"PV_B", "", TypeCode::Int32, [](int c, size_t r) { return c * 100.0 + static_cast<double>(r) + 1000.0; }},
            {"PV_C", "", TypeCode::Float32, [](int c, size_t r) { return c + static_cast<double>(r) * 0.5; }},
        });

    // Ten Float64 columns W00..W09; value = column * 1000 + row.
    {
        std::vector<Column> wide;
        for (int i = 0; i < 10; ++i)
        {
            char field[8];
            std::snprintf(field, sizeof(field), "W%02d", i);
            wide.push_back({field, "", TypeCode::Float64,
                            [i](int, size_t r) { return i * 1000.0 + static_cast<double>(r); }});
        }
        add("test:bsas_gen1_wide", std::move(wide));
    }

    // 250 rows per update: exercises rows-per-data-batch (default 120 -> 120 + 120 + 10).
    add("test:bsas_gen1_long",
        {
            {"L_A", "", TypeCode::Float64, [](int, size_t r) { return static_cast<double>(r); }},
            {"L_B", "", TypeCode::Int32, [](int, size_t r) { return 10000.0 + static_cast<double>(r); }},
        },
        Options{.rows = 250});

    // Mix of valid and all-NaN columns.
    add("test:bsas_gen1_nan",
        {
            {"OK_A", "", TypeCode::Float64, [](int c, size_t r) { return c + static_cast<double>(r); }},
            {"NAN_B", "", TypeCode::Float64, [nan](int, size_t) { return nan; }},
            {"OK_C", "", TypeCode::Int32, [](int c, size_t r) { return c * 10.0 + static_cast<double>(r); }},
        });

    // Every column all-NaN.
    add("test:bsas_gen1_all_nan",
        {
            {"NAN_A", "", TypeCode::Float64, [nan](int, size_t) { return nan; }},
            {"NAN_B", "", TypeCode::Float32, [nan](int, size_t) { return nan; }},
        });

    // No timestamp columns at all.
    add("test:bsas_gen1_no_ts",
        {
            {"PV_A", "", TypeCode::Float64, [](int c, size_t r) { return c + static_cast<double>(r); }},
        },
        Options{.tsSecondsField = "", .tsNanosField = ""});

    // Custom timestamp column names.
    add("test:bsas_gen1_custom_ts",
        {
            {"PV_A", "", TypeCode::Float64, [](int c, size_t r) { return c + static_cast<double>(r); }},
            {"PV_B", "", TypeCode::UInt64, [](int c, size_t r) { return c * 2.0 + static_cast<double>(r); }},
        },
        Options{.tsSecondsField = "secs", .tsNanosField = "nsecs"});

    // ---- Gen2 ---------------------------------------------------------------
    // Two PVs x Val/CNT/RMS/AVG/MIN/MAX plus pid.  Labels carry the real names
    // (TST:GEN2:A.Val ...), fields the sanitized ones (TST_GEN2_A_Val ...).
    // Val = counter * 100 + row (+ 50 for B); statistics are negative so they
    // can never be mistaken for Val.
    const auto gen2Columns = [](const std::string& pidField, const std::string& pidLabel,
                                const std::vector<std::string>& suffixes, char labelSep, bool nanValA)
    {
        const double        nanValue = std::numeric_limits<double>::quiet_NaN();
        std::vector<Column> cols;
        cols.push_back({pidField, pidLabel, TypeCode::UInt64,
                        [](int c, size_t r) { return c * 1000.0 + static_cast<double>(r); }});
        const struct
        {
            const char* field;
            const char* label;
            double      valOffset;
            TypeCode    valType;
        } pvs[] = {{"TST_GEN2_A", "TST:GEN2:A", 0.0, TypeCode::Float64},
                   {"TST_GEN2_B", "TST:GEN2:B", 50.0, TypeCode::Float32}};
        for (const auto& pv : pvs)
        {
            for (size_t s = 0; s < suffixes.size(); ++s)
            {
                const auto& suffix = suffixes[s];
                Column      col;
                col.field = std::string(pv.field) + "_" + suffix;
                col.label = std::string(pv.label) + labelSep + suffix;
                if (s == 0) // value column
                {
                    col.type = pv.valType;
                    const bool   isNan = nanValA && pv.valOffset == 0.0;
                    const double offset = pv.valOffset;
                    col.value = [isNan, offset, nanValue](int c, size_t r)
                    {
                        return isNan ? nanValue : c * 100.0 + static_cast<double>(r) + offset;
                    };
                }
                else if (s == 1) // count
                {
                    col.type = TypeCode::UInt32;
                    col.value = [](int, size_t) { return 7.0; };
                }
                else // statistics
                {
                    col.type = TypeCode::Float64;
                    col.value = [s](int, size_t r) { return -static_cast<double>(s) - static_cast<double>(r) * 0.25; };
                }
                cols.push_back(std::move(col));
            }
        }
        return cols;
    };
    const std::vector<std::string> kStdSuffixes{"Val", "CNT", "RMS", "AVG", "MIN", "MAX"};

    add("test:bsas_gen2_table", gen2Columns("pid", "pid", kStdSuffixes, '.', false));
    add("test:bsas_gen2_no_labels", gen2Columns("pid", "pid", kStdSuffixes, '.', false),
        Options{.withLabels = false});
    add("test:bsas_gen2_nan", gen2Columns("pid", "pid", kStdSuffixes, '.', true));
    // Custom pid column, lowercase "value" suffix, '_' separator in labels.
    add("test:bsas_gen2_custom",
        gen2Columns("pulseId", "pulseId", {"value", "cnt", "rms", "avg", "min", "max"}, '_', false));
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
