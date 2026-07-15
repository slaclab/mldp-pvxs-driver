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
    // Timestamps: always append.
    dst.timestamps.insert(dst.timestamps.end(), src.timestamps.begin(), src.timestamps.end());

    // Regular columns: first sample seeds the column list; subsequent samples append values.
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

    // Enum columns: same pattern.
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

    // Array dims are invariant across samples of the same PV type; take from first.
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

EpicsArchiverReader::EpicsArchiverReader(
    std::shared_ptr<IDataBus>                 bus,
    std::shared_ptr<Metrics>                  metrics,
    const ::mldp_pvxs_driver::config::Config& cfg)
    : ::mldp_pvxs_driver::reader::Reader(std::move(bus), std::move(metrics))
    , logger_(::mldp_pvxs_driver::util::log::newLogger("reader:epics-archiver:" + cfg.get("name")))
    , http_client_(nullptr)
    , name_(cfg.get("name"))
    , config_(cfg)
{
    // Validate configuration
    if (!config_.valid())
    {
        throw EpicsArchiverReaderConfig::Error("Failed to parse Archiver reader configuration");
    }

    // Initialize reusable HTTP transport for archiver communication
    try
    {
        initializeHttpClient();
    }
    catch (const std::exception& e)
    {
        throw EpicsArchiverReaderConfig::Error(std::string("Failed to initialize HTTP client: ") + e.what());
    }

    startWorker();
}

EpicsArchiverReader::~EpicsArchiverReader()
{
    // Stop worker before transport teardown so no background access races with
    // HTTP client destruction.
    stopWorker();
    destroyHttpClient();
}

std::string EpicsArchiverReader::name() const
{
    return name_;
}

void EpicsArchiverReader::initializeHttpClient()
{
    auto client = std::make_unique<::mldp_pvxs_driver::util::http::CurlHttpClient>(logger_);

    HttpClientOptions options;
    options.connect_timeout_sec = config_.connectTimeoutSec();
    options.total_timeout_sec = config_.totalTimeoutSec();
    options.tls.verify_peer = config_.tlsVerifyPeer();
    options.tls.verify_host = config_.tlsVerifyHost();
    options.user_agent = "MLDP Archiver Reader 1.0 (mldp-pvxs-driver)";

    client->setDefaultOptions(options);
    client->setDefaultHeaders({"Accept: application/octet-stream"});

    http_client_ = std::move(client);
    debugf(
        *logger_,
        "Initialized HTTP client for Archiver reader with options: connect_timeout={}s total_timeout={}s tls(peer={},host={})",
        options.connect_timeout_sec,
        options.total_timeout_sec,
        options.tls.verify_peer,
        options.tls.verify_host);
}

void EpicsArchiverReader::destroyHttpClient()
{
    http_client_.reset();
}

void EpicsArchiverReader::startWorker()
{
    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_error_ = nullptr;
        worker_done_ = false;
    }

    reader_thread_ = std::thread([this]()
                                 {
                                     runWorker();
                                 });
}

void EpicsArchiverReader::stopWorker()
{
    running_.store(false);
    worker_cv_.notify_all();
    if (http_client_)
    {
        // Interrupt any blocking streamGet() so join() is not held until a long
        // network timeout expires during destruction.
        http_client_->cancelOngoingRequests();
    }

    if (reader_thread_.joinable())
    {
        reader_thread_.join();
    }
}

void EpicsArchiverReader::runWorker()
{
    try
    {
        switch (config_.fetchMode())
        {
        case EpicsArchiverReaderConfig::FetchMode::HistoricalOnce:
            {
                // Historical archiver reader performs a one-shot fetch over the
                // configured time window, but it runs on a dedicated reader thread to
                // match the lifecycle model used by other readers.
                if (running_.load())
                {
                    fetchConfiguredPVs();
                    if (config_.batchFlushIntervalMs() > 0)
                    {
                        flushAllPendingBatches();
                    }
                    signalCompleted();
                }
                break;
            }
        case EpicsArchiverReaderConfig::FetchMode::PeriodicTail:
            {
                infof(*logger_,
                      "Archiver reader '{}' running in periodic_tail mode (poll_interval={}s lookback={}s)",
                      name_,
                      config_.pollIntervalSec(),
                      config_.lookbackSec());

                std::optional<std::chrono::system_clock::time_point> previous_iteration_end;
                const bool                                           contiguous_windows = (config_.lookbackSec() == config_.pollIntervalSec());

                while (running_.load())
                {
                    const auto iteration_end = DateTimeUtils::truncateToMilliseconds(std::chrono::system_clock::now());
                    auto       iteration_start = iteration_end - std::chrono::seconds(config_.lookbackSec());
                    if (contiguous_windows && previous_iteration_end.has_value())
                    {
                        iteration_start = *previous_iteration_end;
                    }

                    const std::string from = DateTimeUtils::formatIso8601UtcMillis(iteration_start);
                    const std::string to = DateTimeUtils::formatIso8601UtcMillis(iteration_end);
                    debugf(*logger_, "Periodic tail fetch for '{}' window [{} -> {}]", name_, from, to);
                    fetchConfiguredPVs(from, to);
                    previous_iteration_end = iteration_end;

                    std::unique_lock<std::mutex> lock(worker_mutex_);
                    worker_cv_.wait_for(lock,
                                        std::chrono::seconds(config_.pollIntervalSec()),
                                        [this]()
                                        {
                                            return !running_.load();
                                        });
                }
                if (config_.batchFlushIntervalMs() > 0)
                {
                    flushAllPendingBatches();
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
            debugf(*logger_, "Archiver reader worker '{}' stopped during shutdown: {}", name_, e.what());
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(worker_mutex_);
                worker_error_ = std::current_exception();
            }
            errorf(*logger_, "Archiver reader worker '{}' failed: {}", name_, e.what());
        }
    }
    catch (...)
    {
        if (!running_.load())
        {
            debugf(*logger_, "Archiver reader worker '{}' stopped during shutdown", name_);
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(worker_mutex_);
                worker_error_ = std::current_exception();
            }
            errorf(*logger_, "Archiver reader worker '{}' failed with unknown exception", name_);
        }
    }

    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_done_ = true;
    }
}

void EpicsArchiverReader::submitPendingBatch(const std::string& pv)
{
    auto it = pending_pv_batches_.find(pv);
    if (it == pending_pv_batches_.end() || it->second.accumulated.timestamps.empty())
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

    pending_pv_batches_.erase(it);

    if (!ts_payload.frames.empty())
    {
        batch.payload = std::move(ts_payload);
        batch.reader_name = name();
        bus_->push(std::move(batch));

        const double flush_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - flush_start).count();
        metric_call(metrics_, [&](auto& m)
                    {
                        m.observeReaderProcessingTimeMs(flush_ms, source_tag);
                        m.incrementReaderEvents(1.0, source_tag);
                    });
    }
}

void EpicsArchiverReader::flushAllPendingBatches()
{
    std::vector<std::string> pvs;
    pvs.reserve(pending_pv_batches_.size());
    for (const auto& [pv, pending] : pending_pv_batches_)
    {
        if (!pending.accumulated.timestamps.empty())
        {
            pvs.push_back(pv);
        }
    }
    for (const auto& pv : pvs)
    {
        submitPendingBatch(pv);
    }
}

void EpicsArchiverReader::flushExpiredPendingBatches()
{
    if (config_.batchFlushIntervalMs() <= 0)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto threshold = std::chrono::milliseconds(config_.batchFlushIntervalMs());

    std::vector<std::string> to_flush;
    for (const auto& [pv, pending] : pending_pv_batches_)
    {
        if (!pending.accumulated.timestamps.empty() && (now - pending.created_at) >= threshold)
        {
            to_flush.push_back(pv);
        }
    }
    for (const auto& pv : to_flush)
    {
        submitPendingBatch(pv);
    }
}

void EpicsArchiverReader::flushChunk(PbChunkState& state)
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
        // Build merged metadata: reader-level base, PV-level overrides
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
            bus_->push(std::move(batch));
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

void EpicsArchiverReader::finalizeChunk(PbChunkState& state)
{
    // Called at PB/HTTP chunk boundary (blank line) or end-of-stream. This
    // flushes any pending events, then resets the full chunk state so the next
    // non-empty line is interpreted as a new PayloadInfo header.
    if (!state.have_header)
    {
        return;
    }

    flushChunk(state);
    state = PbChunkState{};
}

void EpicsArchiverReader::parsePbHttpLineIntoState(const std::string& line, PbChunkState& state)
{
    // PB/HTTP framing is line-based after transport streaming:
    // - first line in a chunk: EPICS::PayloadInfo
    // - following lines: sample payloads (ScalarDouble currently supported)
    // - empty line: chunk terminator (handled by fetchConfiguredPVs)
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

    // Record that we received a sample from the archiver
    const std::string        pv = state.header.pvname();
    const prometheus::Labels source_tag{{"source", pv}};
    metric_call(metrics_, [&](auto& m)
                {
                    m.incrementReaderEventsReceived(1.0, source_tag);
                });

    // --- Duplicate suppression (periodic_tail only) ---
    if (config_.fetchMode() == EpicsArchiverReaderConfig::FetchMode::PeriodicTail)
    {
        auto it = last_published_ns_per_pv_.find(pv);
        if (it != last_published_ns_per_pv_.end())
        {
            const auto [wm_epoch, wm_nano] = it->second;
            // Skip the sample if its timestamp does not strictly exceed the watermark.
            if (!sampleTimeLessThan(wm_epoch, wm_nano, parsed.epoch_seconds, parsed.nanoseconds))
            {
                debugf(*logger_,
                       "Skipping duplicate sample for PV '{}' at ({}, {}): not newer than watermark ({}, {})",
                       pv, parsed.epoch_seconds, parsed.nanoseconds, wm_epoch, wm_nano);
                return;
            }
        }
        // Update watermark as this sample is accepted.
        last_published_ns_per_pv_[pv] = {parsed.epoch_seconds, parsed.nanoseconds};
    }

    if (config_.pvSamplesPerBatch() > 0)
    {
        // Per-PV sample-count batching path: accumulate into pending_pv_batches_.
        auto& pending = pending_pv_batches_[pv];
        if (pending.accumulated.timestamps.empty())
        {
            // First sample for this batch — initialize metadata.
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
            submitPendingBatch(pv);
        }
    }
    else
    {
        state.events.emplace_back(std::move(parsed.batch));
    }
}

void EpicsArchiverReader::fetchConfiguredPVs()
{
    fetchConfiguredPVs(config_.startDate(), config_.endDate());
}

void EpicsArchiverReader::fetchConfiguredPVs(const std::string& from, const std::optional<std::string>& to)
{
    // High-level flow:
    // 1) Build Archiver Appliance PB/HTTP URL for each configured PV.
    // 2) Stream HTTP bytes incrementally.
    // 3) Reconstruct PB/HTTP lines from streamed bytes.
    // 4) Parse header/sample lines into PbChunkState.
    // 5) Flush batches either on historical time-window overflow or PB/HTTP
    //    chunk/end-of-stream boundaries.
    if (!http_client_)
    {
        throw std::runtime_error("HTTP client not initialized");
    }
    if (!bus_)
    {
        throw std::runtime_error("Event bus is not available");
    }

    for (const auto& pv : config_.pvNames())
    {
        if (!running_.load())
        {
            break;
        }

        const std::string url = buildArchiverUrl(config_, pv, from, to);
        infof(*logger_, "Fetching archiver PB/HTTP stream for PV '{}' from {}", pv, url);

        PbChunkState     chunk_state;
        std::string      line_buf;
        std::size_t      total_bytes = 0;
        const auto       fetch_start = std::chrono::steady_clock::now();
        HttpResponseInfo response = http_client_->streamGet(
            HttpRequest{.url = url},
            [&](const char* data, std::size_t size)
            {
                total_bytes += size;
                // The HTTP client may deliver arbitrary byte fragment sizes.
                // Reassemble newline-delimited PB/HTTP records before parsing.
                for (std::size_t i = 0; i < size; ++i)
                {
                    const char ch = data[i];
                    if (ch == '\n')
                    {
                        if (line_buf.empty())
                        {
                            // Empty line marks PB/HTTP chunk end; publish any
                            // remaining events and reset chunk/header state.
                            finalizeChunk(chunk_state);
                        }
                        else
                        {
                            // Non-empty line belongs to the current PB/HTTP
                            // chunk: parse header or sample and apply
                            // historical time-based batch splitting.
                            try
                            {
                                parsePbHttpLineIntoState(line_buf, chunk_state);
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
            // Support responses that do not end with a trailing newline.
            try
            {
                parsePbHttpLineIntoState(line_buf, chunk_state);
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
        // Ensure the final partially accumulated chunk/batch is published.
        finalizeChunk(chunk_state);

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
    }

    // Flush pending PV batches whose age exceeds the configured interval.
    flushExpiredPendingBatches();
}
