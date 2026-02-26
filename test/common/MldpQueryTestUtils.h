#pragma once

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

inline std::optional<std::unordered_map<std::string, DataColumn>> queryAndCollectColumns(
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
            std::unordered_map<std::string, DataColumn> collected;
            for (const auto& bucket : response.querydata().databuckets())
            {
                if (!bucket.has_datacolumn())
                {
                    continue;
                }

                const auto& column = bucket.datacolumn();
                if (!nameSet.contains(column.name()))
                {
                    continue;
                }

                collected.emplace(column.name(), column);
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
