//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <pvxs/data.h>
#include <reader/impl/epics/BSASEpicsMLDPConversion.h>

#include <algorithm>
#include <cstdint>
#include <future>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using mldp_pvxs_driver::util::bus::EventValueStruct;

namespace {
struct UIntArrayView
{
    std::variant<pvxs::shared_array<const uint64_t>,
                 pvxs::shared_array<const int64_t>,
                 pvxs::shared_array<const uint32_t>,
                 pvxs::shared_array<const int32_t>>
        data;

    size_t size() const
    {
        return std::visit([](const auto& arr)
                          {
                              return static_cast<size_t>(arr.size());
                          },
                          data);
    }

    uint64_t at(size_t idx) const
    {
        return std::visit(
            [idx](const auto& arr)
            {
                using T = std::remove_cvref_t<decltype(arr[0])>;
                if constexpr (std::is_signed_v<T>)
                {
                    return static_cast<uint64_t>(static_cast<int64_t>(arr[idx]));
                }
                else
                {
                    return static_cast<uint64_t>(arr[idx]);
                }
            },
            data);
    }
};

std::optional<UIntArrayView> asUIntArrayView(const pvxs::Value& value)
{
    switch (value.type().code)
    {
    case pvxs::TypeCode::UInt64A: return UIntArrayView{value.as<pvxs::shared_array<const uint64_t>>()};
    case pvxs::TypeCode::Int64A: return UIntArrayView{value.as<pvxs::shared_array<const int64_t>>()};
    case pvxs::TypeCode::UInt32A: return UIntArrayView{value.as<pvxs::shared_array<const uint32_t>>()};
    case pvxs::TypeCode::Int32A: return UIntArrayView{value.as<pvxs::shared_array<const int32_t>>()};
    default: return std::nullopt;
    }
}
/// Result of converting a single column.
struct ColumnResult
{
    std::string                                                             name;
    std::vector<mldp_pvxs_driver::util::bus::IEventBusPush::EventValue>    events;
    size_t                                                                  emitted{0};
};

/// Process a single NTTable column, returning its converted events.
ColumnResult convertColumn(const pvxs::Value&           columns,
                           const pvxs::Value&           col,
                           const std::string&           colName,
                           size_t                       rowCount,
                           const std::vector<uint64_t>& tsSeconds,
                           const std::vector<uint64_t>& tsNanos)
{
    ColumnResult result;
    result.name = colName;

    const auto colCode = col.type().code;

    const auto emitArray = [&]<typename ArrT>(const pvxs::Value& v, auto&& setValue)
    {
        const auto arr = v.as<ArrT>();
        const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
        auto bulk = std::make_shared<std::vector<EventValueStruct>>(n);
        for (size_t i = 0; i < n; ++i)
        {
            auto& ev = (*bulk)[i];
            ev.epoch_seconds = tsSeconds[i];
            ev.nanoseconds   = tsNanos[i];
            setValue(&ev.data_value, arr[i]);
        }
        result.events.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            result.events.emplace_back(bulk, &(*bulk)[i]);
        }
        result.emitted = n;
    };

    switch (colCode)
    {
    case pvxs::TypeCode::BoolA:
        emitArray.template operator()<pvxs::shared_array<const bool>>(
            col, [](auto* dv, bool v) { dv->set_booleanvalue(v); });
        break;
    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::Int32A:
        emitArray.template operator()<pvxs::shared_array<const int32_t>>(
            col, [](auto* dv, int32_t v) { dv->set_intvalue(v); });
        break;
    case pvxs::TypeCode::Int64A:
        emitArray.template operator()<pvxs::shared_array<const int64_t>>(
            col, [](auto* dv, int64_t v) { dv->set_longvalue(v); });
        break;
    case pvxs::TypeCode::UInt8A:
    case pvxs::TypeCode::UInt16A:
    case pvxs::TypeCode::UInt32A:
        emitArray.template operator()<pvxs::shared_array<const uint32_t>>(
            col, [](auto* dv, uint32_t v) { dv->set_uintvalue(v); });
        break;
    case pvxs::TypeCode::UInt64A:
        emitArray.template operator()<pvxs::shared_array<const uint64_t>>(
            col, [](auto* dv, uint64_t v) { dv->set_ulongvalue(v); });
        break;
    case pvxs::TypeCode::Float32A:
        emitArray.template operator()<pvxs::shared_array<const float>>(
            col, [](auto* dv, float v) { dv->set_floatvalue(v); });
        break;
    case pvxs::TypeCode::Float64A:
        emitArray.template operator()<pvxs::shared_array<const double>>(
            col, [](auto* dv, double v) { dv->set_doublevalue(v); });
        break;
    case pvxs::TypeCode::StringA:
        emitArray.template operator()<pvxs::shared_array<const std::string>>(
            col, [](auto* dv, const std::string& v) { dv->set_stringvalue(v); });
        break;
    case pvxs::TypeCode::StructA:
    case pvxs::TypeCode::UnionA:
    case pvxs::TypeCode::AnyA:
        {
            const auto arr = col.as<pvxs::shared_array<const pvxs::Value>>();
            const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
            auto bulk = std::make_shared<std::vector<EventValueStruct>>(n);
            for (size_t i = 0; i < n; ++i)
            {
                auto& ev = (*bulk)[i];
                ev.epoch_seconds = tsSeconds[i];
                ev.nanoseconds   = tsNanos[i];
                const pvxs::Value cell = arr[i];
                const pvxs::Value cellValue = (cell.type().kind() == pvxs::Kind::Compound) ? cell["value"] : pvxs::Value{};
                mldp_pvxs_driver::reader::impl::epics::EpicsMLDPConversion::convertPVToProtoValue(
                    cellValue.valid() ? cellValue : cell, &ev.data_value);
            }
            result.events.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                result.events.emplace_back(bulk, &(*bulk)[i]);
            }
            result.emitted = n;
        }
        break;
    default:
        break;
    }

    return result;
}

} // namespace

bool BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&                    log,
                                                        const std::string&                                      tablePvName,
                                                        const pvxs::Value&                                      epicsValue,
                                                        const std::string&                                      tsSecondsField,
                                                        const std::string&                                      tsNanosField,
                                                        mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch* outBatch,
                                                        size_t&                                                 outEmitted)
{
    outBatch->tags.clear();
    outBatch->values.clear();
    outBatch->tags.push_back(tablePvName);
    return tryBuildNtTableRowTsBatch(log, tablePvName, epicsValue,
        tsSecondsField, tsNanosField,
        [&](std::string colName, std::vector<mldp_pvxs_driver::util::bus::IEventBusPush::EventValue> events) {
            outBatch->values[std::move(colName)] = std::move(events);
        }, outEmitted);
}

bool BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                                        const std::string&                    tablePvName,
                                                        const pvxs::Value&                    epicsValue,
                                                        const std::string&                    tsSecondsField,
                                                        const std::string&                    tsNanosField,
                                                        ColumnEmitFn                          emitColumn,
                                                        size_t&                               outEmitted)
{
    outEmitted = 0;

    if (!epicsValue || epicsValue.type().kind() != pvxs::Kind::Compound)
    {
        warnf(log, "NTTable row-ts PV {} update is not a compound value", tablePvName);
        return false;
    }

    const auto columns = epicsValue["value"];
    if (!columns || columns.type().kind() != pvxs::Kind::Compound)
    {
        warnf(log, "NTTable row-ts PV {} has no usable value struct", tablePvName);
        return false;
    }

    pvxs::Value secondsValue = epicsValue[tsSecondsField];
    pvxs::Value nanosValue = epicsValue[tsNanosField];
    if (!secondsValue.valid() || !nanosValue.valid())
    {
        secondsValue = columns[tsSecondsField];
        nanosValue = columns[tsNanosField];
    }

    if (!secondsValue.valid() || !nanosValue.valid())
    {
        warnf(log,
              "NTTable row-ts PV {} missing timestamp arrays '{}'/'{}' (expected either as top-level fields or under value.*)",
              tablePvName,
              tsSecondsField,
              tsNanosField);
        return false;
    }

    const auto secondsArr = asUIntArrayView(secondsValue);
    const auto nanosArr = asUIntArrayView(nanosValue);
    if (!secondsArr.has_value() || !nanosArr.has_value())
    {
        warnf(log, "NTTable row-ts PV {} timestamp arrays have unsupported types", tablePvName);
        return false;
    }

    const auto rowCount = std::min(secondsArr->size(), nanosArr->size());
    if (rowCount == 0)
    {
        tracef(log, "NTTable row-ts PV {} has 0 timestamped rows", tablePvName);
        return false;
    }

    std::vector<uint64_t> tsSeconds(rowCount), tsNanos(rowCount);
    for (size_t i = 0; i < rowCount; ++i)
    {
        tsSeconds[i] = secondsArr->at(i);
        tsNanos[i]   = nanosArr->at(i);
    }

    for (const auto& col : columns.ichildren())
    {
        if (!col.valid())
        {
            continue;
        }
        const auto colName = columns.nameOf(col);
        if (colName == tsSecondsField || colName == tsNanosField)
        {
            continue;
        }

        auto result = convertColumn(columns, col, colName, rowCount, tsSeconds, tsNanos);
        if (result.emitted > 0)
        {
            outEmitted += result.emitted;
            emitColumn(std::move(result.name), std::move(result.events));
        }
        else if (col.type().code != pvxs::TypeCode::StructA &&
                 col.type().code != pvxs::TypeCode::UnionA &&
                 col.type().code != pvxs::TypeCode::AnyA)
        {
            // convertColumn returns 0 emitted for unsupported types
            const auto colCode = col.type().code;
            if (colCode != pvxs::TypeCode::BoolA &&
                colCode != pvxs::TypeCode::Int8A && colCode != pvxs::TypeCode::Int16A && colCode != pvxs::TypeCode::Int32A &&
                colCode != pvxs::TypeCode::Int64A &&
                colCode != pvxs::TypeCode::UInt8A && colCode != pvxs::TypeCode::UInt16A && colCode != pvxs::TypeCode::UInt32A &&
                colCode != pvxs::TypeCode::UInt64A &&
                colCode != pvxs::TypeCode::Float32A && colCode != pvxs::TypeCode::Float64A &&
                colCode != pvxs::TypeCode::StringA)
            {
                tracef(log,
                       "NTTable row-ts PV {} column '{}' has unsupported type code {}",
                       tablePvName,
                       colName,
                       static_cast<int>(colCode));
            }
        }
    }

    return outEmitted > 0;
}

bool BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                                        const std::string&                    tablePvName,
                                                        const pvxs::Value&                    epicsValue,
                                                        const std::string&                    tsSecondsField,
                                                        const std::string&                    tsNanosField,
                                                        ColumnEmitFn                          emitColumn,
                                                        size_t&                               outEmitted,
                                                        BS::light_thread_pool*                pool)
{
    if (!pool)
    {
        return tryBuildNtTableRowTsBatch(log, tablePvName, epicsValue, tsSecondsField, tsNanosField, emitColumn, outEmitted);
    }

    outEmitted = 0;

    if (!epicsValue || epicsValue.type().kind() != pvxs::Kind::Compound)
    {
        warnf(log, "NTTable row-ts PV {} update is not a compound value", tablePvName);
        return false;
    }

    const auto columns = epicsValue["value"];
    if (!columns || columns.type().kind() != pvxs::Kind::Compound)
    {
        warnf(log, "NTTable row-ts PV {} has no usable value struct", tablePvName);
        return false;
    }

    pvxs::Value secondsValue = epicsValue[tsSecondsField];
    pvxs::Value nanosValue = epicsValue[tsNanosField];
    if (!secondsValue.valid() || !nanosValue.valid())
    {
        secondsValue = columns[tsSecondsField];
        nanosValue = columns[tsNanosField];
    }

    if (!secondsValue.valid() || !nanosValue.valid())
    {
        warnf(log,
              "NTTable row-ts PV {} missing timestamp arrays '{}'/'{}' (expected either as top-level fields or under value.*)",
              tablePvName,
              tsSecondsField,
              tsNanosField);
        return false;
    }

    const auto secondsArr = asUIntArrayView(secondsValue);
    const auto nanosArr = asUIntArrayView(nanosValue);
    if (!secondsArr.has_value() || !nanosArr.has_value())
    {
        warnf(log, "NTTable row-ts PV {} timestamp arrays have unsupported types", tablePvName);
        return false;
    }

    const auto rowCount = std::min(secondsArr->size(), nanosArr->size());
    if (rowCount == 0)
    {
        tracef(log, "NTTable row-ts PV {} has 0 timestamped rows", tablePvName);
        return false;
    }

    auto tsSeconds = std::make_shared<std::vector<uint64_t>>(rowCount);
    auto tsNanos   = std::make_shared<std::vector<uint64_t>>(rowCount);
    for (size_t i = 0; i < rowCount; ++i)
    {
        (*tsSeconds)[i] = secondsArr->at(i);
        (*tsNanos)[i]   = nanosArr->at(i);
    }

    // Collect columns into a vector for parallel dispatch
    struct ColInfo
    {
        pvxs::Value col;
        std::string name;
    };
    std::vector<ColInfo> colInfos;
    for (const auto& col : columns.ichildren())
    {
        if (!col.valid())
        {
            continue;
        }
        const auto colName = columns.nameOf(col);
        if (colName == tsSecondsField || colName == tsNanosField)
        {
            continue;
        }
        colInfos.push_back({col, colName});
    }

    // Submit column conversions to the thread pool
    std::vector<std::future<ColumnResult>> futures;
    futures.reserve(colInfos.size());
    for (auto& ci : colInfos)
    {
        futures.push_back(pool->submit_task(
            [&columns, col = ci.col, name = ci.name, rowCount, tsSeconds, tsNanos]() -> ColumnResult
            {
                return convertColumn(columns, col, name, rowCount, *tsSeconds, *tsNanos);
            }));
    }

    // Collect results and emit sequentially
    for (auto& fut : futures)
    {
        auto result = fut.get();
        if (result.emitted > 0)
        {
            outEmitted += result.emitted;
            emitColumn(std::move(result.name), std::move(result.events));
        }
    }

    return outEmitted > 0;
}
