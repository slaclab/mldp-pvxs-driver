#include <reader/impl/epics/BSASEpicsMLDPConversion.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <spdlog/spdlog.h>
#include <type_traits>
#include <utility>
#include <variant>

using namespace mldp_pvxs_driver::reader::impl::epics;

namespace {
struct UIntArrayView
{
    std::variant<pvxs::shared_array<const uint64_t>,
                 pvxs::shared_array<const int64_t>,
                 pvxs::shared_array<const uint32_t>,
                 pvxs::shared_array<const int32_t>> data;

    size_t size() const
    {
        return std::visit([](const auto& arr) { return static_cast<size_t>(arr.size()); }, data);
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

bool BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(const std::string& tablePvName,
                                                       const pvxs::Value& epicsValue,
                                                       const std::string& tsSecondsField,
                                                       const std::string& tsNanosField,
                                                       mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch* outBatch,
                                                       size_t* outEmitted)
{
    if (!outBatch || !outEmitted)
    {
        return false;
    }

    *outEmitted = 0;
    outBatch->tags.clear();
    outBatch->values.clear();
    outBatch->tags.push_back(tablePvName);

    if (!epicsValue || epicsValue.type().kind() != pvxs::Kind::Compound)
    {
        spdlog::warn("NTTable row-ts PV {} update is not a compound value", tablePvName);
        return false;
    }

    // BSAS NTTable row-ts layout: timestamp arrays and sampled columns are expected
    // to exist at the top-level of the NTTable structure.
    const auto columns = epicsValue;
    if (!columns || columns.type().kind() != pvxs::Kind::Compound)
    {
        spdlog::warn("NTTable row-ts PV {} has no usable value struct", tablePvName);
        return false;
    }

    pvxs::Value secondsValue = columns[tsSecondsField];
    pvxs::Value nanosValue = columns[tsNanosField];

    if (!secondsValue.valid() || !nanosValue.valid())
    {
        spdlog::warn("NTTable row-ts PV {} missing timestamp arrays '{}'/'{}'", tablePvName, tsSecondsField, tsNanosField);
        return false;
    }

    const auto secondsArr = asUIntArrayView(secondsValue);
    const auto nanosArr = asUIntArrayView(nanosValue);
    if (!secondsArr.has_value() || !nanosArr.has_value())
    {
        spdlog::warn("NTTable row-ts PV {} timestamp arrays have unsupported types", tablePvName);
        return false;
    }

    const auto rowCount = std::min(secondsArr->size(), nanosArr->size());
    if (rowCount == 0)
    {
        spdlog::trace("NTTable row-ts PV {} has 0 timestamped rows", tablePvName);
        return false;
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

        // Emit one source per column; each row is a timestamped event.
        // We switch once per column for efficiency.
        switch (colCode)
        {
        case pvxs::TypeCode::BoolA:
            {
                const auto arr = col.as<pvxs::shared_array<const bool>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    ev->data_value->set_booleanvalue(arr[i]);
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        case pvxs::TypeCode::Int8A:
        case pvxs::TypeCode::Int16A:
        case pvxs::TypeCode::Int32A:
            {
                const auto arr = col.as<pvxs::shared_array<const int32_t>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    ev->data_value->set_intvalue(arr[i]);
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        case pvxs::TypeCode::Int64A:
            {
                const auto arr = col.as<pvxs::shared_array<const int64_t>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    ev->data_value->set_longvalue(arr[i]);
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        case pvxs::TypeCode::UInt8A:
        case pvxs::TypeCode::UInt16A:
        case pvxs::TypeCode::UInt32A:
            {
                const auto arr = col.as<pvxs::shared_array<const uint32_t>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    ev->data_value->set_uintvalue(arr[i]);
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        case pvxs::TypeCode::UInt64A:
            {
                const auto arr = col.as<pvxs::shared_array<const uint64_t>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    ev->data_value->set_ulongvalue(arr[i]);
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        case pvxs::TypeCode::Float32A:
            {
                const auto arr = col.as<pvxs::shared_array<const float>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    ev->data_value->set_floatvalue(arr[i]);
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        case pvxs::TypeCode::Float64A:
            {
                const auto arr = col.as<pvxs::shared_array<const double>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    ev->data_value->set_doublevalue(arr[i]);
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        case pvxs::TypeCode::StringA:
            {
                const auto arr = col.as<pvxs::shared_array<const std::string>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    ev->data_value->set_stringvalue(arr[i]);
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        case pvxs::TypeCode::StructA:
        case pvxs::TypeCode::UnionA:
        case pvxs::TypeCode::AnyA:
            {
                const auto arr = col.as<pvxs::shared_array<const pvxs::Value>>();
                const auto n = std::min(rowCount, static_cast<size_t>(arr.size()));
                auto& dest = outBatch->values[colName];
                dest.reserve(dest.size() + n);
                for (size_t i = 0; i < n; ++i)
                {
                    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(secondsArr->at(i), nanosArr->at(i));
                    EpicsMLDPConversion::convertPVToProtoValue(arr[i], ev->data_value.get());
                    dest.emplace_back(std::move(ev));
                }
                *outEmitted += n;
            }
            break;
        default:
            spdlog::trace("NTTable row-ts PV {} column '{}' has unsupported type code {}", tablePvName, colName, static_cast<int>(colCode));
            break;
        }
    }

    return *outEmitted > 0;
}
