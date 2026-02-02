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
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;

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

        const auto colCode = col.type().code;

        const auto emitArray = [&]<typename ArrT>(const pvxs::Value& v, auto&& setValue)
        {
            const auto arr = v.as<ArrT>();
            const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
            std::vector<mldp_pvxs_driver::util::bus::IEventBusPush::EventValue> dest;
            dest.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(tsSeconds[i], tsNanos[i]);
                setValue(&ev->data_value, arr[i]);
                dest.emplace_back(std::move(ev));
            }
            outEmitted += n;
            emitColumn(colName, std::move(dest));
        };

        switch (colCode)
        {
        case pvxs::TypeCode::BoolA:
            emitArray.template operator()<pvxs::shared_array<const bool>>(
                col,
                [](auto* dv, bool v)
                {
                    dv->set_booleanvalue(v);
                });
            break;
        case pvxs::TypeCode::Int8A:
        case pvxs::TypeCode::Int16A:
        case pvxs::TypeCode::Int32A:
            emitArray.template operator()<pvxs::shared_array<const int32_t>>(
                col,
                [](auto* dv, int32_t v)
                {
                    dv->set_intvalue(v);
                });
            break;
        case pvxs::TypeCode::Int64A:
            emitArray.template operator()<pvxs::shared_array<const int64_t>>(
                col,
                [](auto* dv, int64_t v)
                {
                    dv->set_longvalue(v);
                });
            break;
        case pvxs::TypeCode::UInt8A:
        case pvxs::TypeCode::UInt16A:
        case pvxs::TypeCode::UInt32A:
            emitArray.template operator()<pvxs::shared_array<const uint32_t>>(
                col,
                [](auto* dv, uint32_t v)
                {
                    dv->set_uintvalue(v);
                });
            break;
        case pvxs::TypeCode::UInt64A:
            emitArray.template operator()<pvxs::shared_array<const uint64_t>>(
                col,
                [](auto* dv, uint64_t v)
                {
                    dv->set_ulongvalue(v);
                });
            break;
        case pvxs::TypeCode::Float32A:
            emitArray.template operator()<pvxs::shared_array<const float>>(
                col,
                [](auto* dv, float v)
                {
                    dv->set_floatvalue(v);
                });
            break;
        case pvxs::TypeCode::Float64A:
            emitArray.template operator()<pvxs::shared_array<const double>>(
                col,
                [](auto* dv, double v)
                {
                    dv->set_doublevalue(v);
                });
            break;
        case pvxs::TypeCode::StringA:
            emitArray.template operator()<pvxs::shared_array<const std::string>>(
                col,
                [](auto* dv, const std::string& v)
                {
                    dv->set_stringvalue(v);
                });
            break;
        case pvxs::TypeCode::StructA:
        case pvxs::TypeCode::UnionA:
        case pvxs::TypeCode::AnyA:
            {
                const auto arr = col.as<pvxs::shared_array<const pvxs::Value>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                std::vector<mldp_pvxs_driver::util::bus::IEventBusPush::EventValue> dest;
                dest.reserve(n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto              ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(tsSeconds[i], tsNanos[i]);
                    const pvxs::Value cell = arr[i];
                    const pvxs::Value cellValue = (cell.type().kind() == pvxs::Kind::Compound) ? cell["value"] : pvxs::Value{};
                    EpicsMLDPConversion::convertPVToProtoValue(cellValue.valid() ? cellValue : cell, &ev->data_value);
                    dest.emplace_back(std::move(ev));
                }
                outEmitted += n;
                emitColumn(colName, std::move(dest));
            }
            break;
        default:
            tracef(log,
                   "NTTable row-ts PV {} column '{}' has unsupported type code {}",
                   tablePvName,
                   colName,
                   static_cast<int>(colCode));
            break;
        }
    }

    return outEmitted > 0;
}
