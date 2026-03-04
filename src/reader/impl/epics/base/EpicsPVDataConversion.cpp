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
#include <sstream>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using mldp_pvxs_driver::util::bus::EventValueStruct;
using mldp_pvxs_driver::util::bus::IDataBus;
using DataFrame = dp::service::common::DataFrame;

namespace {

// Appends a single-element typed column to frame.
template <typename T>
void setScalarValue(DataFrame* frame, const T& value, const std::string& name)
{
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, epics::pvData::boolean>)
    {
        auto* c = frame->add_boolcolumns();
        c->set_name(name);
        c->add_values(static_cast<bool>(value));
    }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T> && sizeof(T) <= 4)
    {
        auto* c = frame->add_int32columns();
        c->set_name(name);
        c->add_values(static_cast<int32_t>(value));
    }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
    {
        auto* c = frame->add_int64columns();
        c->set_name(name);
        c->add_values(static_cast<int64_t>(value));
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) <= 4)
    {
        auto* c = frame->add_int32columns();
        c->set_name(name);
        c->add_values(static_cast<int32_t>(value));
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
    {
        auto* c = frame->add_int64columns();
        c->set_name(name);
        c->add_values(static_cast<int64_t>(value));
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        auto* c = frame->add_floatcolumns();
        c->set_name(name);
        c->add_values(value);
    }
    else if constexpr (std::is_same_v<T, double>)
    {
        auto* c = frame->add_doublecolumns();
        c->set_name(name);
        c->add_values(value);
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        auto* c = frame->add_stringcolumns();
        c->set_name(name);
        c->add_values(value);
    }
}

// Appends a multi-element typed column to frame.
template <typename T>
void setArrayValues(DataFrame* frame, const ::epics::pvData::shared_vector<const T>& values, const std::string& name)
{
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, epics::pvData::boolean>)
    {
        auto* c = frame->add_boolarraycolumns();
        c->set_name(name);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(values.size()));
        c->mutable_values()->Reserve(static_cast<int>(values.size()));
        for (const auto& v : values) c->add_values(static_cast<bool>(v));
    }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T> && sizeof(T) <= 4)
    {
        auto* c = frame->add_int32arraycolumns();
        c->set_name(name);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(values.size()));
        c->mutable_values()->Reserve(static_cast<int>(values.size()));
        for (const auto& v : values) c->add_values(static_cast<int32_t>(v));
    }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
    {
        auto* c = frame->add_int64arraycolumns();
        c->set_name(name);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(values.size()));
        c->mutable_values()->Reserve(static_cast<int>(values.size()));
        for (const auto& v : values) c->add_values(static_cast<int64_t>(v));
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) <= 4)
    {
        auto* c = frame->add_int32arraycolumns();
        c->set_name(name);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(values.size()));
        c->mutable_values()->Reserve(static_cast<int>(values.size()));
        for (const auto& v : values) c->add_values(static_cast<int32_t>(v));
    }
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
    {
        auto* c = frame->add_int64arraycolumns();
        c->set_name(name);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(values.size()));
        c->mutable_values()->Reserve(static_cast<int>(values.size()));
        for (const auto& v : values) c->add_values(static_cast<int64_t>(v));
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        auto* c = frame->add_floatarraycolumns();
        c->set_name(name);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(values.size()));
        c->mutable_values()->Reserve(static_cast<int>(values.size()));
        for (const auto& v : values) c->add_values(v);
    }
    else if constexpr (std::is_same_v<T, double>)
    {
        auto* c = frame->add_doublearraycolumns();
        c->set_name(name);
        c->mutable_dimensions()->add_dims(static_cast<uint32_t>(values.size()));
        c->mutable_values()->Reserve(static_cast<int>(values.size()));
        for (const auto& v : values) c->add_values(v);
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        auto* c = frame->add_datacolumns();
        c->set_name(name);
        auto* sample = c->add_datavalues();
        auto* list = sample->mutable_arrayvalue();
        for (const auto& v : values) list->add_datavalues()->set_stringvalue(v);
    }
}

void convertStructure(const ::epics::pvData::PVStructure& pvStruct, DataFrame* frame, const std::string& prefix)
{
    const auto fieldNames = pvStruct.getStructure()->getFieldNames();
    const auto fields = pvStruct.getPVFields();
    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (!fields[i])
        {
            continue;
        }
        EpicsPVDataConversion::convertPVToProtoValue(*fields[i], frame, prefix + "." + fieldNames[i]);
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
    std::string                                                          name;
    std::vector<IDataBus::EventValue> events;
    size_t                                                               emitted{0};
};

ColumnResult convertColumn(const ::epics::pvData::PVFieldPtr& col,
                           const std::string&              colName,
                           size_t                          rowCount,
                           const std::vector<uint64_t>&    tsSeconds,
                           const std::vector<uint64_t>&    tsNanos)
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
        const auto emitArray = [&](auto view, auto&& setValue)
        {
            const auto n = std::min(rowCount, static_cast<size_t>(view.size()));
            auto bulk = std::make_shared<std::vector<EventValueStruct>>(n);
            for (size_t i = 0; i < n; ++i)
            {
                auto& ev = (*bulk)[i];
                ev.epoch_seconds = tsSeconds[i];
                ev.nanoseconds = tsNanos[i];
                setValue(&ev.data_value, view[i]);
            }
            result.events.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                result.events.emplace_back(bulk, &(*bulk)[i]);
            }
            result.emitted = n;
        };

        switch (scalarArray->getScalarArray()->getElementType())
        {
        case ::epics::pvData::pvBoolean:
            {
                ::epics::pvData::shared_vector<const epics::pvData::boolean> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataFrame* df, epics::pvData::boolean v) {
                    setScalarValue(df, v, colName);
                });
            }
            return result;
        case ::epics::pvData::pvByte:
        case ::epics::pvData::pvShort:
        case ::epics::pvData::pvInt:
            {
                ::epics::pvData::shared_vector<const int32_t> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataFrame* df, int32_t v) {
                    setScalarValue(df, v, colName);
                });
            }
            return result;
        case ::epics::pvData::pvLong:
            {
                ::epics::pvData::shared_vector<const int64_t> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataFrame* df, int64_t v) {
                    setScalarValue(df, v, colName);
                });
            }
            return result;
        case ::epics::pvData::pvUByte:
        case ::epics::pvData::pvUShort:
        case ::epics::pvData::pvUInt:
            {
                ::epics::pvData::shared_vector<const uint32_t> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataFrame* df, uint32_t v) {
                    setScalarValue(df, v, colName);
                });
            }
            return result;
        case ::epics::pvData::pvULong:
            {
                ::epics::pvData::shared_vector<const uint64_t> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataFrame* df, uint64_t v) {
                    setScalarValue(df, v, colName);
                });
            }
            return result;
        case ::epics::pvData::pvFloat:
            {
                ::epics::pvData::shared_vector<const float> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataFrame* df, float v) {
                    setScalarValue(df, v, colName);
                });
            }
            return result;
        case ::epics::pvData::pvDouble:
            {
                ::epics::pvData::shared_vector<const double> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataFrame* df, double v) {
                    setScalarValue(df, v, colName);
                });
            }
            return result;
        case ::epics::pvData::pvString:
            {
                ::epics::pvData::shared_vector<const std::string> view;
                scalarArray->getAs(view);
                emitArray(view, [&colName](DataFrame* df, const std::string& v) {
                    setScalarValue(df, v, colName);
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
        auto bulk = std::make_shared<std::vector<EventValueStruct>>(n);
        for (size_t i = 0; i < n; ++i)
        {
            auto& ev = (*bulk)[i];
            ev.epoch_seconds = tsSeconds[i];
            ev.nanoseconds = tsNanos[i];
            if (view[i])
            {
                EpicsPVDataConversion::convertPVToProtoValue(*view[i], &ev.data_value, colName);
            }
        }
        result.events.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            result.events.emplace_back(bulk, &(*bulk)[i]);
        }
        result.emitted = n;
    }

    return result;
}

} // namespace

void EpicsPVDataConversion::convertPVToProtoValue(const ::epics::pvData::PVField& pvField,
                                                   DataFrame*                      frame,
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
            case ::epics::pvData::pvBoolean: setScalarValue(frame, pvScalar.getAs<::epics::pvData::boolean>(), columnName); return;
            case ::epics::pvData::pvByte:
            case ::epics::pvData::pvShort:
            case ::epics::pvData::pvInt: setScalarValue(frame, pvScalar.getAs<int32_t>(), columnName); return;
            case ::epics::pvData::pvLong: setScalarValue(frame, pvScalar.getAs<int64_t>(), columnName); return;
            case ::epics::pvData::pvUByte:
            case ::epics::pvData::pvUShort:
            case ::epics::pvData::pvUInt: setScalarValue(frame, pvScalar.getAs<uint32_t>(), columnName); return;
            case ::epics::pvData::pvULong: setScalarValue(frame, pvScalar.getAs<uint64_t>(), columnName); return;
            case ::epics::pvData::pvFloat: setScalarValue(frame, pvScalar.getAs<float>(), columnName); return;
            case ::epics::pvData::pvDouble: setScalarValue(frame, pvScalar.getAs<double>(), columnName); return;
            case ::epics::pvData::pvString: setScalarValue(frame, pvScalar.getAs<std::string>(), columnName); return;
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
                    setArrayValues(frame, view, columnName);
                }
                return;
            case ::epics::pvData::pvByte:
            case ::epics::pvData::pvShort:
            case ::epics::pvData::pvInt:
                {
                    ::epics::pvData::shared_vector<const int32_t> view;
                    pvArray.getAs(view);
                    setArrayValues(frame, view, columnName);
                }
                return;
            case ::epics::pvData::pvLong:
                {
                    ::epics::pvData::shared_vector<const int64_t> view;
                    pvArray.getAs(view);
                    setArrayValues(frame, view, columnName);
                }
                return;
            case ::epics::pvData::pvUByte:
            case ::epics::pvData::pvUShort:
            case ::epics::pvData::pvUInt:
                {
                    ::epics::pvData::shared_vector<const uint32_t> view;
                    pvArray.getAs(view);
                    setArrayValues(frame, view, columnName);
                }
                return;
            case ::epics::pvData::pvULong:
                {
                    ::epics::pvData::shared_vector<const uint64_t> view;
                    pvArray.getAs(view);
                    setArrayValues(frame, view, columnName);
                }
                return;
            case ::epics::pvData::pvFloat:
                {
                    ::epics::pvData::shared_vector<const float> view;
                    pvArray.getAs(view);
                    setArrayValues(frame, view, columnName);
                }
                return;
            case ::epics::pvData::pvDouble:
                {
                    ::epics::pvData::shared_vector<const double> view;
                    pvArray.getAs(view);
                    setArrayValues(frame, view, columnName);
                }
                return;
            case ::epics::pvData::pvString:
                {
                    ::epics::pvData::shared_vector<const std::string> view;
                    pvArray.getAs(view);
                    setArrayValues(frame, view, columnName);
                }
                return;
            }
        }
        break;
    case ::epics::pvData::structure:
        convertStructure(static_cast<const ::epics::pvData::PVStructure&>(pvField), frame, columnName);
        return;
    case ::epics::pvData::structureArray:
        {
        const auto& pvStructArray = static_cast<const ::epics::pvData::PVStructureArray&>(pvField);
        const auto view = pvStructArray.view();
            for (const auto& entry : view)
            {
                if (entry)
                {
                    convertStructure(*entry, frame, columnName);
                }
            }
            return;
        }
    case ::epics::pvData::union_:
        {
        const auto& pvUnion = static_cast<const ::epics::pvData::PVUnion&>(pvField);
            const auto value = pvUnion.get();
            if (value)
            {
                convertPVToProtoValue(*value, frame, columnName);
                return;
            }
        }
        break;
    case ::epics::pvData::unionArray:
        {
        const auto& pvUnionArray = static_cast<const ::epics::pvData::PVUnionArray&>(pvField);
        const auto view = pvUnionArray.view();
            for (const auto& entry : view)
            {
                if (entry && entry->get())
                {
                    convertPVToProtoValue(*entry->get(), frame, columnName);
                }
            }
            return;
        }
    default:
        break;
    }

    std::ostringstream oss;
    pvField.dumpValue(oss);
    auto* c = frame->add_stringcolumns();
    c->set_name(columnName);
    c->add_values(oss.str());
}

bool EpicsPVDataConversion::tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&                    log,
                                                      const std::string&                                      tablePvName,
    const ::epics::pvData::PVStructurePtr&                    epicsValue,
                                                      const std::string&                                      tsSecondsField,
                                                      const std::string&                                      tsNanosField,
                                                      IDataBus::EventBatch* outBatch,
                                                      size_t&                                                 outEmitted)
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

bool EpicsPVDataConversion::tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                                      const std::string&                    tablePvName,
    const ::epics::pvData::PVStructurePtr&  epicsValue,
                                                      const std::string&                    tsSecondsField,
                                                      const std::string&                    tsNanosField,
                                                      ColumnEmitFn                          emitColumn,
                                                      size_t&                               outEmitted)
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
