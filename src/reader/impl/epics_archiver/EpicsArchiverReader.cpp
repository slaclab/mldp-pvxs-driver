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
#include <reader/impl/epics_archiver/EpicsArchiverReader.h>

#include <EPICSEvent.pb.h>
#include <util/bus/IEventBusPush.h>
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
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::http;
using namespace mldp_pvxs_driver::util::time;
using namespace mldp_pvxs_driver::reader::impl::epics_archiver;

namespace {

constexpr const char* kArchiverPbRawPath = "/retrieval/data/getData.raw";

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

bool isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint64_t unixEpochSecondsFromYearAndSecondsIntoYear(int year, uint32_t seconds_into_year)
{
    if (year < 1970)
    {
        throw std::runtime_error("PB/HTTP sample year before 1970 is unsupported");
    }

    uint64_t days = 0;
    for (int y = 1970; y < year; ++y)
    {
        days += isLeapYear(y) ? 366u : 365u;
    }
    return days * 86400ULL + static_cast<uint64_t>(seconds_into_year);
}

bool sampleTimeLessThan(uint64_t lhs_epoch, uint32_t lhs_nano, uint64_t rhs_epoch, uint32_t rhs_nano)
{
    return (lhs_epoch < rhs_epoch) || (lhs_epoch == rhs_epoch && lhs_nano < rhs_nano);
}

uint64_t elapsedNanoseconds(uint64_t start_epoch, uint32_t start_nano, uint64_t end_epoch, uint32_t end_nano)
{
    uint64_t sec_delta = end_epoch - start_epoch;
    if (end_nano >= start_nano)
    {
        return sec_delta * 1'000'000'000ULL + static_cast<uint64_t>(end_nano - start_nano);
    }

    return (sec_delta - 1ULL) * 1'000'000'000ULL + (1'000'000'000ULL + static_cast<uint64_t>(end_nano) - start_nano);
}

} // namespace

EpicsArchiverReader::EpicsArchiverReader(
    std::shared_ptr<::mldp_pvxs_driver::util::bus::IEventBusPush> bus,
    std::shared_ptr<::mldp_pvxs_driver::metrics::Metrics>         metrics,
    const ::mldp_pvxs_driver::config::Config&                     cfg)
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
                }
                break;
            }
        case EpicsArchiverReaderConfig::FetchMode::PeriodicTail: {
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

void EpicsArchiverReader::flushChunk(PbChunkState& state)
{
    // Flush only the currently accumulated output batch (events vector) while
    // keeping the parsed PB/HTTP chunk header. This is used for time-based
    // splitting inside a single PB/HTTP chunk.
    if (!state.have_header)
    {
        return;
    }

    const std::string& pv = state.header.pvname();
    if (!state.events.empty())
    {
        mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch batch;
        batch.root_source = pv.empty() ? name_ : pv;
        batch.tags.push_back(batch.root_source);
        batch.values[pv.empty() ? name_ : pv] = std::move(state.events);
        bus_->push(std::move(batch));
    }

    state.have_batch_start_time = false;
    state.batch_start_epoch_seconds = 0;
    state.batch_start_nanoseconds = 0;
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

void EpicsArchiverReader::splitBatchIfHistoricalWindowExceeded(PbChunkState& state,
                                                               uint64_t      sample_epoch_seconds,
                                                               uint32_t      sample_nanoseconds)
{
    // Batch splitting is based on historical sample timestamps from the
    // archiver payload, not wall-clock processing time. The first sample starts
    // the batch window; later samples trigger a flush only when they exceed the
    // configured threshold (strict overflow: elapsed > batch_duration_sec).
    if (state.events.empty())
    {
        state.have_batch_start_time = true;
        state.batch_start_epoch_seconds = sample_epoch_seconds;
        state.batch_start_nanoseconds = sample_nanoseconds;
        return;
    }

    if (!state.have_batch_start_time)
    {
        state.have_batch_start_time = true;
        state.batch_start_epoch_seconds = sample_epoch_seconds;
        state.batch_start_nanoseconds = sample_nanoseconds;
        return;
    }

    if (sampleTimeLessThan(sample_epoch_seconds,
                           sample_nanoseconds,
                           state.batch_start_epoch_seconds,
                           state.batch_start_nanoseconds))
    {
        // Recover safely from unexpected out-of-order data by publishing current batch first.
        flushChunk(state);
        state.have_batch_start_time = true;
        state.batch_start_epoch_seconds = sample_epoch_seconds;
        state.batch_start_nanoseconds = sample_nanoseconds;
        return;
    }

    const uint64_t threshold_ns = static_cast<uint64_t>(config_.batchDurationSec()) * 1'000'000'000ULL;
    const uint64_t elapsed_ns = elapsedNanoseconds(state.batch_start_epoch_seconds,
                                                   state.batch_start_nanoseconds,
                                                   sample_epoch_seconds,
                                                   sample_nanoseconds);
    if (elapsed_ns > threshold_ns)
    {
        flushChunk(state);
        state.have_batch_start_time = true;
        state.batch_start_epoch_seconds = sample_epoch_seconds;
        state.batch_start_nanoseconds = sample_nanoseconds;
    }
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

    if (state.header.type() != EPICS::SCALAR_DOUBLE)
    {
        throw std::runtime_error("unsupported archiver PB/HTTP payload type (only SCALAR_DOUBLE is implemented)");
    }

    EPICS::ScalarDouble sample;
    if (!sample.ParseFromString(msg_bytes))
    {
        throw std::runtime_error("failed to parse PB/HTTP ScalarDouble sample");
    }

    const auto epoch_seconds = unixEpochSecondsFromYearAndSecondsIntoYear(
        state.header.year(),
        sample.secondsintoyear());
    // Decide whether the current sample starts a new output batch before
    // appending it, so the sample that crosses the threshold belongs to the
    // new batch.
    splitBatchIfHistoricalWindowExceeded(state, epoch_seconds, sample.nano());
    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(epoch_seconds, sample.nano());
    ev->data_value.set_doublevalue(sample.val());
    state.events.emplace_back(std::move(ev));
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
        HttpResponseInfo response = http_client_->streamGet(
            HttpRequest{.url = url},
            [&](const char* data, std::size_t size)
            {
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
                            parsePbHttpLineIntoState(line_buf, chunk_state);
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
            parsePbHttpLineIntoState(line_buf, chunk_state);
            line_buf.clear();
        }
        // Ensure the final partially accumulated chunk/batch is published.
        finalizeChunk(chunk_state);

        if (response.http_status != 200)
        {
            throw std::runtime_error(
                "archiver HTTP GET returned status " + std::to_string(response.http_status) + " for PV " + pv);
        }
        infof(*logger_, "Completed fetch of archiver PB/HTTP stream for PV '{}'", pv);
    }
}
