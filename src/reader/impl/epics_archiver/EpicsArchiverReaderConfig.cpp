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

const std::vector<EpicsArchiverReaderConfig::PVConfig>& EpicsArchiverReaderConfig::pvs() const
{
    return pvs_;
}

const std::vector<std::string>& EpicsArchiverReaderConfig::pvNames() const
{
    return pvNames_;
}

void EpicsArchiverReaderConfig::parse(const Config& readerEntry)
{
    // Parse reader name
    if (!readerEntry.hasChild("name"))
    {
        throw Error(makeMissingFieldMessage("name"));
    }

    const auto nameNodes = readerEntry.subConfig("name");
    if (nameNodes.empty())
    {
        throw Error(makeMissingFieldMessage("name"));
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
    if (!readerEntry.hasChild("hostname"))
    {
        throw Error(makeMissingFieldMessage("hostname"));
    }

    requireScalarChild(readerEntry, "hostname", "archiver reader config");
    hostname_ = readerEntry.get("hostname");
    if (hostname_.empty())
    {
        throw Error("hostname must not be empty");
    }

    // Parse PVs
    if (!readerEntry.hasChild("pvs"))
    {
        throw Error(makeMissingFieldMessage("pvs"));
    }

    if (!readerEntry.isSequence("pvs"))
    {
        throw Error("pvs must be a sequence");
    }

    const auto pvNodes = readerEntry.subConfig("pvs");

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

        if (!pvNode.hasChild("name"))
        {
            throw Error(makeMissingFieldMessage("pvs[].name"));
        }

        const auto pvNameNodes = pvNode.subConfig("name");
        if (pvNameNodes.empty())
        {
            throw Error(makeMissingFieldMessage("pvs[].name"));
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
