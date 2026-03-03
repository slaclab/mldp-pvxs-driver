#pragma once

#include <common.pb.h>
#include <grpcpp/grpcpp.h>
#include <query.grpc.pb.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mldp_pvxs_driver::testutil {

inline void appendDataValuesToRows(const dp::service::common::DataValues& src,
                                   std::vector<dp::service::common::DataValue>* rows)
{
    if (!rows)
    {
        return;
    }

    auto pushRow = [&](const dp::service::common::DataValue& v)
    {
        rows->push_back(v);
    };

    using DV = dp::service::common::DataValues;
    switch (src.values_case())
    {
    case DV::kDataColumn:
        for (const auto& v : src.datacolumn().datavalues())
        {
            pushRow(v);
        }
        break;
    case DV::kSerializedDataColumn:
    {
        dp::service::common::DataColumn parsed;
        if (parsed.ParseFromString(src.serializeddatacolumn().payload()))
        {
            for (const auto& v : parsed.datavalues())
            {
                pushRow(v);
            }
        }
        break;
    }
    case DV::kDoubleColumn:
        for (double v : src.doublecolumn().values())
        {
            auto& row = rows->emplace_back();
            row.set_doublevalue(v);
        }
        break;
    case DV::kFloatColumn:
        for (float v : src.floatcolumn().values())
        {
            auto& row = rows->emplace_back();
            row.set_floatvalue(v);
        }
        break;
    case DV::kInt32Column:
        for (int32_t v : src.int32column().values())
        {
            auto& row = rows->emplace_back();
            row.set_intvalue(v);
        }
        break;
    case DV::kInt64Column:
        for (int64_t v : src.int64column().values())
        {
            auto& row = rows->emplace_back();
            row.set_longvalue(v);
        }
        break;
    case DV::kBoolColumn:
        for (bool v : src.boolcolumn().values())
        {
            auto& row = rows->emplace_back();
            row.set_booleanvalue(v);
        }
        break;
    case DV::kStringColumn:
        for (const auto& v : src.stringcolumn().values())
        {
            auto& row = rows->emplace_back();
            row.set_stringvalue(v);
        }
        break;
    default:
        break;
    }
}

inline std::vector<dp::service::common::DataValue> flattenDataValues(
    const std::vector<dp::service::common::DataValues>& buckets)
{
    std::vector<dp::service::common::DataValue> rows;
    for (const auto& bucket : buckets)
    {
        appendDataValuesToRows(bucket, &rows);
    }
    return rows;
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
        const auto status = stub->queryData(&context, request, &response);
        if (status.ok() && response.has_querydata() && !response.has_exceptionalresult())
        {
            std::unordered_map<std::string, std::vector<dp::service::common::DataValues>> collected;
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
