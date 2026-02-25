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

#include <util/http/CurlHttpClient.h>
#include <util/http/HttpClient.h>
#include <util/log/ILog.h>

#include <exception>
#include <utility>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::http;
using namespace mldp_pvxs_driver::reader::impl::epics_archiver;


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

    // TODO: Implement initialization for Archiver Appliance reader
    // - Set up connection to archiver service at config_.hostname()
    // - Initialize data retrieval for PVs in config_.pvNames()
    // - Query [startDate, endDate] time window (endDate may be omitted for open-ended reads)
    // - Start background polling/fetching threads
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
