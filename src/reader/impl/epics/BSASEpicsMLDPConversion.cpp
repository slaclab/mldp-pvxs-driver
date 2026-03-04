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
using mldp_pvxs_driver::util::bus::IDataBus;

namespace {

/// Helper to interpret integer PVXS array fields (of any width) as uint64.
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
    case pvxs::TypeCode::Int64A:  return UIntArrayView{value.as<pvxs::shared_array<const int64_t>>()};
    case pvxs::TypeCode::UInt32A: return UIntArrayView{value.as<pvxs::shared_array<const uint32_t>>()};
    case pvxs::TypeCode::Int32A:  return UIntArrayView{value.as<pvxs::shared_array<const int32_t>>()};
    default:                      return std::nullopt;
    }
}

/// Result of converting a single NTTable column to a DataFrame-based EventValue.
struct ColumnResult
{
    std::string                       name;
    std::vector<IDataBus::EventValue> events;  ///< At most one element (all rows packed).
    size_t                            emitted{0};
};

/// Fill per-row timestamps into a DataFrame's TimestampList.
void fillTimestamps(dp::service::common::DataFrame& frame,
                    size_t                          n,
                    const std::vector<uint64_t>&    tsSeconds,
                    const std::vector<uint64_t>&    tsNanos)
{
    auto* tsList = frame.mutable_datatimestamps()->mutable_timestamplist();
    tsList->mutable_timestamps()->Reserve(static_cast<int>(n));
    for (size_t i = 0; i < n; ++i)
    {
        auto* ts = tsList->add_timestamps();
        ts->set_epochseconds(tsSeconds[i]);
        ts->set_nanoseconds(tsNanos[i]);
    }
}

/// Convert one NTTable column into a single EventValue whose DataFrame contains
/// all @p rowCount timestamped values in the appropriate typed column.
///
/// Compound column types (StructA/UnionA/AnyA) are not supported and produce an
/// empty ColumnResult.
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

    // Generic helper: fills a typed scalar column from a PVXS shared_array.
    // Calls addColFn() on the frame to add the typed column, then iterates
    // the array and appends each element via addValFn().
    const auto emitTypedColumn =
        [&]<typename ColT, typename ArrT, typename ValT>(ColT* (dp::service::common::DataFrame::*addColFn)(),
                                                          void (ColT::*addValFn)(ValT))
    {
        const auto arr = col.as<pvxs::shared_array<const ArrT>>();
        const auto n   = std::min(rowCount, static_cast<size_t>(arr.size()));
        if (n == 0)
        {
            return;
        }

        auto ev    = IDataBus::MakeEventValue(tsSeconds[0], tsNanos[0]);
        auto& frame = ev->data_value;

        fillTimestamps(frame, n, tsSeconds, tsNanos);

        auto* c = (frame.*addColFn)();
        c->set_name(colName);
        c->mutable_values()->Reserve(static_cast<int>(n));
        for (size_t i = 0; i < n; ++i)
        {
            (c->*addValFn)(static_cast<ValT>(arr[i]));
        }

        result.events.push_back(std::move(ev));
        result.emitted = n;
    };

    switch (colCode)
    {
    case pvxs::TypeCode::BoolA:
        emitTypedColumn.template operator()<dp::service::common::BoolColumn, bool, bool>(
            &dp::service::common::DataFrame::add_boolcolumns,
            &dp::service::common::BoolColumn::add_values);
        break;

    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::Int32A:
        emitTypedColumn.template operator()<dp::service::common::Int32Column, int32_t, int32_t>(
            &dp::service::common::DataFrame::add_int32columns,
            &dp::service::common::Int32Column::add_values);
        break;

    case pvxs::TypeCode::Int64A:
        emitTypedColumn.template operator()<dp::service::common::Int64Column, int64_t, int64_t>(
            &dp::service::common::DataFrame::add_int64columns,
            &dp::service::common::Int64Column::add_values);
        break;

    case pvxs::TypeCode::UInt8A:
    case pvxs::TypeCode::UInt16A:
    case pvxs::TypeCode::UInt32A:
        emitTypedColumn.template operator()<dp::service::common::Int32Column, uint32_t, int32_t>(
            &dp::service::common::DataFrame::add_int32columns,
            &dp::service::common::Int32Column::add_values);
        break;

    case pvxs::TypeCode::UInt64A:
        emitTypedColumn.template operator()<dp::service::common::Int64Column, uint64_t, int64_t>(
            &dp::service::common::DataFrame::add_int64columns,
            &dp::service::common::Int64Column::add_values);
        break;

    case pvxs::TypeCode::Float32A:
        emitTypedColumn.template operator()<dp::service::common::FloatColumn, float, float>(
            &dp::service::common::DataFrame::add_floatcolumns,
            &dp::service::common::FloatColumn::add_values);
        break;

    case pvxs::TypeCode::Float64A:
        emitTypedColumn.template operator()<dp::service::common::DoubleColumn, double, double>(
            &dp::service::common::DataFrame::add_doublecolumns,
            &dp::service::common::DoubleColumn::add_values);
        break;

    case pvxs::TypeCode::StringA:
    {
        const auto arr = col.as<pvxs::shared_array<const std::string>>();
        const auto n   = std::min(rowCount, static_cast<size_t>(arr.size()));
        if (n > 0)
        {
            auto ev    = IDataBus::MakeEventValue(tsSeconds[0], tsNanos[0]);
            auto& frame = ev->data_value;

            fillTimestamps(frame, n, tsSeconds, tsNanos);

            auto* c = frame.add_stringcolumns();
            c->set_name(colName);
            for (size_t i = 0; i < n; ++i)
            {
                c->add_values(arr[i]);
            }

            result.events.push_back(std::move(ev));
            result.emitted = n;
        }
        break;
    }

    case pvxs::TypeCode::StructA:
    case pvxs::TypeCode::UnionA:
    case pvxs::TypeCode::AnyA:
    {
        // Compound array columns: convert each cell's value fields into the
        // same DataFrame, using EpicsMLDPConversion for the actual type dispatch.
        const auto arr = col.as<pvxs::shared_array<const pvxs::Value>>();
        const auto n   = std::min(rowCount, static_cast<size_t>(arr.size()));
        if (n > 0)
        {
            auto ev    = IDataBus::MakeEventValue(tsSeconds[0], tsNanos[0]);
            auto& frame = ev->data_value;

            fillTimestamps(frame, n, tsSeconds, tsNanos);

            for (size_t i = 0; i < n; ++i)
            {
                const pvxs::Value cell = arr[i];
                const pvxs::Value cellValue =
                    (cell.valid() && cell.type().kind() == pvxs::Kind::Compound)
                        ? cell["value"]
                        : pvxs::Value{};
                EpicsMLDPConversion::convertPVToDataFrame(
                    cellValue.valid() ? cellValue : cell, &frame, colName);
            }

            result.events.push_back(std::move(ev));
            result.emitted = n;
        }
        break;
    }

    default:
        break;
    }

    (void)columns; // unused after nameOf is called in the callers
    return result;
}

} // namespace

bool BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                                        const std::string&                    tablePvName,
                                                        const pvxs::Value&                    epicsValue,
                                                        const std::string&                    tsSecondsField,
                                                        const std::string&                    tsNanosField,
                                                        IDataBus::EventBatch*                 outBatch,
                                                        size_t&                               outEmitted)
{
    outBatch->tags.clear();
    outBatch->values.clear();
    outBatch->tags.push_back(tablePvName);
    return tryBuildNtTableRowTsBatch(log, tablePvName, epicsValue,
        tsSecondsField, tsNanosField,
        [&](std::string colName, std::vector<IDataBus::EventValue> events) {
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
    pvxs::Value nanosValue   = epicsValue[tsNanosField];
    if (!secondsValue.valid() || !nanosValue.valid())
    {
        secondsValue = columns[tsSecondsField];
        nanosValue   = columns[tsNanosField];
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
    const auto nanosArr   = asUIntArrayView(nanosValue);
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
        else
        {
            const auto colCode = col.type().code;
            if (colCode != pvxs::TypeCode::BoolA &&
                colCode != pvxs::TypeCode::Int8A && colCode != pvxs::TypeCode::Int16A && colCode != pvxs::TypeCode::Int32A &&
                colCode != pvxs::TypeCode::Int64A &&
                colCode != pvxs::TypeCode::UInt8A && colCode != pvxs::TypeCode::UInt16A && colCode != pvxs::TypeCode::UInt32A &&
                colCode != pvxs::TypeCode::UInt64A &&
                colCode != pvxs::TypeCode::Float32A && colCode != pvxs::TypeCode::Float64A &&
                colCode != pvxs::TypeCode::StringA &&
                colCode != pvxs::TypeCode::StructA && colCode != pvxs::TypeCode::UnionA && colCode != pvxs::TypeCode::AnyA)
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
    pvxs::Value nanosValue   = epicsValue[tsNanosField];
    if (!secondsValue.valid() || !nanosValue.valid())
    {
        secondsValue = columns[tsSecondsField];
        nanosValue   = columns[tsNanosField];
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
    const auto nanosArr   = asUIntArrayView(nanosValue);
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

    // Build shared timestamp arrays so parallel tasks can safely reference them.
    auto tsSeconds = std::make_shared<std::vector<uint64_t>>(rowCount);
    auto tsNanos   = std::make_shared<std::vector<uint64_t>>(rowCount);
    for (size_t i = 0; i < rowCount; ++i)
    {
        (*tsSeconds)[i] = secondsArr->at(i);
        (*tsNanos)[i]   = nanosArr->at(i);
    }

    // Collect columns to dispatch in parallel.
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

    // Submit column conversions to the thread pool.
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

    // Collect results and emit sequentially.
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
