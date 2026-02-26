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
#include <util/log/ILog.h>

#include <cctype>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::http;
using namespace mldp_pvxs_driver::reader::impl::epics_archiver;

namespace {

constexpr const char* kArchiverPbRawPath = "/retrieval/data/getData.raw";

bool hasScheme(const std::string& s)
{
    return s.find("://") != std::string::npos;
}

std::string trimTrailingSlash(std::string s)
{
    while (!s.empty() && s.back() == '/')
    {
        s.pop_back();
    }
    return s;
}

std::string percentEncode(const std::string& in)
{
    std::ostringstream os;
    os << std::uppercase << std::hex;
    for (unsigned char c : in)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            os << static_cast<char>(c);
        }
        else
        {
            os << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return os.str();
}

std::string buildArchiverUrl(const EpicsArchiverReaderConfig& cfg, const std::string& pv)
{
    std::string base = cfg.hostname();
    if (!hasScheme(base))
    {
        base = "http://" + base;
    }
    base = trimTrailingSlash(std::move(base));

    std::string url = base + kArchiverPbRawPath + "?pv=" + percentEncode(pv) + "&from=" + percentEncode(cfg.startDate());
    if (cfg.endDate().has_value())
    {
        url += "&to=" + percentEncode(*cfg.endDate());
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

void flushChunk(mldp_pvxs_driver::util::bus::IEventBusPush* bus,
                const std::string&                          reader_name,
                PbChunkState&                               state)
{
    if (!state.have_header)
    {
        return;
    }

    const std::string& pv = state.header.pvname();
    if (!state.events.empty())
    {
        mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch batch;
        batch.root_source = pv.empty() ? reader_name : pv;
        batch.tags.push_back(batch.root_source);
        batch.values[pv.empty() ? reader_name : pv] = std::move(state.events);
        bus->push(std::move(batch));
    }

    state.have_batch_start_time = false;
    state.batch_start_epoch_seconds = 0;
    state.batch_start_nanoseconds = 0;
}

void finalizeChunk(mldp_pvxs_driver::util::bus::IEventBusPush* bus,
                   const std::string&                          reader_name,
                   PbChunkState&                               state)
{
    if (!state.have_header)
    {
        return;
    }

    flushChunk(bus, reader_name, state);
    state = PbChunkState{};
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

void splitBatchIfHistoricalWindowExceeded(mldp_pvxs_driver::util::bus::IEventBusPush* bus,
                                          const std::string&                          reader_name,
                                          PbChunkState&                               state,
                                          long                                        batch_duration_sec,
                                          uint64_t                                    sample_epoch_seconds,
                                          uint32_t                                    sample_nanoseconds)
{
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
        flushChunk(bus, reader_name, state);
        state.have_batch_start_time = true;
        state.batch_start_epoch_seconds = sample_epoch_seconds;
        state.batch_start_nanoseconds = sample_nanoseconds;
        return;
    }

    const uint64_t threshold_ns = static_cast<uint64_t>(batch_duration_sec) * 1'000'000'000ULL;
    const uint64_t elapsed_ns = elapsedNanoseconds(state.batch_start_epoch_seconds,
                                                   state.batch_start_nanoseconds,
                                                   sample_epoch_seconds,
                                                   sample_nanoseconds);
    if (elapsed_ns > threshold_ns)
    {
        flushChunk(bus, reader_name, state);
        state.have_batch_start_time = true;
        state.batch_start_epoch_seconds = sample_epoch_seconds;
        state.batch_start_nanoseconds = sample_nanoseconds;
    }
}

void parsePbHttpLineIntoState(const std::string&                          line,
                              PbChunkState&                               state,
                              mldp_pvxs_driver::util::bus::IEventBusPush* bus,
                              const std::string&                          reader_name,
                              long                                        batch_duration_sec)
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
    splitBatchIfHistoricalWindowExceeded(
        bus, reader_name, state, batch_duration_sec, epoch_seconds, sample.nano());
    auto ev = mldp_pvxs_driver::util::bus::IEventBusPush::MakeEventValue(epoch_seconds, sample.nano());
    ev->data_value.set_doublevalue(sample.val());
    state.events.emplace_back(std::move(ev));
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

    try
    {
        fetchConfiguredPVs();
    }
    catch (const std::exception& e)
    {
        throw EpicsArchiverReaderConfig::Error(std::string("Failed to fetch archiver data: ") + e.what());
    }
}

EpicsArchiverReader::~EpicsArchiverReader()
{
    // Clean up transport and archiver reader resources
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

void EpicsArchiverReader::fetchConfiguredPVs()
{
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
        const std::string url = buildArchiverUrl(config_, pv);
        debugf(*logger_, "Fetching archiver PB/HTTP stream for PV '{}' from {}", pv, url);

        PbChunkState     chunk_state;
        std::string      line_buf;
        HttpResponseInfo response = http_client_->streamGet(
            HttpRequest{.url = url},
            [&](const char* data, std::size_t size)
            {
                for (std::size_t i = 0; i < size; ++i)
                {
                    const char ch = data[i];
                    if (ch == '\n')
                    {
                        if (line_buf.empty())
                        {
                            finalizeChunk(bus_.get(), name_, chunk_state);
                        }
                        else
                        {
                            parsePbHttpLineIntoState(
                                line_buf, chunk_state, bus_.get(), name_, config_.batchDurationSec());
                            line_buf.clear();
                        }
                        continue;
                    }
                    line_buf.push_back(ch);
                }
            });

        if (!line_buf.empty())
        {
            parsePbHttpLineIntoState(line_buf, chunk_state, bus_.get(), name_, config_.batchDurationSec());
            line_buf.clear();
        }
        finalizeChunk(bus_.get(), name_, chunk_state);

        if (response.http_status != 200)
        {
            throw std::runtime_error(
                "archiver HTTP GET returned status " + std::to_string(response.http_status) + " for PV " + pv);
        }
    }
}
