//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <cli/ConfigPrinter.h>

#include <controller/MLDPPVXSControllerConfig.h>
#include <pool/MLDPGrpcPoolConfig.h>
#include <reader/impl/epics/shared/EpicsReaderConfig.h>
#include <reader/impl/epics_archiver/EpicsArchiverReaderConfig.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

using mldp_pvxs_driver::controller::MLDPPVXSControllerConfig;
using mldp_pvxs_driver::reader::impl::epics::EpicsReaderConfig;
using mldp_pvxs_driver::reader::impl::epics_archiver::EpicsArchiverReaderConfig;
using mldp_pvxs_driver::util::pool::MLDPGrpcPoolConfig;

namespace {

constexpr const char* kEpicsPvxsType = "epics-pvxs";
constexpr const char* kEpicsBaseType = "epics-base";
constexpr const char* kEpicsArchiverType = "epics-archiver";

std::string previewList(const std::vector<std::string>& values, std::size_t maxItems = 3)
{
    if (values.empty())
    {
        return "-";
    }

    std::ostringstream out;
    const std::size_t  shown = std::min(values.size(), maxItems);
    for (std::size_t i = 0; i < shown; ++i)
    {
        if (i > 0)
        {
            out << ", ";
        }
        out << values[i];
    }
    if (values.size() > shown)
    {
        out << ", +" << (values.size() - shown) << " more";
    }
    return out.str();
}

std::string credentialsSummary(const MLDPGrpcPoolConfig::Credentials& credentials)
{
    if (credentials.type == MLDPGrpcPoolConfig::Credentials::Type::Insecure)
    {
        return "none";
    }

    const bool hasCustomMaterial =
        !credentials.ssl_options.pem_cert_chain.empty() ||
        !credentials.ssl_options.pem_private_key.empty() ||
        !credentials.ssl_options.pem_root_certs.empty();

    return hasCustomMaterial ? "ssl(custom-pem)" : "ssl(system-default)";
}

std::string archiverModeSummary(EpicsArchiverReaderConfig::FetchMode mode)
{
    return mode == EpicsArchiverReaderConfig::FetchMode::PeriodicTail ? "periodic_tail" : "historical_once";
}

} // namespace

std::string mldp_pvxs_driver::cli::formatStartupConfig(
    const mldp_pvxs_driver::config::Config& root,
    const std::string&                      configPath)
{
    const MLDPPVXSControllerConfig controllerConfig(root);

    std::ostringstream out;
    out << "=== Effective Startup Configuration ===\n";
    out << "file: " << configPath << "\n";

    for (const auto& grpcCfg : controllerConfig.writerConfig().grpcConfigs)
    {
        out << "writer.grpc[" << grpcCfg.name << "]: threads=" << grpcCfg.threadPoolSize
            << " stream_max_bytes=" << grpcCfg.streamMaxBytes
            << " stream_max_age_ms=" << grpcCfg.streamMaxAge.count() << "\n";

        const auto& pool = grpcCfg.poolConfig;
        out << "  mldp-pool: provider=" << pool.providerName()
            << " conn=" << pool.minConnections() << ".." << pool.maxConnections()
            << " ingestion=" << pool.ingestionUrl()
            << " query=" << pool.queryUrl()
            << " credentials=" << credentialsSummary(pool.credentials())
            << "\n";
    }
    for (const auto& hdf5Cfg : controllerConfig.writerConfig().hdf5Configs)
    {
        out << "writer.hdf5[" << hdf5Cfg.name << "]: base-path=" << hdf5Cfg.basePath << "\n";
    }

    if (controllerConfig.metricsConfig().has_value() && controllerConfig.metricsConfig()->valid())
    {
        out << "metrics: enabled endpoint=" << controllerConfig.metricsConfig()->endpoint()
            << " scan_interval_s=" << controllerConfig.metricsConfig()->scanIntervalSeconds() << "\n";
    }
    else
    {
        out << "metrics: disabled\n";
    }

    const auto& readers = controllerConfig.readerEntries();
    out << "readers: count=" << readers.size() << "\n";
    for (std::size_t i = 0; i < readers.size(); ++i)
    {
        const auto& [type, cfg] = readers[i];
        out << "  [" << (i + 1) << "] type=" << type << " ";

        if (type == kEpicsPvxsType || type == kEpicsBaseType)
        {
            const EpicsReaderConfig reader(cfg);
            out << "name=" << reader.name()
                << " pvs=" << reader.pvs().size()
                << " thread_pool=" << reader.threadPoolSize()
                << " monitor_poll_threads=" << reader.monitorPollThreads()
                << " monitor_poll_ms=" << reader.monitorPollIntervalMs()
                << " column_batch_size=" << reader.columnBatchSize()
                << " pv_preview=[" << previewList(reader.pvNames()) << "]";
        }
        else if (type == kEpicsArchiverType)
        {
            const EpicsArchiverReaderConfig reader(cfg);
            out << "name=" << reader.name()
                << " mode=" << archiverModeSummary(reader.fetchMode())
                << " host=" << reader.hostname()
                << " pvs=" << reader.pvs().size()
                << " batch_duration_s=" << reader.batchDurationSec();
            if (reader.fetchMode() == EpicsArchiverReaderConfig::FetchMode::PeriodicTail)
            {
                out << " poll_interval_s=" << reader.pollIntervalSec()
                    << " lookback_s=" << reader.lookbackSec();
            }
            else
            {
                out << " start_date=" << reader.startDate();
            }
            out << " pv_preview=[" << previewList(reader.pvNames()) << "]";
        }
        else
        {
            out << "summary=unavailable";
        }

        out << "\n";
    }

    out << "=======================================\n";
    return out.str();
}

namespace {
void collectFlatRows(const mldp_pvxs_driver::config::ryml::ConstNodeRef& node,
                     const std::string&                                  path,
                     std::vector<std::pair<std::string, std::string>>&   rows)
{
    if (node.invalid())
    {
        return;
    }

    if (node.is_map())
    {
        for (const auto child : node.children())
        {
            if (!child.has_key())
            {
                continue;
            }
            const auto        keyView = child.key();
            const std::string key{keyView.str, keyView.len};
            const std::string nextPath = path.empty() ? key : (path + "." + key);
            collectFlatRows(child, nextPath, rows);
        }
        return;
    }

    if (node.is_seq())
    {
        std::size_t idx = 0;
        for (const auto child : node.children())
        {
            const std::string nextPath = path + "[" + std::to_string(idx++) + "]";
            collectFlatRows(child, nextPath, rows);
        }
        if (idx == 0)
        {
            rows.emplace_back(path, "[]");
        }
        return;
    }

    if (!path.empty())
    {
        std::string value;
        if (node.has_val())
        {
            node >> value;
        }
        rows.emplace_back(path, value);
    }
}
} // namespace

std::string mldp_pvxs_driver::cli::formatConfigKeyValueTable(
    const mldp_pvxs_driver::config::Config& root,
    const std::string&                      configPath)
{
    std::vector<std::pair<std::string, std::string>> rows;
    collectFlatRows(root.raw(), "", rows);

    std::size_t keyWidth = std::string("Key").size();
    for (const auto& row : rows)
    {
        keyWidth = std::max(keyWidth, row.first.size());
    }

    std::ostringstream out;
    out << "=== Configuration Table (" << configPath << ") ===\n";
    out << std::left << std::setw(static_cast<int>(keyWidth)) << "Key" << " | Value\n";
    out << std::string(keyWidth, '-') << "-|------\n";
    for (const auto& [k, v] : rows)
    {
        out << std::left << std::setw(static_cast<int>(keyWidth)) << k << " | " << v << "\n";
    }
    out << "=======================================\n";
    return out.str();
}
