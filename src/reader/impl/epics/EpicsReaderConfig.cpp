#include <reader/impl/epics/EpicsReaderConfig.h>

#include <sstream>
#include <utility>

namespace {
std::string makeMissingFieldMessage(const std::string& field)
{
    std::ostringstream oss;
    oss << "Missing required field '" << field << "' in epics reader config";
    return oss.str();
}
} // namespace

using namespace mldp_pvxs_driver::reader::impl::epics;

EpicsReaderConfig::EpicsReaderConfig() = default;

EpicsReaderConfig::EpicsReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry)
{
    if (!readerEntry.valid())
    {
        throw Error("Reader entry is invalid");
    }

    parse(readerEntry);
}

bool EpicsReaderConfig::valid() const
{
    return valid_;
}

const std::string& EpicsReaderConfig::name() const
{
    return name_;
}

const std::vector<EpicsReaderConfig::PVConfig>& EpicsReaderConfig::pvs() const
{
    return pvs_;
}

const std::vector<std::string>& EpicsReaderConfig::pvNames() const
{
    return pvNames_;
}

void EpicsReaderConfig::parse(const ::mldp_pvxs_driver::config::Config& readerEntry)
{
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

    if (!readerEntry.hasChild("pvs"))
    {
        return;
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

        std::string option;
        if (pvNode.hasChild("option"))
        {
            const auto optionNodes = pvNode.subConfig("option");
            if (optionNodes.empty())
            {
                throw Error(makeMissingFieldMessage("pvs[].option"));
            }

            const auto& optionNode = optionNodes.front();
            if (!optionNode.raw().has_val())
            {
                throw Error("pvs[].option must be a scalar");
            }

            optionNode >> option;
        }

        pvs_.push_back({std::move(pvName), std::move(option)});
        pvNames_.push_back(pvs_.back().name);
    }

    valid_ = true;
}
