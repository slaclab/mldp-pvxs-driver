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
#include <reader/impl/epics/pvxs/BSASEpicsMLDPConversion.h>

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
using namespace mldp_pvxs_driver::util::bus;
using mldp_pvxs_driver::util::bus::IDataBus;

namespace {

/*
 * Conversion flow overview (BSAS NTTable, row-timestamp mode):
 * 1) Validate the incoming PVXS value and locate the NTTable "value" struct.
 * 2) Resolve row timestamp arrays (seconds/nanoseconds), accepting either:
 *    - top-level fields, or
 *    - value.<field> columns.
 * 3) Build normalized uint64 vectors for the timestamp fields only
 *    (seconds/nanoseconds) so downstream row indexing is uniform regardless
 *    of original integer width/signedness.
 * 4) For each non-timestamp column:
 *    - convert supported NTTable array types into one DataBatch payload,
 *    - attach the full row timestamp list to that batch,
 *    - emit the result under the column name.
 * 5) Report total emitted rows across all converted columns.
 */

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
    case pvxs::TypeCode::Int64A: return UIntArrayView{value.as<pvxs::shared_array<const int64_t>>()};
    case pvxs::TypeCode::UInt32A: return UIntArrayView{value.as<pvxs::shared_array<const uint32_t>>()};
    case pvxs::TypeCode::Int32A: return UIntArrayView{value.as<pvxs::shared_array<const int32_t>>()};
    default: return std::nullopt;
    }
}

/// Result of converting a single NTTable column to DataBatch payload(s).
struct ColumnResult
{
    std::string                    name;
    std::vector<DataBatch>         events; ///< At most one element (all rows packed).
    size_t                         emitted{0};
};

/// Fill per-row timestamps into a DataBatch's timestamps vector.
void fillTimestamps(DataBatch&                   batch,
                    size_t                       n,
                    const std::vector<uint64_t>& tsSeconds,
                    const std::vector<uint64_t>& tsNanos)
{
    batch.timestamps.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        batch.timestamps.push_back(TimestampEntry{tsSeconds[i], tsNanos[i]});
    }
}

/// Convert one NTTable column into a single DataBatch that contains
/// all @p rowCount timestamped values in the appropriate typed column.
///
/// Compound column types (StructA/UnionA/AnyA) are supported: each cell's nested
/// "value" field (or the cell itself) is recursively converted via EpicsMLDPConversion.
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

    // Generic helper: builds a DataBatch with one typed DataColumn from a PVXS
    // shared_array. Iterates the array and casts each element via CastFn.
    const auto emitTypedColumn =
        [&]<typename ArrT, typename OutT>(auto castFn)
    {
        const auto arr = col.as<pvxs::shared_array<const ArrT>>();
        const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
        if (n == 0)
        {
            return;
        }

        DataBatch batch;
        fillTimestamps(batch, n, tsSeconds, tsNanos);

        std::vector<OutT> vals;
        vals.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            vals.push_back(castFn(arr[i]));
        }

        DataColumn dc;
        dc.name   = colName;
        dc.values = std::move(vals);
        batch.columns.push_back(std::move(dc));

        result.events.push_back(std::move(batch));
        result.emitted = n;
    };

    // Identity cast helper.
    const auto identity = [](auto v) { return v; };

    switch (colCode)
    {
    case pvxs::TypeCode::BoolA:
        emitTypedColumn.template operator()<bool, bool>(identity);
        break;

    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::Int32A:
        emitTypedColumn.template operator()<int32_t, int32_t>(identity);
        break;

    case pvxs::TypeCode::Int64A:
        emitTypedColumn.template operator()<int64_t, int64_t>(identity);
        break;

    case pvxs::TypeCode::UInt8A:
    case pvxs::TypeCode::UInt16A:
    case pvxs::TypeCode::UInt32A:
        emitTypedColumn.template operator()<uint32_t, int32_t>(
            [](uint32_t v) { return static_cast<int32_t>(v); });
        break;

    case pvxs::TypeCode::UInt64A:
        emitTypedColumn.template operator()<uint64_t, int64_t>(
            [](uint64_t v) { return static_cast<int64_t>(v); });
        break;

    case pvxs::TypeCode::Float32A:
        emitTypedColumn.template operator()<float, float>(identity);
        break;

    case pvxs::TypeCode::Float64A:
        emitTypedColumn.template operator()<double, double>(identity);
        break;

    case pvxs::TypeCode::StringA:
        {
            const auto arr = col.as<pvxs::shared_array<const std::string>>();
            const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
            if (n > 0)
            {
                DataBatch batch;
                fillTimestamps(batch, n, tsSeconds, tsNanos);

                std::vector<std::string> vals(arr.begin(), arr.begin() + static_cast<ptrdiff_t>(n));
                DataColumn dc;
                dc.name   = colName;
                dc.values = std::move(vals);
                batch.columns.push_back(std::move(dc));

                result.events.push_back(std::move(batch));
                result.emitted = n;
            }
            break;
        }

    case pvxs::TypeCode::StructA:
    case pvxs::TypeCode::UnionA:
    case pvxs::TypeCode::AnyA:
        {
            // Compound array columns: convert each cell's value fields into the
            // same DataBatch, using EpicsMLDPConversion for the actual type dispatch.
            const auto arr = col.as<pvxs::shared_array<const pvxs::Value>>();
            const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
            if (n > 0)
            {
                DataBatch batch;
                fillTimestamps(batch, n, tsSeconds, tsNanos);

                for (size_t i = 0; i < n; ++i)
                {
                    const pvxs::Value cell = arr[i];
                    const pvxs::Value cellValue =
                        (cell.valid() && cell.type().kind() == pvxs::Kind::Compound)
                            ? cell["value"]
                            : pvxs::Value{};
                    EpicsMLDPConversion::convertPVToDataBatch(
                        cellValue.valid() ? cellValue : cell, &batch, colName);
                }

                result.events.push_back(std::move(batch));
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
    outBatch->frames.clear();
    outBatch->tags.push_back(tablePvName);
    return tryBuildNtTableRowTsBatch(log, tablePvName, epicsValue, tsSecondsField, tsNanosField,
                                     [&](std::string colName, std::vector<DataBatch> batches)
                                     {
                                         (void)colName;
                                         for (auto& b : batches)
                                             outBatch->frames.push_back(std::move(b));
                                     },
                                     outEmitted);
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

    // Phase 1: Validate and locate NTTable payload.
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

    // Phase 2: Resolve row timestamp arrays (top-level first, then value.* fallback).
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

    // Phase 3: Normalize timestamp arrays to uint64 and clamp row count to the
    // shortest timestamp array to avoid out-of-bounds row access.
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
        tsNanos[i] = nanosArr->at(i);
    }

    // Phase 4: Convert each non-timestamp column and emit it independently.
    // One emitted entry contains one DataBatch with all rows of that column.
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

    // Phase 5: report whether any rows were emitted.
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

    // Same validation/resolution flow as the sequential overload.
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

    // Build shared timestamp arrays so parallel tasks can safely reference them.
    auto tsSeconds = std::make_shared<std::vector<uint64_t>>(rowCount);
    auto tsNanos = std::make_shared<std::vector<uint64_t>>(rowCount);
    for (size_t i = 0; i < rowCount; ++i)
    {
        (*tsSeconds)[i] = secondsArr->at(i);
        (*tsNanos)[i] = nanosArr->at(i);
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

    // Collect futures and emit sequentially to keep callback side effects ordered
    // at this boundary (conversion work itself is parallelized).
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
