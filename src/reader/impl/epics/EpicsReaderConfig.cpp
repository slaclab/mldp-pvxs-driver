#include <reader/impl/epics/EpicsReaderConfig.h>

#include <sstream>

using mldp_pvxs_driver::config::Config;
using mldp_pvxs_driver::config::EpicsReaderConfig;

namespace {
std::string makeMissingFieldMessage(const std::string& field)
{
    std::ostringstream oss;
    oss << "Missing required field '" << field << "' in epics reader config";
    return oss.str();
}
} // namespace

EpicsReaderConfig::EpicsReaderConfig(const Config& readerEntry)
{
    if (!readerEntry.valid())
    {
        throw Error("Reader entry is invalid");
    }

    parse(readerEntry);
}

void EpicsReaderConfig::parse(const Config& readerEntry)
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

    if (!readerEntry.hasChild("pv_names"))
    {
        throw Error(makeMissingFieldMessage("pv_names"));
    }

    if (!readerEntry.isSequence("pv_names"))
    {
        throw Error("pv_names must be a sequence");
    }

    pvNames_ = readStringSequence(readerEntry.raw()["pv_names"]);
    if (pvNames_.empty())
    {
        throw Error("pv_names must not be empty");
    }

    valid_ = true;
}

std::vector<std::string> EpicsReaderConfig::readStringSequence(const c4::yml::ConstNodeRef& node)
{
    std::vector<std::string> values;
    if (!node.is_seq())
    {
        return values;
    }

    values.reserve(node.num_children());
    for (const auto& child : node.children())
    {
        std::string value;
        if (child.has_val())
        {
            child >> value;
        }
        values.emplace_back(std::move(value));
    }

    return values;
}
