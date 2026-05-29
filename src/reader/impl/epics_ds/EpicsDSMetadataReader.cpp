//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics_ds/EpicsDSMetadataReader.h>

#include <util/log/Logger.h>

#include <pvxs/client.h>
#include <pvxs/data.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pvxs;

namespace mldp_pvxs_driver::reader::impl::epics_ds {

namespace {

/// Split a comma-separated string into tokens, trimming whitespace and
/// skipping empty tokens.
std::vector<std::string> splitTags(const std::string& s)
{
    std::vector<std::string> result;
    std::istringstream       ss(s);
    std::string              token;
    while (std::getline(ss, token, ','))
    {
        const auto start = token.find_first_not_of(" \t\r\n");
        const auto end   = token.find_last_not_of(" \t\r\n");
        if (start != std::string::npos)
            result.push_back(token.substr(start, end - start + 1));
    }
    return result;
}

} // namespace

EpicsDSMetadataReader::EpicsDSMetadataReader(
    std::shared_ptr<util::bus::IDataBus> bus,
    std::shared_ptr<metrics::Metrics>    metrics,
    const config::Config&                cfg)
    : reader::Reader(std::move(bus), std::move(metrics))
    , config_(cfg)
    , logger_(util::log::newLogger("reader:epics-ds-metadata:" + config_.name()))
    , pva_context_(pvxs::client::Config::fromEnv().build())
{
    running_ = true;
    worker_thread_ = std::thread([this] { runWorker(); });
}

EpicsDSMetadataReader::~EpicsDSMetadataReader()
{
    {
        std::lock_guard<std::mutex> lk(worker_mutex_);
        running_ = false;
    }
    worker_cv_.notify_all();
    if (worker_thread_.joinable())
        worker_thread_.join();
}

void EpicsDSMetadataReader::runWorker()
{
    do
    {
        try
        {
            // Build NTURI argument struct
            Value arg = TypeDef(TypeCode::Struct, "epics:nt/NTURI:1.0",
                                {
                                    Member(TypeCode::String, "scheme"),
                                    Member(TypeCode::String, "path"),
                                    Member(TypeCode::Struct, "query",
                                           {
                                               Member(TypeCode::String, "name"),
                                                Member(TypeCode::String, "show")
                                           }),
                                })
                            .create();

            arg["scheme"]     = "pva";
            arg["path"]       = config_.service();
            arg["query.name"] = config_.query();
            arg["query.show"] = "ioc,dname";

            const double timeoutSec = config_.timeoutSec();

            Value result = pva_context_.rpc(config_.service(), arg)
                               .exec()
                               ->wait(timeoutSec);

            {
                std::ostringstream oss;
                oss << result;
                util::log::debugf(*logger_, "DS RPC raw response:\n{}", oss.str());
            }

            auto payload = parseNTTable(result);

            util::bus::IDataBus::EventBatch batch;
            batch.reader_name = config_.name();
            batch.root_source = config_.name();
            batch.payload     = std::move(payload);
            bus_->push(std::move(batch));
        }
        catch (const std::exception& e)
        {
            util::log::errorf(*logger_,
                              "EpicsDSMetadataReader '{}' RPC failed: {}",
                              config_.name(),
                              e.what());
        }

        if (config_.rescanIntervalSec() <= 0.0)
            break;

        // Interruptible sleep until the next rescan or destruction
        std::unique_lock<std::mutex> lk(worker_mutex_);
        worker_cv_.wait_for(lk,
                            std::chrono::duration<double>(config_.rescanIntervalSec()),
                            [this] { return !running_.load(); });

    } while (running_.load());
}

util::bus::SourceMetadataPayload
EpicsDSMetadataReader::parseNTTable(const pvxs::Value& result) const
{
    util::bus::SourceMetadataPayload payload;

    const auto labelsVal = result["labels"];
    if (!labelsVal.valid())
    {
        util::log::warnf(*logger_, "parseNTTable: no 'labels' field in response");
        return payload;
    }

    const auto   labels = labelsVal.as<shared_array<const std::string>>();
    const size_t ncols  = labels.size();
    if (ncols == 0)
        return payload;

    const auto valueStruct = result["value"];
    if (!valueStruct.valid())
        return payload;

    // Determine row count from the first column
    size_t nrows = 0;
    {
        const auto firstCol = valueStruct[std::string(labels[0])];
        if (firstCol.valid())
        {
            const auto arr = firstCol.as<shared_array<const std::string>>();
            nrows          = arr.size();
        }
    }
    if (nrows == 0)
        return payload;

    // Locate special column indices
    size_t srcIdx   = 0;
    size_t tagsIdx  = SIZE_MAX;
    bool   srcFound = false;

    for (size_t i = 0; i < ncols; ++i)
    {
        if (std::string(labels[i]) == config_.sourceNameColumn())
        {
            srcIdx   = i;
            srcFound = true;
        }
        if (!config_.tagsColumn().empty() &&
            std::string(labels[i]) == config_.tagsColumn())
        {
            tagsIdx = i;
        }
    }

    if (!srcFound)
    {
        std::string colList;
        for (size_t i = 0; i < ncols; ++i)
        {
            if (i) colList += ", ";
            colList += labels[i];
        }
        util::log::warnf(*logger_,
                         "parseNTTable: source-name-column '{}' not found, using column 0. "
                         "Received columns ({}): [{}]",
                         config_.sourceNameColumn(),
                         ncols,
                         colList);
    }

    // Extract all column arrays as string vectors
    std::vector<std::vector<std::string>> colData(ncols);
    for (size_t i = 0; i < ncols; ++i)
    {
        const auto col = valueStruct[std::string(labels[i])];
        if (!col.valid())
        {
            colData[i].resize(nrows);
            continue;
        }
        try
        {
            const auto arr = col.as<shared_array<const std::string>>();
            colData[i].reserve(arr.size());
            for (size_t r = 0; r < arr.size(); ++r)
                colData[i].push_back(std::string(arr[r]));
        }
        catch (const std::exception&)
        {
            // Non-string column: leave as empty strings
            colData[i].resize(nrows);
        }
        // Pad to nrows if the column is shorter
        while (colData[i].size() < nrows)
            colData[i].emplace_back();
    }

    // Build the payload map
    for (size_t r = 0; r < nrows; ++r)
    {
        const std::string key = (srcIdx < ncols) ? colData[srcIdx][r] : "";
        if (key.empty())
            continue;

        util::bus::SourceMetadataEntry entry;

        if (tagsIdx != SIZE_MAX && tagsIdx < ncols)
        {
            auto tagVec = splitTags(colData[tagsIdx][r]);
            if (!tagVec.empty())
                entry.tags = std::move(tagVec);
        }

        for (size_t i = 0; i < ncols; ++i)
        {
            if (i == srcIdx || i == tagsIdx)
                continue;
            entry.attributes[std::string(labels[i])] = colData[i][r];
        }

        payload[key] = std::move(entry);
    }

    return payload;
}

} // namespace mldp_pvxs_driver::reader::impl::epics_ds
