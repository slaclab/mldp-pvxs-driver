//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file   EpicsDSMetadataReader.cpp
 * @brief  Implementation of EpicsDSMetadataReader.
 * @author SLAC MLDP Team
 * @date   2025-01-01
 * @copyright Copyright (c) 2025 SLAC National Accelerator Laboratory
 */

#include <reader/impl/epics_ds/EpicsDSMetadataReader.h>

#include <thread>
#include <util/log/Logger.h>

#include <pvxs/client.h>
#include <pvxs/data.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace pvxs;

using namespace mldp_pvxs_driver::reader::impl::epics_ds;
using namespace mldp_pvxs_driver::util::bus;

namespace {

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

// ---------------------------------------------------------------------------
// RpcResultQueue
// ---------------------------------------------------------------------------

bool EpicsDSMetadataReader::RpcResultQueue::push(pvxs::Value item, std::stop_token st)
{
    std::unique_lock lk(mu_);
    not_full_.wait(lk, st, [this] {
        return closed_ || queue_.size() < max_depth_;
    });
    if (closed_) return false;
    queue_.push(std::move(item));
    not_empty_.notify_one();
    return true;
}

std::optional<pvxs::Value> EpicsDSMetadataReader::RpcResultQueue::pop(std::stop_token st)
{
    std::unique_lock lk(mu_);
    not_empty_.wait(lk, st, [this] {
        return closed_ || !queue_.empty();
    });
    if (queue_.empty()) return std::nullopt;
    auto val = std::move(queue_.front());
    queue_.pop();
    not_full_.notify_one();
    return val;
}

void EpicsDSMetadataReader::RpcResultQueue::close()
{
    {
        std::lock_guard lk(mu_);
        closed_ = true;
    }
    not_full_.notify_all();
    not_empty_.notify_all();
}

// ---------------------------------------------------------------------------
// EpicsDSMetadataReader
// ---------------------------------------------------------------------------

EpicsDSMetadataReader::EpicsDSMetadataReader(
    std::shared_ptr<util::bus::IDataBus> bus,
    std::shared_ptr<metrics::Metrics>    metrics,
    const config::Config&                cfg)
    : reader::Reader(std::move(bus), std::move(metrics))
    , config_(cfg)
    , logger_(util::log::newLogger("reader:epics-ds-metadata:" + config_.name()))
    , pva_context_(pvxs::client::Config::fromEnv().build())
{
    const auto n = config_.workerThreadCount();
    if (n == 1) {
        // Single-thread: dispatch inline — no queue, no extra threads.
        dispatch_fn_ = [this](pvxs::Value v, std::stop_token) {
            processResult(std::move(v));
            return true;
        };
    } else {
        // Multi-thread: dispatch pushes to bounded queue; N-1 consumers drain it.
        result_queue_.emplace(config_.maxQueueDepth());
        dispatch_fn_ = [this](pvxs::Value v, std::stop_token st) {
            return result_queue_->push(std::move(v), st);
        };
        consumer_threads_.reserve(n - 1);
        for (std::size_t i = 0; i < n - 1; ++i)
            consumer_threads_.emplace_back([this](std::stop_token st) { runConsumer(st); });
    }
    worker_thread_ = std::jthread([this](std::stop_token st) { runWorker(st); });
}

EpicsDSMetadataReader::~EpicsDSMetadataReader()
{
    util::log::tracef(*logger_, "~EpicsDSMetadataReader '{}' [tid={}]: requesting stop on worker thread", config_.name(), std::this_thread::get_id());
    worker_thread_.request_stop();
    worker_thread_.join();
    util::log::tracef(*logger_, "~EpicsDSMetadataReader '{}' [tid={}]: worker thread joined", config_.name(), std::this_thread::get_id());
    if (result_queue_)
    {
        util::log::tracef(*logger_, "~EpicsDSMetadataReader '{}' [tid={}]: closing result queue", config_.name(), std::this_thread::get_id());
        result_queue_->close();
    }
    util::log::tracef(*logger_, "~EpicsDSMetadataReader '{}' [tid={}]: done (consumer jthreads joining)", config_.name(), std::this_thread::get_id());
    // consumer_threads_ jthread destructors join automatically
}

pvxs::Value EpicsDSMetadataReader::buildNTURI() const
{
    TypeDef queryDef(TypeCode::Struct, "epics:nt/NTURI:1.0",
                     {
                         Member(TypeCode::String, "scheme"),
                         Member(TypeCode::String, "path"),
                         Member(TypeCode::Struct, "query",
                                {
                                    Member(TypeCode::String, "name"),
                                    Member(TypeCode::String, "show"),
                                }),
                     });

    Value arg = queryDef.create();
    arg["scheme"]     = "pva";
    arg["path"]       = config_.service();
    arg["query.name"] = config_.query();
    if (!config_.showColumns().empty())
        arg["query.show"] = config_.showColumns();

    return arg;
}

void EpicsDSMetadataReader::processResult(pvxs::Value result)
{
    auto payload = parseNTTable(result);
    payload.root_source_name = config_.name();
    bus_->push(IDataBus::EventBatch{
        .reader_name = config_.name(),
        .payload     = std::move(payload),
    });
}

pvxs::Value EpicsDSMetadataReader::buildNTURIForPV(const std::string& pvName,
                                                   const std::string& showCol) const
{
    TypeDef queryDef(TypeCode::Struct, "epics:nt/NTURI:1.0",
                     {
                         Member(TypeCode::String, "scheme"),
                         Member(TypeCode::String, "path"),
                         Member(TypeCode::Struct, "query",
                                {
                                    Member(TypeCode::String, "name"),
                                    Member(TypeCode::String, "show"),
                                }),
                     });

    Value arg = queryDef.create();
    arg["scheme"]     = "pva";
    arg["path"]       = config_.service();
    arg["query.name"] = pvName;
    arg["query.show"] = showCol;
    return arg;
}

std::unordered_map<std::string, std::string>
EpicsDSMetadataReader::queryPVAttributes(const EpicsDSMetadataReaderConfig::PVEntry& pv)
{
    std::unordered_map<std::string, std::string> attributes;
    for (const auto& col : config_.pvShowColumns())
        attributes[col] = "";

    for (const auto& showCol : config_.pvShowColumns())
    {
        try
        {
            const Value result = pva_context_.rpc(config_.service(), buildNTURIForPV(pv.name, showCol))
                                     .exec()
                                     ->wait(config_.timeoutSec());

            const auto labelsVal   = result["labels"];
            const auto valueStruct = result["value"];
            if (!labelsVal.valid() || !valueStruct.valid())
                continue;

            const auto labels = labelsVal.as<shared_array<const std::string>>();
            if (labels.empty())
                continue;

            const auto column = valueStruct[std::string(labels[0])];
            if (!column.valid())
                continue;

            const auto values = column.as<shared_array<const std::string>>();
            if (values.empty())
                continue;

            attributes[showCol] = std::string(values[0]);
        }
        catch (const std::exception& e)
        {
            util::log::errorf(*logger_,
                              "EpicsDSMetadataReader '{}' PV-list RPC failed for '{}' show='{}': {}",
                              config_.name(), pv.name, showCol, e.what());
        }
    }

    if (logger_->shouldLog(util::log::Level::Debug))
    {
        std::ostringstream summary;
        summary << "EpicsDSMetadataReader '" << config_.name()
                << "' PV '" << pv.name << "' attributes:";
        for (const auto& col : config_.pvShowColumns())
        {
            const auto it = attributes.find(col);
            summary << ' ' << col << '='
                    << (it != attributes.end() && !it->second.empty() ? it->second : "<missing>");
        }
        util::log::debugf(*logger_, "{}", summary.str());
    }

    return attributes;
}

void EpicsDSMetadataReader::runPVListSweep(std::stop_token st) noexcept
{
    for (const auto& pv : config_.pvs())
    {
        if (st.stop_requested())
            return;

        auto attributes = queryPVAttributes(pv);
        for (const auto& [key, value] : pv.metadata)
            attributes[key] = value;

        util::bus::SourceMetadataEntry entry;
        entry.attributes = std::move(attributes);

        SourceMetadataPayload payload;
        payload.root_source_name = pv.name;
        payload.sources.emplace(pv.name, std::move(entry));

        bus_->push(IDataBus::EventBatch{
            .reader_name = config_.name(),
            .payload     = std::move(payload),
        });
    }
}

void EpicsDSMetadataReader::runWorker(std::stop_token st)
{
    while (!st.stop_requested()) {
        try {
            auto op = pva_context_.rpc(config_.service(), buildNTURI()).exec();
            std::stop_callback cancel_on_stop{st, [&op] { op->interrupt(); }};
            Value result = op->wait(config_.timeoutSec());

            {
                std::ostringstream oss;
                oss << result;
                util::log::debugf(*logger_, "DS RPC raw response:\n{}", oss.str());
            }

            // dispatch_fn_ returns false if the worker should stop (e.g. if the queue is closed)
            // the implementation of dispatch_fn_ depends on the number of threads configured:
            // in single-thread mode it processes inline and always returns true;
            // in multi-thread mode it pushes to the queue and returns false if the queue is closed.
            if (!dispatch_fn_(std::move(result), st))
                break;

            if (!config_.pvs().empty())
                runPVListSweep(st);
        }
        catch (const pvxs::client::Interrupted&) {
            break;
        }
        catch (const std::exception& e) {
            util::log::errorf(*logger_,
                              "EpicsDSMetadataReader '{}' RPC failed: {}",
                              config_.name(), e.what());
        }

        if (config_.rescanIntervalSec() <= 0.0)
        {
            signalCompleted();
            break;
        }

        std::unique_lock lk(sleep_mutex_);
        sleep_cv_.wait_for(lk,
                           std::chrono::duration<double>(config_.rescanIntervalSec()),
                           [&st] { return st.stop_requested(); });
    }
}

bool EpicsDSMetadataReader::queueResult(pvxs::Value result, std::stop_token st)
{
    return result_queue_->push(std::move(result), st);
}

void EpicsDSMetadataReader::runConsumer(std::stop_token st)
{
    while (auto item = result_queue_->pop(st)) {
        try {
            processResult(std::move(*item));
        }
        catch (const std::exception& e) {
            util::log::errorf(*logger_,
                              "EpicsDSMetadataReader '{}' consumer failed: {}",
                              config_.name(), e.what());
        }
    }
}

SourceMetadataPayload
EpicsDSMetadataReader::parseNTTable(const pvxs::Value& result) const
{
    SourceMetadataPayload payload;

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

    size_t nrows = 0;
    {
        const auto firstCol = valueStruct[std::string(labels[0])];
        if (firstCol.valid())
        {
            const auto arr = firstCol.as<shared_array<const std::string>>();
            nrows = arr.size();
        }
    }
    if (nrows == 0)
        return payload;

    size_t srcIdx  = 0;
    std::optional<std::size_t> tagsIdx;
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
        bool first = true;
        for (const auto& label : labels)
        {
            if (!first) colList += ", ";
            colList += std::string(label);
            first = false;
        }
        util::log::warnf(*logger_,
                         "parseNTTable: source-name-column '{}' not found, using column 0. "
                         "Received columns ({}): [{}]",
                         config_.sourceNameColumn(),
                         ncols,
                         colList);
    }

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
            colData[i].resize(nrows);
        }
        while (colData[i].size() < nrows)
            colData[i].emplace_back();
    }

    for (size_t r = 0; r < nrows; ++r)
    {
        const std::string key = (srcIdx < ncols) ? colData[srcIdx][r] : "";
        if (key.empty())
            continue;

        util::bus::SourceMetadataEntry entry;

        if (tagsIdx.has_value() && *tagsIdx < ncols)
        {
            auto tagVec = splitTags(colData[*tagsIdx][r]);
            if (!tagVec.empty())
                entry.tags = std::move(tagVec);
        }

        for (size_t i = 0; i < ncols; ++i)
        {
            if (i == srcIdx || (tagsIdx.has_value() && i == *tagsIdx))
                continue;
            entry.attributes[std::string(labels[i])] = colData[i][r];
        }

        payload.sources[key] = std::move(entry);
    }

    return payload;
}
