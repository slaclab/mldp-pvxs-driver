#pragma once

#include <common.pb.h>
#include <grpcpp/grpcpp.h>
#include <query.grpc.pb.h>

#include <chrono>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::testutil {

/// Typed result for one DataValues bucket (one column from one DataBucket).
struct ColumnResult
{
    std::string name;               ///< Column name from the proto message.
    std::string provenance_source;  ///< metadata.provenance.source, empty if unset.

    using Values = std::variant<
        std::vector<double>,
        std::vector<float>,
        std::vector<int32_t>,
        std::vector<int64_t>,
        std::vector<bool>,
        std::vector<std::string>,
        std::vector<std::vector<double>>,
        std::vector<std::vector<float>>,
        std::vector<std::vector<int32_t>>,
        std::vector<std::vector<int64_t>>,
        std::vector<std::vector<bool>>
    >;
    Values values;
};

/// Extract a ColumnResult from a DataValues oneof.
/// Handles: kDoubleColumn, kFloatColumn, kInt32Column, kInt64Column, kBoolColumn,
/// kStringColumn, kDoubleArrayColumn, kFloatArrayColumn, kInt32ArrayColumn,
/// kInt64ArrayColumn, kBoolArrayColumn.
inline ColumnResult extractColumn(const dp::service::common::DataValues& src)
{
    using DV = dp::service::common::DataValues;

    ColumnResult result;

    switch (src.values_case())
    {
    case DV::kDoubleColumn:
        {
            const auto& col = src.doublecolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            result.values = std::vector<double>(col.values().begin(), col.values().end());
            break;
        }
    case DV::kFloatColumn:
        {
            const auto& col = src.floatcolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            result.values = std::vector<float>(col.values().begin(), col.values().end());
            break;
        }
    case DV::kInt32Column:
        {
            const auto& col = src.int32column();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            result.values = std::vector<int32_t>(col.values().begin(), col.values().end());
            break;
        }
    case DV::kInt64Column:
        {
            const auto& col = src.int64column();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            result.values = std::vector<int64_t>(col.values().begin(), col.values().end());
            break;
        }
    case DV::kBoolColumn:
        {
            const auto& col = src.boolcolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            result.values = std::vector<bool>(col.values().begin(), col.values().end());
            break;
        }
    case DV::kStringColumn:
        {
            const auto& col = src.stringcolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            result.values = std::vector<std::string>(col.values().begin(), col.values().end());
            break;
        }
    case DV::kDoubleArrayColumn:
        {
            // Flat repeated values — wrap as a single inner vector per bucket.
            const auto& col = src.doublearraycolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            std::vector<double> inner(col.values().begin(), col.values().end());
            result.values = std::vector<std::vector<double>>{std::move(inner)};
            break;
        }
    case DV::kFloatArrayColumn:
        {
            const auto& col = src.floatarraycolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            std::vector<float> inner(col.values().begin(), col.values().end());
            result.values = std::vector<std::vector<float>>{std::move(inner)};
            break;
        }
    case DV::kInt32ArrayColumn:
        {
            const auto& col = src.int32arraycolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            std::vector<int32_t> inner(col.values().begin(), col.values().end());
            result.values = std::vector<std::vector<int32_t>>{std::move(inner)};
            break;
        }
    case DV::kInt64ArrayColumn:
        {
            const auto& col = src.int64arraycolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            std::vector<int64_t> inner(col.values().begin(), col.values().end());
            result.values = std::vector<std::vector<int64_t>>{std::move(inner)};
            break;
        }
    case DV::kBoolArrayColumn:
        {
            const auto& col = src.boolarraycolumn();
            result.name = col.name();
            result.provenance_source = col.metadata().provenance().source();
            std::vector<bool> inner(col.values().begin(), col.values().end());
            result.values = std::vector<std::vector<bool>>{std::move(inner)};
            break;
        }
    default:
        result.values = std::vector<double>{};
        break;
    }

    return result;
}

/// Flatten all DataValues buckets into a vector of ColumnResult (one per bucket).
inline std::vector<ColumnResult> flattenColumns(
    const std::vector<dp::service::common::DataValues>& buckets)
{
    std::vector<ColumnResult> cols;
    cols.reserve(buckets.size());
    for (const auto& bucket : buckets)
    {
        cols.push_back(extractColumn(bucket));
    }
    return cols;
}

inline std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>> queryAndCollectColumns(
    const std::vector<std::string>& pvNames,
    std::chrono::milliseconds       timeout,
    std::chrono::seconds            lookback_window = std::chrono::seconds(30))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto channel = grpc::CreateChannel("dp-query:50052", grpc::InsecureChannelCredentials());
    auto       stub = dp::service::query::DpQueryService::NewStub(channel);

    if (!stub)
    {
        return std::nullopt;
    }

    std::unordered_set<std::string> nameSet(pvNames.begin(), pvNames.end());

    // Accumulate across multiple query rounds: each round may return a subset of
    // the requested PVs.  We only return once every requested name has been seen.
    std::unordered_map<std::string, std::vector<dp::service::common::DataValues>> collected;

    while (std::chrono::steady_clock::now() < deadline)
    {
        dp::service::query::QueryDataRequest request;
        auto*                                spec = request.mutable_queryspec();
        for (const auto& pvName : pvNames)
        {
            spec->add_pvnames(pvName);
        }

        const auto now = std::chrono::system_clock::now();
        const auto begin = now - lookback_window;
        const auto end = now + std::chrono::seconds(1);

        auto* beginTs = spec->mutable_begintime();
        beginTs->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(begin.time_since_epoch()).count());

        auto* endTs = spec->mutable_endtime();
        endTs->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(end.time_since_epoch()).count());

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        dp::service::query::QueryDataResponse response;
        const auto                            status = stub->queryData(&context, request, &response);
        if (status.ok() && response.has_querydata() && !response.has_exceptionalresult())
        {
            for (const auto& bucket : response.querydata().databuckets())
            {
                if (!bucket.has_datavalues())
                {
                    continue;
                }

                std::string source_name = bucket.pvname();
                if (source_name.empty())
                {
                    const auto& values = bucket.datavalues();
                    if (values.values_case() == dp::service::common::DataValues::kDataColumn)
                    {
                        source_name = values.datacolumn().name();
                    }
                    else if (values.values_case() == dp::service::common::DataValues::kSerializedDataColumn)
                    {
                        dp::service::common::DataColumn parsed;
                        if (parsed.ParseFromString(values.serializeddatacolumn().payload()))
                        {
                            source_name = parsed.name();
                        }
                    }
                    // Some backends omit pvName and column names for struct/table
                    // payloads. When querying a single PV, bind unnamed buckets to it.
                    if (source_name.empty() && nameSet.size() == 1)
                    {
                        source_name = *nameSet.begin();
                    }
                }

                if (source_name.empty() || !nameSet.contains(source_name))
                {
                    continue;
                }

                collected[source_name].push_back(bucket.datavalues());

                if (collected.size() == nameSet.size())
                {
                    return collected;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return std::nullopt;
}

} // namespace mldp_pvxs_driver::testutil
