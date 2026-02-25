//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics_archiver/EpicsArchiverReader.h>

using namespace mldp_pvxs_driver::reader::impl::epics_archiver;
using namespace mldp_pvxs_driver::config;

EpicsArchiverReader::EpicsArchiverReader(
    std::shared_ptr<::mldp_pvxs_driver::util::bus::IEventBusPush> bus,
    std::shared_ptr<::mldp_pvxs_driver::metrics::Metrics>         metrics,
    const ::mldp_pvxs_driver::config::Config&                     cfg)
    : ::mldp_pvxs_driver::reader::Reader(std::move(bus), std::move(metrics))
    , name_(cfg.get("name"))
    , config_(cfg)
    , curl_handle_(nullptr)
{
    // Validate configuration
    if (!config_.valid())
    {
        throw EpicsArchiverReaderConfig::Error("Failed to parse Archiver reader configuration");
    }

    // Initialize CURL for HTTP communication with archiver service
    try
    {
        initializeCurl();
    }
    catch (const std::exception& e)
    {
        throw EpicsArchiverReaderConfig::Error(std::string("Failed to initialize CURL: ") + e.what());
    }

    // TODO: Implement initialization for Archiver Appliance reader
    // - Set up connection to archiver service at config_.hostname()
    // - Initialize data retrieval for PVs in config_.pvNames()
    // - Query [startDate, endDate] time window (endDate may be omitted for open-ended reads)
    // - Start background polling/fetching threads
}

EpicsArchiverReader::~EpicsArchiverReader()
{
    // Clean up CURL and archiver reader resources
    destroyCurl();
}

std::string EpicsArchiverReader::name() const
{
    return name_;
}

void EpicsArchiverReader::initializeCurl()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_handle_ = curl_easy_init();

    if (!curl_handle_)
    {
        throw std::runtime_error("Failed to initialize CURL handle");
    }

    // === Connection Timeouts ===
    // Use configurable timeouts from EpicsArchiverReaderConfig
    curl_easy_setopt(curl_handle_, CURLOPT_CONNECTTIMEOUT, config_.connectTimeoutSec());
    curl_easy_setopt(curl_handle_, CURLOPT_TIMEOUT, config_.totalTimeoutSec());

    // === HTTP Chunking Configuration ===
    // Enable chunked transfer encoding for streaming large responses
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl_handle_, CURLOPT_HTTPHEADER, headers);

    // === HTTP Protocol Settings ===
    // Follow redirects automatically (e.g., if archiver redirects)
    curl_easy_setopt(curl_handle_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle_, CURLOPT_MAXREDIRS, 5L);

    // === SSL/TLS Configuration ===
    // Note: SSL verification is disabled for internal SLAC archiver (adjust for production)
    curl_easy_setopt(curl_handle_, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_handle_, CURLOPT_SSL_VERIFYHOST, 0L);

    // === TCP Keep-Alive for Connection Persistence ===
    // Maintain persistent connections with keep-alive probes
    curl_easy_setopt(curl_handle_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl_handle_, CURLOPT_TCP_KEEPIDLE, 120L);      // Start probes after 2 minutes idle
    curl_easy_setopt(curl_handle_, CURLOPT_TCP_KEEPINTVL, 60L);      // Probe interval: 60 seconds

    // === Buffer and Performance Tuning ===
    // Set buffer size for HTTP requests (64KB - good balance for archiver protobuf data)
    curl_easy_setopt(curl_handle_, CURLOPT_BUFFERSIZE, 65536L);
    // Low speed limit: if less than 1KB/sec for 60 sec, timeout
    curl_easy_setopt(curl_handle_, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl_handle_, CURLOPT_LOW_SPEED_TIME, 60L);

    // === Compression Support ===
    // Accept gzip and deflate compression from archiver responses
    curl_easy_setopt(curl_handle_, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");

    // === User Agent ===
    // Identify client for logging and debugging on server side
    curl_easy_setopt(curl_handle_, CURLOPT_USERAGENT, "MLDP Archiver Reader 1.0 (mldp-pvxs-driver)");

    // === Verbose Mode (only in debug builds) ===
#ifdef DEBUG
    curl_easy_setopt(curl_handle_, CURLOPT_VERBOSE, 1L);
#endif
}

void EpicsArchiverReader::destroyCurl()
{
    if (curl_handle_)
    {
        curl_easy_cleanup(curl_handle_);
        curl_handle_ = nullptr;
    }
    curl_global_cleanup();
}
