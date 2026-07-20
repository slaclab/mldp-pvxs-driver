//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "util/log/Logger.h"
#include <reader/impl/epics_archiver/ArchiverPbHttpConversion.h>
#include <reader/impl/epics_archiver/EpicsArchiverReader.h>

#include <EPICSEvent.pb.h>
#include <metrics/Metrics.h>
#include <util/bus/IDataBus.h>
#include <util/http/CurlHttpClient.h>
#include <util/http/HttpClient.h>
#include <util/http/HttpUrlUtils.h>
#include <util/log/ILog.h>
#include <util/time/DateTimeUtils.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::http;
using namespace mldp_pvxs_driver::util::time;
using namespace mldp_pvxs_driver::reader::impl::epics_archiver;

namespace {

constexpr const char* kArchiverPbRawPath = "/retrieval/data/getData.raw";

bool hasTimestamps(const DataBatch& batch)
{
    return !batch.timestamps.empty();
}

void appendToBatch(DataBatch& dst, const DataBatch& src)
{
    dst.timestamps.insert(dst.timestamps.end(), src.timestamps.begin(), src.timestamps.end());

    if (dst.columns.empty())
    {
        dst.columns = src.columns;
    }
    else
    {
        const std::size_t n = std::min(dst.columns.size(), src.columns.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            std::visit(
                [&](auto& dst_vals)
                {
                    using T = std::decay_t<decltype(dst_vals)>;
                    if (const auto* src_vals = std::get_if<T>(&src.columns[i].values))
                    {
                        dst_vals.insert(dst_vals.end(), src_vals->begin(), src_vals->end());
                    }
                },
                dst.columns[i].values);
        }
    }

    if (dst.enum_columns.empty())
    {
        dst.enum_columns = src.enum_columns;
    }
    else
    {
        const std::size_t n = std::min(dst.enum_columns.size(), src.enum_columns.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            auto& d = dst.enum_columns[i].values;
            const auto& s = src.enum_columns[i].values;
            d.insert(d.end(), s.begin(), s.end());
        }
    }

    if (dst.array_dims.empty())
    {
        dst.array_dims = src.array_dims;
    }
}

std::string buildArchiverUrl(const EpicsArchiverReaderConfig&  cfg,
                             const std::string&                pv,
                             const std::string&                from,
                             const std::optional<std::string>& to)
{
    std::string base = cfg.hostname();
    if (!HttpUrlUtils::hasScheme(base))
    {
        base = "http://" + base;
    }
    base = HttpUrlUtils::trimTrailingSlash(std::move(base));

    std::string url = base + kArchiverPbRawPath + "?pv=" + HttpUrlUtils::percentEncode(pv) + "&from=" + HttpUrlUtils::percentEncode(from);
    if (to.has_value())
    {
        url += "&to=" + HttpUrlUtils::percentEncode(*to);
    }
    return url;
}

std::string unescapePbHttpLine(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c != 0x1B)
        {
            out.push_back(static_cast<char>(c));
            continue;
        }
        if (i + 1 >= in.size())
        {
            throw std::runtime_error("truncated PB/HTTP escape sequence");
        }
        const unsigned char code = static_cast<unsigned char>(in[++i]);
        switch (code)
        {
        case 0x01: out.push_back(static_cast<char>(0x1B)); break;
        case 0x02: out.push_back('\n'); break;
        case 0x03: out.push_back('\r'); break;
        default: throw std::runtime_error("invalid PB/HTTP escape code");
        }
    }
    return out;
}

bool sampleTimeLessThan(uint64_t lhs_epoch, uint32_t lhs_nano, uint64_t rhs_epoch, uint32_t rhs_nano)
{
    return (lhs_epoch < rhs_epoch) || (lhs_epoch == rhs_epoch && lhs_nano < rhs_nano);
}

} // namespace

// --- PVWorkQueue ---

void EpicsArchiverReader::PVWorkQueue::populate(const std::vector<std::string>& pv_names)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::queue<std::string> empty;
    pvs_.swap(empty);
    for (const auto& pv : pv_names)
    {
        pvs_.push(pv);
    }
}

std::optional<std::string> EpicsArchiverReader::PVWorkQueue::pop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pvs_.empty())
    {
        return std::nullopt;
    }
    std::string pv = std::move(pvs_.front());
    pvs_.pop();
    return pv;
}

std::size_t EpicsArchiverReader::PVWorkQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pvs_.size();
}

// --- EpicsArchiverReader ---

EpicsArchiverReader::EpicsArchiverReader(
    std::shared_ptr<IDataBus>                 bus,
    std::shared_ptr<Metrics>                  metrics,
    const ::mldp_pvxs_driver::config::Config& cfg)
    : ::mldp_pvxs_driver::reader::Reader(std::move(bus), std::move(metrics))
    , logger_(::mldp_pvxs_driver::util::log::newLogger("reader:epics-archiver:" + cfg.get("name")))
    , name_(cfg.get("name"))
    , config_(cfg)
{
    if (!config_.valid())
    {
        throw EpicsArchiverReaderConfig::Error("Failed to parse Archiver reader configuration");
    }

    try
    {
        initializeHttpClients();
    }
    catch (const std::exception& e)
    {
        throw EpicsArchiverReaderConfig::Error(std::string("Failed to initialize HTTP clients: ") + e.what());
    }

    startWorkers();
}

EpicsArchiverReader::~EpicsArchiverReader()
{
    stopWorkers();
    destroyHttpClients();
}

std::string EpicsArchiverReader::name() const
{
    return name_;
}

void EpicsArchiverReader::initializeHttpClients()
{
    const auto num_threads = static_cast<std::size_t>(config_.fetchThreads());
    worker_contexts_.resize(num_threads);

    for (std::size_t i = 0; i < num_threads; ++i)
    {
        auto client = std::make_unique<CurlHttpClient>(logger_);

        HttpClientOptions options;
        options.connect_timeout_sec = config_.connectTimeoutSec();
        options.total_timeout_sec = 0;
        options.low_speed_limit_bytes_per_sec = 0;
        options.low_speed_time_sec = 0;
        options.tls.verify_peer = config_.tlsVerifyPeer();
        options.tls.verify_host = config_.tlsVerifyHost();
        options.user_agent = "MLDP Archiver Reader 1.0 (mldp-pvxs-driver)";

        client->setDefaultOptions(options);
        client->setDefaultHeaders({"Accept: application/octet-stream"});

        worker_contexts_[i].http_client = std::move(client);
    }

    debugf(*logger_,
           "Initialized {} HTTP clients for Archiver reader (connect_timeout={}s tls(peer={},host={}))",
           num_threads,
           config_.connectTimeoutSec(),
           config_.tlsVerifyPeer(),
           config_.tlsVerifyHost());
}

void EpicsArchiverReader::destroyHttpClients()
{
    for (auto& ctx : worker_contexts_)
    {
        ctx.http_client.reset();
    }
    worker_contexts_.clear();
}

void EpicsArchiverReader::startWorkers()
{
    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_error_ = nullptr;
        worker_done_ = false;
    }
    workers_completed_.store(0);

    if (config_.fetchMode() == EpicsArchiverReaderConfig::FetchMode::HistoricalOnce)
    {
        pv_queue_.populate(config_.pvNames());
    }

    const auto num_threads = static_cast<std::size_t>(config_.fetchThreads());
    worker_threads_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
    {
        worker_threads_.emplace_back([this, i]() { runWorker(i); });
    }
}

void EpicsArchiverReader::stopWorkers()
{
    running_.store(false);
    worker_cv_.notify_all();
    cycle_cv_.notify_all();

    for (auto& ctx : worker_contexts_)
    {
        if (ctx.http_client)
        {
            ctx.http_client->cancelOngoingRequests();
        }
    }

    for (auto& t : worker_threads_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    worker_threads_.clear();
}

void EpicsArchiverReader::runWorker(std::size_t index)
{
    auto& ctx = worker_contexts_[index];

    try
    {
        switch (config_.fetchMode())
        {
        case EpicsArchiverReaderConfig::FetchMode::HistoricalOnce:
            {
                while (running_.load())
                {
                    auto pv = pv_queue_.pop();
                    if (!pv)
                    {
                        break;
                    }
                    infof(*logger_, "Worker {} starting PV '{}' (queue remaining: {})",
                           index, *pv, pv_queue_.size());
                    fetchSinglePV(ctx, *pv, config_.startDate(), config_.endDate());
                }

                if (running_.load() && config_.batchFlushIntervalMs() > 0)
                {
                    flushAllPendingBatches(ctx);
                }
                break;
            }
        case EpicsArchiverReaderConfig::FetchMode::PeriodicTail:
            {
                infof(*logger_,
                      "Archiver reader '{}' worker {} running in periodic_tail mode (poll_interval={}s lookback={}s)",
                      name_, index,
                      config_.pollIntervalSec(),
                      config_.lookbackSec());

                std::optional<std::chrono::system_clock::time_point> previous_iteration_end;
                const bool contiguous_windows = (config_.lookbackSec() == config_.pollIntervalSec());
                std::size_t last_cycle_seen = 0;

                while (running_.load())
                {
                    if (index == 0)
                    {
                        const auto iteration_end = DateTimeUtils::truncateToMilliseconds(std::chrono::system_clock::now());
                        auto       iteration_start = iteration_end - std::chrono::seconds(config_.lookbackSec());
                        if (contiguous_windows && previous_iteration_end.has_value())
                        {
                            iteration_start = *previous_iteration_end;
                        }

                        {
                            std::lock_guard<std::mutex> lock(cycle_mutex_);
                            cycle_from_ = DateTimeUtils::formatIso8601UtcMillis(iteration_start);
                            cycle_to_ = DateTimeUtils::formatIso8601UtcMillis(iteration_end);
                        }
                        debugf(*logger_, "Periodic tail fetch for '{}' window [{} -> {}]", name_, cycle_from_, *cycle_to_);

                        pv_queue_.populate(config_.pvNames());
                        previous_iteration_end = iteration_end;

                        cycle_ready_.fetch_add(1);
                        cycle_cv_.notify_all();
                    }

                    // All workers wait for the current cycle to be signaled.
                    {
                        std::unique_lock<std::mutex> lock(cycle_mutex_);
                        cycle_cv_.wait(lock, [&]()
                                       {
                                           return cycle_ready_.load() > last_cycle_seen || !running_.load();
                                       });
                        if (!running_.load())
                        {
                            break;
                        }
                        last_cycle_seen = cycle_ready_.load();
                    }

                    std::string from;
                    std::optional<std::string> to;
                    {
                        std::lock_guard<std::mutex> lock(cycle_mutex_);
                        from = cycle_from_;
                        to = cycle_to_;
                    }

                    while (running_.load())
                    {
                        auto pv = pv_queue_.pop();
                        if (!pv)
                        {
                            break;
                        }
                        infof(*logger_, "Worker {} starting PV '{}' (queue remaining: {})",
                               index, *pv, pv_queue_.size());
                        fetchSinglePV(ctx, *pv, from, to);
                    }

                    if (running_.load() && config_.batchFlushIntervalMs() > 0)
                    {
                        flushAllPendingBatches(ctx);
                    }

                    if (index == 0)
                    {
                        // Coordinator sleeps for the poll interval then starts next cycle.
                        std::unique_lock<std::mutex> lock(worker_mutex_);
                        worker_cv_.wait_for(lock,
                                            std::chrono::seconds(config_.pollIntervalSec()),
                                            [this]()
                                            {
                                                return !running_.load();
                                            });
                    }
                    // Non-coordinator workers loop back and block on cycle_cv_ until
                    // the coordinator increments cycle_ready_ in the next iteration.
                }
                break;
            }
        default:
            throw std::runtime_error("Unsupported fetch mode in Archiver reader configuration");
        }
    }
    catch (const std::exception& e)
    {
        if (!running_.load())
        {
            debugf(*logger_, "Archiver reader worker '{}' [{}] stopped during shutdown: {}", name_, index, e.what());
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(worker_mutex_);
                if (!worker_error_)
                {
                    worker_error_ = std::current_exception();
                }
            }
            errorf(*logger_, "Archiver reader worker '{}' [{}] failed: {}", name_, index, e.what());
        }
    }
    catch (...)
    {
        if (!running_.load())
        {
            debugf(*logger_, "Archiver reader worker '{}' [{}] stopped during shutdown", name_, index);
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(worker_mutex_);
                if (!worker_error_)
                {
                    worker_error_ = std::current_exception();
                }
            }
            errorf(*logger_, "Archiver reader worker '{}' [{}] failed with unknown exception", name_, index);
        }
    }

    const auto completed = workers_completed_.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (completed == static_cast<std::size_t>(config_.fetchThreads()))
        {
            worker_done_ = true;
            if (config_.fetchMode() == EpicsArchiverReaderConfig::FetchMode::HistoricalOnce
                && running_.load() && !worker_error_)
            {
                signalCompleted();
            }
        }
    }
}

bool EpicsArchiverReader::pushBatch(IDataBus::EventBatch batch)
{
    if (!running_.load())
    {
        return false;
    }
    return bus_->push(std::move(batch));
}

void EpicsArchiverReader::submitPendingBatch(WorkerContext& ctx, const std::string& pv)
{
    auto it = ctx.pending_pv_batches.find(pv);
    if (it == ctx.pending_pv_batches.end() || it->second.accumulated.timestamps.empty())
    {
        return;
    }

    PendingPvBatch& pending = it->second;
    const prometheus::Labels source_tag{{"source", pv}};
    const auto flush_start = std::chrono::steady_clock::now();

    IDataBus::EventBatch batch;
    batch.metadata = pending.metadata;
    TimeSeriesPayload ts_payload;
    ts_payload.root_source_name = pending.root_source_name;

    if (hasTimestamps(pending.accumulated))
    {
        ts_payload.frames.push_back(std::move(pending.accumulated));
    }
    else
    {
        errorf(*logger_, "Dropping pending batch without timestamps for root source {}", pending.root_source_name);
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementReaderErrors(1.0, source_tag);
                    });
    }

    ctx.pending_pv_batches.erase(it);

    if (!ts_payload.frames.empty())
    {
        batch.payload = std::move(ts_payload);
        batch.reader_name = name();
        pushBatch(std::move(batch));

        const double flush_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - flush_start).count();
        metric_call(metrics_, [&](auto& m)
                    {
                        m.observeReaderProcessingTimeMs(flush_ms, source_tag);
                        m.incrementReaderEvents(1.0, source_tag);
                    });
    }
}

void EpicsArchiverReader::flushAllPendingBatches(WorkerContext& ctx)
{
    std::vector<std::string> pvs;
    pvs.reserve(ctx.pending_pv_batches.size());
    for (const auto& [pv, pending] : ctx.pending_pv_batches)
    {
        if (!pending.accumulated.timestamps.empty())
        {
            pvs.push_back(pv);
        }
    }
    for (const auto& pv : pvs)
    {
        submitPendingBatch(ctx, pv);
    }
}

void EpicsArchiverReader::flushExpiredPendingBatches(WorkerContext& ctx)
{
    if (config_.batchFlushIntervalMs() <= 0)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto threshold = std::chrono::milliseconds(config_.batchFlushIntervalMs());

    std::vector<std::string> to_flush;
    for (const auto& [pv, pending] : ctx.pending_pv_batches)
    {
        if (!pending.accumulated.timestamps.empty() && (now - pending.created_at) >= threshold)
        {
            to_flush.push_back(pv);
        }
    }
    for (const auto& pv : to_flush)
    {
        submitPendingBatch(ctx, pv);
    }
}

void EpicsArchiverReader::flushChunk(WorkerContext& ctx, PbChunkState& state)
{
    if (!state.have_header)
    {
        return;
    }

    const std::string& pv = state.header.pvname();
    if (!state.events.empty())
    {
        const prometheus::Labels source_tag{{"source", pv}};
        const auto               flush_start = std::chrono::steady_clock::now();

        IDataBus::EventBatch batch;
        auto merged = config_.staticMetadata();
        for (const auto& pv_cfg : config_.pvs())
        {
            if (pv_cfg.name == pv)
            {
                for (auto& [k, v] : pv_cfg.metadata)
                    merged[k] = v;
                break;
            }
        }
        batch.metadata = std::move(merged);
        const std::string root_name = pv.empty() ? name_ : pv;
        TimeSeriesPayload ts_payload;
        ts_payload.root_source_name = root_name;
        for (auto& frame : state.events)
        {
            if (!hasTimestamps(frame))
            {
                errorf(*logger_, "Dropping archiver batch without timestamps for root source {}", root_name);
                metric_call(metrics_, [&](auto& m)
                            {
                                m.incrementReaderErrors(1.0, source_tag);
                            });
                continue;
            }
            ts_payload.frames.push_back(std::move(frame));
        }
        if (!ts_payload.frames.empty())
        {
            batch.payload = std::move(ts_payload);
            batch.reader_name = name();
            pushBatch(std::move(batch));
        }

        const auto   flush_end = std::chrono::steady_clock::now();
        const double flush_ms = std::chrono::duration<double, std::milli>(flush_end - flush_start).count();

        metric_call(metrics_, [&](auto& m)
                    {
                        m.observeReaderProcessingTimeMs(flush_ms, source_tag);
                        m.incrementReaderEvents(1.0, source_tag);
                    });
    }

    state.events.clear();
}

void EpicsArchiverReader::finalizeChunk(WorkerContext& ctx, PbChunkState& state)
{
    if (!state.have_header)
    {
        return;
    }

    flushChunk(ctx, state);
    state = PbChunkState{};
}

void EpicsArchiverReader::parsePbHttpLineIntoState(WorkerContext& ctx, const std::string& line, PbChunkState& state)
{
    const std::string msg_bytes = unescapePbHttpLine(line);

    if (!state.have_header)
    {
        EPICS::PayloadInfo header;
        if (!header.ParseFromString(msg_bytes))
        {
            throw std::runtime_error("failed to parse PB/HTTP PayloadInfo");
        }
        state.header = std::move(header);
        state.have_header = true;
        return;
    }

    const auto parsed = ArchiverPbHttpConversion::parseSample(state.header, msg_bytes);

    const std::string        pv = state.header.pvname();
    const prometheus::Labels source_tag{{"source", pv}};
    metric_call(metrics_, [&](auto& m)
                {
                    m.incrementReaderEventsReceived(1.0, source_tag);
                });

    if (config_.fetchMode() == EpicsArchiverReaderConfig::FetchMode::PeriodicTail)
    {
        auto it = ctx.last_published_ns_per_pv.find(pv);
        if (it != ctx.last_published_ns_per_pv.end())
        {
            const auto [wm_epoch, wm_nano] = it->second;
            if (!sampleTimeLessThan(wm_epoch, wm_nano, parsed.epoch_seconds, parsed.nanoseconds))
            {
                debugf(*logger_,
                       "Skipping duplicate sample for PV '{}' at ({}, {}): not newer than watermark ({}, {})",
                       pv, parsed.epoch_seconds, parsed.nanoseconds, wm_epoch, wm_nano);
                return;
            }
        }
        ctx.last_published_ns_per_pv[pv] = {parsed.epoch_seconds, parsed.nanoseconds};
    }

    if (config_.pvSamplesPerBatch() > 0)
    {
        auto& pending = ctx.pending_pv_batches[pv];
        if (pending.accumulated.timestamps.empty())
        {
            auto merged = config_.staticMetadata();
            for (const auto& pv_cfg : config_.pvs())
            {
                if (pv_cfg.name == pv)
                {
                    for (const auto& [k, v] : pv_cfg.metadata)
                        merged[k] = v;
                    break;
                }
            }
            pending.metadata = std::move(merged);
            pending.root_source_name = pv.empty() ? name_ : pv;
            pending.created_at = std::chrono::steady_clock::now();
        }
        appendToBatch(pending.accumulated, parsed.batch);

        if (static_cast<long>(pending.accumulated.timestamps.size()) >= config_.pvSamplesPerBatch())
        {
            submitPendingBatch(ctx, pv);
        }
    }
    else
    {
        state.events.emplace_back(std::move(parsed.batch));
    }
}

void EpicsArchiverReader::fetchSinglePV(WorkerContext& ctx,
                                         const std::string& pv,
                                         const std::string& from,
                                         const std::optional<std::string>& to)
{
    if (!ctx.http_client)
    {
        throw std::runtime_error("HTTP client not initialized for worker");
    }
    if (!bus_)
    {
        throw std::runtime_error("Event bus is not available");
    }

    const std::string url = buildArchiverUrl(config_, pv, from, to);
    infof(*logger_, "Fetching archiver PB/HTTP stream for PV '{}' from {}", pv, url);

    PbChunkState     chunk_state;
    std::string      line_buf;
    std::size_t      total_bytes = 0;
    const auto       fetch_start = std::chrono::steady_clock::now();
    HttpResponseInfo response = ctx.http_client->streamGet(
        HttpRequest{.url = url},
        [&](const char* data, std::size_t size)
        {
            total_bytes += size;
            for (std::size_t i = 0; i < size; ++i)
            {
                const char ch = data[i];
                if (ch == '\n')
                {
                    if (line_buf.empty())
                    {
                        finalizeChunk(ctx, chunk_state);
                    }
                    else
                    {
                        try
                        {
                            parsePbHttpLineIntoState(ctx, line_buf, chunk_state);
                        }
                        catch (const std::exception& e)
                        {
                            const prometheus::Labels source_tag{{"source", pv}};
                            metric_call(metrics_, [&](auto& m)
                                        {
                                            m.incrementReaderErrors(1.0, source_tag);
                                        });
                            throw;
                        }
                        line_buf.clear();
                    }
                    continue;
                }
                line_buf.push_back(ch);
            }
        });

    if (!line_buf.empty())
    {
        try
        {
            parsePbHttpLineIntoState(ctx, line_buf, chunk_state);
        }
        catch (const std::exception& e)
        {
            const prometheus::Labels source_tag{{"source", pv}};
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementReaderErrors(1.0, source_tag);
                        });
            throw;
        }
        line_buf.clear();
    }
    finalizeChunk(ctx, chunk_state);

    if (total_bytes > 0)
    {
        const prometheus::Labels source_tag{{"source", pv}};
        const double fetch_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - fetch_start).count();
        metric_call(metrics_, [&](auto& m) {
            m.incrementReaderDataBytesTotal(static_cast<double>(total_bytes), source_tag);
        });
        if (fetch_ms > 0.0)
        {
            const double bps = (static_cast<double>(total_bytes) * 1000.0) / fetch_ms;
            metric_call(metrics_, [&](auto& m) {
                m.setReaderDataBytesPerSecond(bps, source_tag);
            });
        }
    }

    if (response.http_status != 200)
    {
        const prometheus::Labels source_tag{{"source", pv}};
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementReaderErrors(1.0, source_tag);
                    });
        throw std::runtime_error(
            "archiver HTTP GET returned status " + std::to_string(response.http_status) + " for PV " + pv);
    }
    infof(*logger_, "Completed fetch of archiver PB/HTTP stream for PV '{}'", pv);

    flushExpiredPendingBatches(ctx);
}
