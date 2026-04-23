//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/base/EpicsPVDataConversion.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::bus;
using mldp_pvxs_driver::util::bus::IDataBus;

namespace {

// ---------------------------------------------------------------------------
// Scalar helpers — append one DataColumn with a single typed value to batch.
// ---------------------------------------------------------------------------

template <typename T>
void setScalarValue(DataBatch* batch, const T& value, const std::string& name)
{
    DataColumn col;
    col.name = name;

    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, epics::pvData::boolean>)
    {
        col.values = std::vector<bool>{static_cast<bool>(value)};
    }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T> && sizeof(T) <= 4)
    {
        col.values = std::vector<int32_t>{static_cast<int32_t>(value)};
    }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
    {
        col.values = std::vector<int64_t>{static_cast<int64_t>(value)};
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) <= 4)
    {
        col.values = std::vector<int32_t>{static_cast<int32_t>(value)};
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
    {
        col.values = std::vector<int64_t>{static_cast<int64_t>(value)};
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        col.values = std::vector<float>{value};
    }
    else if constexpr (std::is_same_v<T, double>)
    {
        col.values = std::vector<double>{value};
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        col.values = std::vector<std::string>{value};
    }

    batch->columns.push_back(std::move(col));
}

// ---------------------------------------------------------------------------
// Array helpers — append one DataColumn with a vector-of-one-inner-vector and
// record array_dims so consumers know the shape.
// ---------------------------------------------------------------------------

template <typename T>
void setArrayValues(DataBatch*                                              batch,
                    const ::epics::pvData::shared_vector<const T>&         values,
                    const std::string&                                      name)
{
    DataColumn col;
    col.name = name;

    const auto sz = static_cast<uint32_t>(values.size());

    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, epics::pvData::boolean>)
    {
        std::vector<bool> inner;
        inner.reserve(values.size());
        for (const auto& v : values)
            inner.push_back(static_cast<bool>(v));
        col.values = std::vector<std::vector<bool>>{std::move(inner)};
    }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T> && sizeof(T) <= 4)
    {
        std::vector<int32_t> inner;
        inner.reserve(values.size());
        for (const auto& v : values)
            inner.push_back(static_cast<int32_t>(v));
        col.values = std::vector<std::vector<int32_t>>{std::move(inner)};
    }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
    {
        std::vector<int64_t> inner;
        inner.reserve(values.size());
        for (const auto& v : values)
            inner.push_back(static_cast<int64_t>(v));
        col.values = std::vector<std::vector<int64_t>>{std::move(inner)};
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) <= 4)
    {
        std::vector<int32_t> inner;
        inner.reserve(values.size());
        for (const auto& v : values)
            inner.push_back(static_cast<int32_t>(v));
        col.values = std::vector<std::vector<int32_t>>{std::move(inner)};
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
    {
        std::vector<int64_t> inner;
        inner.reserve(values.size());
        for (const auto& v : values)
            inner.push_back(static_cast<int64_t>(v));
        col.values = std::vector<std::vector<int64_t>>{std::move(inner)};
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        std::vector<float> inner(values.begin(), values.end());
        col.values = std::vector<std::vector<float>>{std::move(inner)};
    }
    else if constexpr (std::is_same_v<T, double>)
    {
        std::vector<double> inner(values.begin(), values.end());
        col.values = std::vector<std::vector<double>>{std::move(inner)};
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        // String arrays: store as flat std::vector<std::string> with dims recorded.
        std::vector<std::string> inner(values.begin(), values.end());
        col.values = std::move(inner);
        batch->columns.push_back(std::move(col));
        batch->array_dims[name] = ArrayDims{{sz}};
        return;
    }

    batch->columns.push_back(std::move(col));
    batch->array_dims[name] = ArrayDims{{sz}};
}

void convertStructure(const ::epics::pvData::PVStructure& pvStruct,
                      DataBatch*                          batch,
                      const std::string&                  prefix)
{
    const auto fieldNames = pvStruct.getStructure()->getFieldNames();
    const auto fields = pvStruct.getPVFields();
    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (!fields[i])
        {
            continue;
        }
        EpicsPVDataConversion::convertPVToDataBatch(*fields[i], batch, prefix + "." + fieldNames[i]);
    }
}

struct UIntArrayView
{
    std::variant<::epics::pvData::shared_vector<const uint64_t>,
                 ::epics::pvData::shared_vector<const int64_t>,
                 ::epics::pvData::shared_vector<const uint32_t>,
                 ::epics::pvData::shared_vector<const int32_t>>
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

std::optional<UIntArrayView> asUIntArrayView(const ::epics::pvData::PVFieldPtr& field)
{
    if (!field)
    {
        return std::nullopt;
    }
    const auto scalarArray = std::dynamic_pointer_cast<::epics::pvData::PVScalarArray>(field);
    if (!scalarArray)
    {
        return std::nullopt;
    }

    switch (scalarArray->getScalarArray()->getElementType())
    {
    case ::epics::pvData::pvUInt:
        {
            ::epics::pvData::shared_vector<const uint32_t> view;
            scalarArray->getAs(view);
            return UIntArrayView{view};
        }
    case ::epics::pvData::pvULong:
        {
            ::epics::pvData::shared_vector<const uint64_t> view;
            scalarArray->getAs(view);
            return UIntArrayView{view};
        }
    case ::epics::pvData::pvInt:
        {
            ::epics::pvData::shared_vector<const int32_t> view;
            scalarArray->getAs(view);
            return UIntArrayView{view};
        }
    case ::epics::pvData::pvLong:
        {
            ::epics::pvData::shared_vector<const int64_t> view;
            scalarArray->getAs(view);
            return UIntArrayView{view};
        }
    default: return std::nullopt;
    }
}

struct ColumnResult
{
    std::string            name;
    std::vector<DataBatch> events;
    size_t                 emitted{0};
};

ColumnResult convertColumn(const ::epics::pvData::PVFieldPtr& col,
                           const std::string&                 colName,
                           size_t                             rowCount,
                           const std::vector<uint64_t>&       tsSeconds,
                           const std::vector<uint64_t>&       tsNanos)
{
    ColumnResult result;
    result.name = colName;
    if (!col)
    {
        return result;
    }

    const auto scalarArray = std::dynamic_pointer_cast<::epics::pvData::PVScalarArray>(col);
    if (scalarArray)
    {
        // Each row becomes its own DataBatch with a single timestamp and one scalar column.
        const auto emitArray = [&](auto view, auto&& setValue)
        {
            const auto n = std::min(rowCount, static_cast<size_t>(view.size()));
            result.events.reserve(result.events.size() + n);
            for (size_t i = 0; i < n; ++i)
            {
                DataBatch batch;
                batch.timestamps.push_back(TimestampEntry{tsSeconds[i], tsNanos[i]});
                setValue(&batch, view[i]);
                result.events.emplace_back(std::move(batch));
            }
            result.emitted = n;
        };

        switch (scalarArray->getScalarArray()->getElementType())
        {
        case ::epics::pvData::pvBoolean:
            {
                ::epics::pvData::shared_vector<const epics::pvData::boolean> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataBatch* db, epics::pvData::boolean v)
                          {
                              setScalarValue(db, v, colName);
                          });
            }
            return result;
        case ::epics::pvData::pvByte:
        case ::epics::pvData::pvShort:
        case ::epics::pvData::pvInt:
            {
                ::epics::pvData::shared_vector<const int32_t> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataBatch* db, int32_t v)
                          {
                              setScalarValue(db, v, colName);
                          });
            }
            return result;
        case ::epics::pvData::pvLong:
            {
                ::epics::pvData::shared_vector<const int64_t> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataBatch* db, int64_t v)
                          {
                              setScalarValue(db, v, colName);
                          });
            }
            return result;
        case ::epics::pvData::pvUByte:
        case ::epics::pvData::pvUShort:
        case ::epics::pvData::pvUInt:
            {
                ::epics::pvData::shared_vector<const uint32_t> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataBatch* db, uint32_t v)
                          {
                              setScalarValue(db, v, colName);
                          });
            }
            return result;
        case ::epics::pvData::pvULong:
            {
                ::epics::pvData::shared_vector<const uint64_t> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataBatch* db, uint64_t v)
                          {
                              setScalarValue(db, v, colName);
                          });
            }
            return result;
        case ::epics::pvData::pvFloat:
            {
                ::epics::pvData::shared_vector<const float> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataBatch* db, float v)
                          {
                              setScalarValue(db, v, colName);
                          });
            }
            return result;
        case ::epics::pvData::pvDouble:
            {
                ::epics::pvData::shared_vector<const double> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataBatch* db, double v)
                          {
                              setScalarValue(db, v, colName);
                          });
            }
            return result;
        case ::epics::pvData::pvString:
            {
                ::epics::pvData::shared_vector<const std::string> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataBatch* db, const std::string& v)
                          {
                              setScalarValue(db, v, colName);
                          });
            }
            return result;
        default:
            return result;
        }
    }

    const auto structArray = std::dynamic_pointer_cast<::epics::pvData::PVStructureArray>(col);
    if (structArray)
    {
        const auto view = structArray->view();
        const auto n = std::min(rowCount, static_cast<size_t>(view.size()));
        result.events.reserve(result.events.size() + n);
        for (size_t i = 0; i < n; ++i)
        {
            DataBatch batch;
            batch.timestamps.push_back(TimestampEntry{tsSeconds[i], tsNanos[i]});
            if (view[i])
            {
                EpicsPVDataConversion::convertPVToDataBatch(*view[i], &batch, colName);
            }
            result.events.emplace_back(std::move(batch));
        }
        result.emitted = n;
    }

    return result;
}

} // namespace

void EpicsPVDataConversion::convertPVToDataBatch(const ::epics::pvData::PVField& pvField,
                                                 DataBatch*                      batch,
                                                 const std::string&              columnName)
{
    const auto fieldType = pvField.getField()->getType();
    switch (fieldType)
    {
    case ::epics::pvData::scalar:
        {
            const auto& pvScalar = static_cast<const ::epics::pvData::PVScalar&>(pvField);
            switch (pvScalar.getScalar()->getScalarType())
            {
            case ::epics::pvData::pvBoolean: setScalarValue(batch, pvScalar.getAs<::epics::pvData::boolean>(), columnName); return;
            case ::epics::pvData::pvByte:
            case ::epics::pvData::pvShort:
            case ::epics::pvData::pvInt: setScalarValue(batch, pvScalar.getAs<int32_t>(), columnName); return;
            case ::epics::pvData::pvLong: setScalarValue(batch, pvScalar.getAs<int64_t>(), columnName); return;
            case ::epics::pvData::pvUByte:
            case ::epics::pvData::pvUShort:
            case ::epics::pvData::pvUInt: setScalarValue(batch, pvScalar.getAs<uint32_t>(), columnName); return;
            case ::epics::pvData::pvULong: setScalarValue(batch, pvScalar.getAs<uint64_t>(), columnName); return;
            case ::epics::pvData::pvFloat: setScalarValue(batch, pvScalar.getAs<float>(), columnName); return;
            case ::epics::pvData::pvDouble: setScalarValue(batch, pvScalar.getAs<double>(), columnName); return;
            case ::epics::pvData::pvString: setScalarValue(batch, pvScalar.getAs<std::string>(), columnName); return;
            }
        }
        break;
    case ::epics::pvData::scalarArray:
        {
            const auto& pvArray = static_cast<const ::epics::pvData::PVScalarArray&>(pvField);
            switch (pvArray.getScalarArray()->getElementType())
            {
            case ::epics::pvData::pvBoolean:
                {
                    ::epics::pvData::shared_vector<const ::epics::pvData::boolean> view;
                    pvArray.getAs(view);
                    setArrayValues(batch, view, columnName);
                }
                return;
            case ::epics::pvData::pvByte:
            case ::epics::pvData::pvShort:
            case ::epics::pvData::pvInt:
                {
                    ::epics::pvData::shared_vector<const int32_t> view;
                    pvArray.getAs(view);
                    setArrayValues(batch, view, columnName);
                }
                return;
            case ::epics::pvData::pvLong:
                {
                    ::epics::pvData::shared_vector<const int64_t> view;
                    pvArray.getAs(view);
                    setArrayValues(batch, view, columnName);
                }
                return;
            case ::epics::pvData::pvUByte:
            case ::epics::pvData::pvUShort:
            case ::epics::pvData::pvUInt:
                {
                    ::epics::pvData::shared_vector<const uint32_t> view;
                    pvArray.getAs(view);
                    setArrayValues(batch, view, columnName);
                }
                return;
            case ::epics::pvData::pvULong:
                {
                    ::epics::pvData::shared_vector<const uint64_t> view;
                    pvArray.getAs(view);
                    setArrayValues(batch, view, columnName);
                }
                return;
            case ::epics::pvData::pvFloat:
                {
                    ::epics::pvData::shared_vector<const float> view;
                    pvArray.getAs(view);
                    setArrayValues(batch, view, columnName);
                }
                return;
            case ::epics::pvData::pvDouble:
                {
                    ::epics::pvData::shared_vector<const double> view;
                    pvArray.getAs(view);
                    setArrayValues(batch, view, columnName);
                }
                return;
            case ::epics::pvData::pvString:
                {
                    ::epics::pvData::shared_vector<const std::string> view;
                    pvArray.getAs(view);
                    setArrayValues(batch, view, columnName);
                }
                return;
            }
        }
        break;
    case ::epics::pvData::structure:
        convertStructure(static_cast<const ::epics::pvData::PVStructure&>(pvField), batch, columnName);
        return;
    case ::epics::pvData::structureArray:
        {
            const auto& pvStructArray = static_cast<const ::epics::pvData::PVStructureArray&>(pvField);
            const auto  view = pvStructArray.view();
            for (const auto& entry : view)
            {
                if (entry)
                {
                    convertStructure(*entry, batch, columnName);
                }
            }
            return;
        }
    case ::epics::pvData::union_:
        {
            const auto& pvUnion = static_cast<const ::epics::pvData::PVUnion&>(pvField);
            const auto  value = pvUnion.get();
            if (value)
            {
                convertPVToDataBatch(*value, batch, columnName);
                return;
            }
        }
        break;
    case ::epics::pvData::unionArray:
        {
            const auto& pvUnionArray = static_cast<const ::epics::pvData::PVUnionArray&>(pvField);
            const auto  view = pvUnionArray.view();
            for (const auto& entry : view)
            {
                if (entry && entry->get())
                {
                    convertPVToDataBatch(*entry->get(), batch, columnName);
                }
            }
            return;
        }
    default:
        break;
    }

    // Fallback: dump the field value as a string column.
    std::ostringstream oss;
    pvField.dumpValue(oss);
    DataColumn col;
    col.name   = columnName;
    col.values = std::vector<std::string>{oss.str()};
    batch->columns.push_back(std::move(col));
}

bool EpicsPVDataConversion::tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&  log,
                                                      const std::string&                     tablePvName,
                                                      const ::epics::pvData::PVStructurePtr& epicsValue,
                                                      const std::string&                     tsSecondsField,
                                                      const std::string&                     tsNanosField,
                                                      IDataBus::EventBatch*                  outBatch,
                                                      size_t&                                outEmitted)
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

bool EpicsPVDataConversion::tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&  log,
                                                      const std::string&                     tablePvName,
                                                      const ::epics::pvData::PVStructurePtr& epicsValue,
                                                      const std::string&                     tsSecondsField,
                                                      const std::string&                     tsNanosField,
                                                      ColumnEmitFn                           emitColumn,
                                                      size_t&                                outEmitted)
{
    outEmitted = 0;

    if (!epicsValue)
    {
        warnf(log, "NTTable row-ts PV {} update is null", tablePvName);
        return false;
    }

    auto columns = epicsValue->getSubField<::epics::pvData::PVStructure>("value");
    if (!columns)
    {
        warnf(log, "NTTable row-ts PV {} has no usable value struct", tablePvName);
        return false;
    }

    auto secondsField = epicsValue->getSubField(tsSecondsField);
    auto nanosField = epicsValue->getSubField(tsNanosField);
    if (!secondsField || !nanosField)
    {
        secondsField = columns->getSubField(tsSecondsField);
        nanosField = columns->getSubField(tsNanosField);
    }

    const auto secondsArr = asUIntArrayView(secondsField);
    const auto nanosArr = asUIntArrayView(nanosField);
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

    std::vector<uint64_t> tsSeconds(rowCount);
    std::vector<uint64_t> tsNanos(rowCount);
    for (size_t i = 0; i < rowCount; ++i)
    {
        tsSeconds[i] = secondsArr->at(i);
        tsNanos[i] = nanosArr->at(i);
    }

    const auto fieldNames = columns->getStructure()->getFieldNames();
    const auto fields = columns->getPVFields();
    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (!fields[i])
        {
            continue;
        }
        const auto colResult = convertColumn(fields[i], fieldNames[i], rowCount, tsSeconds, tsNanos);
        if (!colResult.events.empty())
        {
            emitColumn(colResult.name, colResult.events);
            outEmitted += colResult.emitted;
        }
    }

    return outEmitted > 0;
}
