//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics_archiver/EpicsArchiverReaderConfig.h>

#include <config/Config.h>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::reader::impl::epics_archiver;

namespace {
std::string getAliasedString(const Config&      cfg,
                             const std::string& primaryKey,
                             const std::string& aliasKey,
                             const std::string& def = "")
{
    if (cfg.hasChild(primaryKey))
    {
        return cfg.get(primaryKey, def);
    }
    return cfg.get(aliasKey, def);
}

void requireScalarChild(const Config& cfg, const std::string& key, const std::string& context)
{
    const auto raw = cfg.raw();
    if (!raw.has_child(key.c_str()))
    {
        return;
    }

    const auto child = raw[key.c_str()];
    if (!child.has_val())
    {
        throw EpicsArchiverReaderConfig::Error(context + "." + key + " must be a scalar");
    }
}

void requireScalarChildAny(const Config&      cfg,
                           const std::string& primaryKey,
                           const std::string& aliasKey,
                           const std::string& context)
{
    if (cfg.hasChild(primaryKey))
    {
        requireScalarChild(cfg, primaryKey, context);
        return;
    }
    if (cfg.hasChild(aliasKey))
    {
        requireScalarChild(cfg, aliasKey, context);
    }
}
} // namespace

EpicsArchiverReaderConfig::EpicsArchiverReaderConfig() = default;

EpicsArchiverReaderConfig::EpicsArchiverReaderConfig(
    const ::mldp_pvxs_driver::config::Config& readerEntry)
{
    if (!readerEntry.valid())
    {
        throw Error("Reader entry is invalid");
    }

    parse(readerEntry);
}

bool EpicsArchiverReaderConfig::valid() const
{
    return valid_;
}

const std::string& EpicsArchiverReaderConfig::name() const
{
    return name_;
}

const std::string& EpicsArchiverReaderConfig::hostname() const
{
    return hostname_;
}

const std::string& EpicsArchiverReaderConfig::startDate() const
{
    return start_date_;
}

const std::optional<std::string>& EpicsArchiverReaderConfig::endDate() const
{
    return end_date_;
}

EpicsArchiverReaderConfig::FetchMode EpicsArchiverReaderConfig::fetchMode() const
{
    return fetch_mode_;
}

const std::vector<EpicsArchiverReaderConfig::PVConfig>& EpicsArchiverReaderConfig::pvs() const
{
    return pvs_;
}

const std::vector<std::string>& EpicsArchiverReaderConfig::pvNames() const
{
    return pvNames_;
}

long EpicsArchiverReaderConfig::connectTimeoutSec() const
{
    return connect_timeout_sec_;
}

long EpicsArchiverReaderConfig::totalTimeoutSec() const
{
    return total_timeout_sec_;
}

long EpicsArchiverReaderConfig::batchDurationSec() const
{
    return batch_duration_sec_;
}

long EpicsArchiverReaderConfig::pollIntervalSec() const
{
    return poll_interval_sec_;
}

long EpicsArchiverReaderConfig::lookbackSec() const
{
    return lookback_sec_;
}

bool EpicsArchiverReaderConfig::tlsVerifyPeer() const
{
    return tls_verify_peer_;
}

bool EpicsArchiverReaderConfig::tlsVerifyHost() const
{
    return tls_verify_host_;
}

void EpicsArchiverReaderConfig::parse(const Config& readerEntry)
{
    using namespace mldp_pvxs_driver::reader::impl::epics_archiver;

    // Parse reader name
    if (!readerEntry.hasChild(NameKey))
    {
        throw Error(makeMissingFieldMessage(NameKey));
    }

    const auto nameNodes = readerEntry.subConfig(NameKey);
    if (nameNodes.empty())
    {
        throw Error(makeMissingFieldMessage(NameKey));
    }

    const auto& nameNode = nameNodes.front();
    if (!nameNode.raw().has_val())
    {
        throw Error("name must be a scalar");
    }

    nameNode >> name_;
    if (name_.empty())
    {
        throw Error("name must not be empty");
    }

    // Parse hostname
    if (!readerEntry.hasChild(HostnameKey))
    {
        throw Error(makeMissingFieldMessage(HostnameKey));
    }

    requireScalarChild(readerEntry, std::string(HostnameKey), "archiver reader config");
    hostname_ = readerEntry.get(HostnameKey);
    if (hostname_.empty())
    {
        throw Error("hostname must not be empty");
    }

    // Parse optional fetch mode (default: historical one-shot for backward compatibility)
    const std::string mode = readerEntry.get(ModeKey, "historical_once");
    if (mode == "historical_once")
    {
        fetch_mode_ = FetchMode::HistoricalOnce;
    }
    else if (mode == "periodic_tail")
    {
        fetch_mode_ = FetchMode::PeriodicTail;
    }
    else
    {
        throw Error("mode must be one of: historical_once, periodic_tail");
    }

    // Parse start date (required in historical_once mode; ignored in periodic_tail)
    requireScalarChildAny(readerEntry, std::string(StartDateKey), std::string(StartDateAliasKey), "archiver reader config");
    start_date_ = getAliasedString(readerEntry, std::string(StartDateKey), std::string(StartDateAliasKey));
    if (fetch_mode_ == FetchMode::HistoricalOnce)
    {
        if (!readerEntry.hasChild(StartDateKey) && !readerEntry.hasChild(StartDateAliasKey))
        {
            throw Error(makeMissingFieldMessage(StartDateKey));
        }
        if (start_date_.empty())
        {
            throw Error("start-date must not be empty");
        }
    }

    // Parse optional end date (accept dash-case and camelCase)
    requireScalarChildAny(readerEntry, std::string(EndDateKey), std::string(EndDateAliasKey), "archiver reader config");
    const auto endDate = getAliasedString(readerEntry, std::string(EndDateKey), std::string(EndDateAliasKey));
    if (readerEntry.hasChild(EndDateKey) || readerEntry.hasChild(EndDateAliasKey))
    {
        if (endDate.empty())
        {
            throw Error("end-date must not be empty when provided");
        }
        end_date_ = endDate;
    }
    else
    {
        end_date_.reset();
    }

    // Parse optional connection timeout (default: 30 seconds)
    connect_timeout_sec_ = readerEntry.getInt(ConnectTimeoutSecKey, 30L);
    if (connect_timeout_sec_ <= 0)
    {
        throw Error("connect-timeout-sec must be positive (>0)");
    }

    // Parse optional total timeout (default: 300 seconds / 5 minutes)
    // Special case: 0 means infinite timeout (useful for long streaming sessions)
    total_timeout_sec_ = readerEntry.getInt(TotalTimeoutSecKey, 300L);
    if (total_timeout_sec_ < 0)
    {
        throw Error("total-timeout-sec must be >= 0 (0 = infinite for streaming)");
    }
    if (total_timeout_sec_ != 0 && total_timeout_sec_ < connect_timeout_sec_)
    {
        throw Error("total-timeout-sec must be >= connect-timeout-sec (or 0 for infinite)");
    }

    // Parse optional historical batch duration threshold (default: 1 second)
    batch_duration_sec_ = readerEntry.getInt(BatchDurationSecKey, 1L);
    if (batch_duration_sec_ <= 0)
    {
        throw Error("batch-duration-sec must be positive (>0)");
    }

    // Parse periodic tail polling controls
    if (fetch_mode_ == FetchMode::PeriodicTail)
    {
        poll_interval_sec_ = readerEntry.getInt(PollIntervalSecKey, 0L);
        if (poll_interval_sec_ <= 0)
        {
            throw Error("poll-interval-sec must be positive (>0) when mode=periodic_tail");
        }

        lookback_sec_ = readerEntry.getInt(LookbackSecKey, poll_interval_sec_);
        if (lookback_sec_ <= 0)
        {
            throw Error("lookback-sec must be positive (>0) when mode=periodic_tail");
        }
        if (lookback_sec_ > poll_interval_sec_)
        {
            throw Error("lookback-sec must be <= poll-interval-sec when mode=periodic_tail");
        }
    }

    // Parse optional TLS verification controls (secure by default)
    tls_verify_peer_ = readerEntry.getBool(TlsVerifyPeerKey, true);
    tls_verify_host_ = readerEntry.getBool(TlsVerifyHostKey, true);
    if (tls_verify_host_ && !tls_verify_peer_)
    {
        throw Error("tls-verify-host=true requires tls-verify-peer=true");
    }

    // Parse PVs
    if (!readerEntry.hasChild(PvsKey))
    {
        throw Error(makeMissingFieldMessage(PvsKey));
    }

    if (!readerEntry.isSequence(PvsKey))
    {
        throw Error("pvs must be a sequence");
    }

    const auto pvNodes = readerEntry.subConfig(PvsKey);

    pvs_.clear();
    pvNames_.clear();
    pvs_.reserve(pvNodes.size());
    pvNames_.reserve(pvNodes.size());

    for (const auto& pvNode : pvNodes)
    {
        if (!pvNode.raw().is_map())
        {
            throw Error("Each entry in pvs must be a map");
        }

        if (!pvNode.hasChild(PvNameKey))
        {
            throw Error(makeMissingFieldMessage(std::string(PvsKey) + "[]." + PvNameKey));
        }

        const auto pvNameNodes = pvNode.subConfig(PvNameKey);
        if (pvNameNodes.empty())
        {
            throw Error(makeMissingFieldMessage(std::string(PvsKey) + "[]." + PvNameKey));
        }

        const auto& pvNameNode = pvNameNodes.front();
        if (!pvNameNode.raw().has_val())
        {
            throw Error("pvs[].name must be a scalar");
        }

        std::string pvName;
        pvNameNode >> pvName;
        if (pvName.empty())
        {
            throw Error("pvs[].name must not be empty");
        }

        pvs_.push_back({std::move(pvName)});
        pvNames_.push_back(pvs_.back().name);
    }

    valid_ = true;
}
