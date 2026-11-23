//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/pvxs/EpicsPVXSBsasGen1Reader.h>

#include <util/StringFormat.h>
#include <util/log/Logger.h>

#include <pvxs/client.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;

namespace {

/// Comparison key: the name without ':' and '_' (the IOC may rewrite either).
std::string normalize(std::string name)
{
    name.erase(std::remove_if(name.begin(), name.end(),
                              [](char c)
                              {
                                  return c == ':' || c == '_';
                              }),
               name.end());
    return name;
}

} // namespace

EpicsPVXSBsasGen1Reader::EpicsPVXSBsasGen1Reader(std::shared_ptr<util::bus::IDataBus> bus,
                                                 std::shared_ptr<metrics::Metrics>    metrics,
                                                 const config::Config&                cfg)
    : EpicsPVXSBsasTableReaderBase(std::move(bus), std::move(metrics), cfg,
                                   EpicsPVXSBsasTableReaderConfig::Generation::Gen1, kReaderType,
                                   &EpicsPVXSBsasGen1Reader::makeColumnFilter)
{
    infof(*logger_, "[{}] Resolving BSAS gen1 column names from signal PVs", name_);
    resolveSignalNames();
    infof(*logger_, "[{}] Column names resolved", name_);
    addPV(pvNames());
}

EpicsPVXSBsasGen1Reader::ColumnFilter EpicsPVXSBsasGen1Reader::makeColumnFilter(const TableOptions& /*opts*/)
{
    // A filter makes prepareTable() hand over the `labels` entry instead of
    // the ':'-mangled field name; keep every column under it.
    return [](std::string_view label) -> std::optional<std::string>
    {
        return std::string(label);
    };
}

EpicsPVXSBsasGen1Reader::ColumnNameMap EpicsPVXSBsasGen1Reader::buildColumnNameMap(const std::string&              tablePv,
                                                                                   const std::string&              signalPv,
                                                                                   const std::vector<std::string>& fields,
                                                                                   const std::vector<std::string>& signals)
{
    if (fields.size() != signals.size())
    {
        throw std::runtime_error(util::format_string(
            "BSAS gen1 table {} has {} data columns but signal PV {} lists {} names",
            tablePv, fields.size(), signalPv, signals.size()));
    }

    ColumnNameMap names;
    names.reserve(fields.size() * 2);
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        if (normalize(fields[i]) != normalize(signals[i]))
        {
            throw std::runtime_error(util::format_string(
                "BSAS gen1 table {} column {} is '{}' but signal PV {} entry {} is '{}'",
                tablePv, i, fields[i], signalPv, i, signals[i]));
        }
        names.emplace(fields[i], signals[i]);
        names.emplace(signals[i], signals[i]); // `labels` may already carry the real name
    }
    return names;
}

void EpicsPVXSBsasGen1Reader::resolveSignalNames()
{
    for (const auto& pv : pvNames())
    {
        const auto& opts = tableConfig().optionsFor(pv);
        if (opts.signalPv.empty())
        {
            infof(*logger_, "[{}/{}] no signal PV; columns named from NTTable labels", name_, pv);
            continue;
        }

        const double timeoutSec = static_cast<double>(opts.signalTimeoutMs) / 1000.0;
        std::vector<std::string> signals;
        std::vector<std::string> fields;
        infof(*logger_, "[{}/{}] reading signal PV {} (timeout {} ms)", name_, pv, opts.signalPv, opts.signalTimeoutMs);
        try
        {
            const auto sigValue = pvaContext().get(opts.signalPv).exec()->wait(timeoutSec);
            const auto sigArray = sigValue["value"];
            if (!sigArray.valid() || sigArray.type().code != pvxs::TypeCode::StringA)
            {
                throw std::runtime_error("value is not a string array");
            }
            for (const auto& s : sigArray.as<pvxs::shared_array<const std::string>>())
            {
                signals.push_back(s);
            }
            infof(*logger_, "[{}/{}] signal PV {} returned {} names", name_, pv, opts.signalPv, signals.size());
            for (std::size_t i = 0; i < signals.size(); ++i)
            {
                debugf(*logger_, "[{}/{}]   sig[{}] = '{}'", name_, pv, i, signals[i]);
            }
        }
        catch (const std::exception& e)
        {
            errorf(*logger_, "[{}/{}] cannot read signal PV {}: {}", name_, pv, opts.signalPv, e.what());
            throw std::runtime_error(util::format_string("Reader {}: cannot read signal PV {} for table {}: {}",
                                                         name_, opts.signalPv, pv, e.what()));
        }

        debugf(*logger_, "[{}/{}] reading table PV to discover columns", name_, pv);
        try
        {
            const auto tblValue = pvaContext().get(pv).exec()->wait(timeoutSec);
            const auto columns = tblValue["value"];
            if (!columns.valid() || columns.type().kind() != pvxs::Kind::Compound)
            {
                throw std::runtime_error("no value struct");
            }
            for (const auto& col : columns.ichildren())
            {
                auto field = columns.nameOf(col);
                if (field != opts.tsSecondsField && field != opts.tsNanosField)
                {
                    fields.push_back(std::move(field));
                }
            }
            infof(*logger_, "[{}/{}] table has {} data columns (ts fields '{}','{}' skipped)", name_, pv, fields.size(),
                  opts.tsSecondsField, opts.tsNanosField);
        }
        catch (const std::exception& e)
        {
            errorf(*logger_, "[{}/{}] cannot read table PV: {}", name_, pv, e.what());
            throw std::runtime_error(util::format_string("Reader {}: cannot read table PV {}: {}", name_, pv, e.what()));
        }

        auto names = std::make_shared<const ColumnNameMap>(
            buildColumnNameMap(pv, opts.signalPv, fields, signals));
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            debugf(*logger_, "[{}/{}]   column '{}' -> '{}'", name_, pv, fields[i], signals[i]);
        }
        infof(*logger_, "[{}/{}] resolved {} column names from {}", name_, pv, fields.size(), opts.signalPv);

        auto logger = logger_;
        auto readerName = name_;
        setColumnFilter(pv, [names, logger, readerName, pv](std::string_view label) -> std::optional<std::string>
                        {
                            const auto it = names->find(std::string(label));
                            if (it != names->end())
                            {
                                return it->second;
                            }
                            debugf(*logger, "[{}/{}] column '{}' not in startup name cache; dropped", readerName, pv, label);
                            return std::nullopt;
                        });
    }
}
