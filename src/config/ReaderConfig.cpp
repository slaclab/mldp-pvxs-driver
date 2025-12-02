#include <config/ReaderConfig.h>

#include <sstream>

using mldp_pvxs_driver::config::Config;
using mldp_pvxs_driver::config::ReaderConfig;

namespace {
std::string makeMissingFieldMessage(const std::string& field)
{
    std::ostringstream oss;
    oss << "Missing required field '" << field << "' in reader config";
    return oss.str();
}
}

ReaderConfig::ReaderConfig(const Config& root)
{
    if (!root.valid())
    {
        throw Error("Root config is invalid");
    }
    if (!root.hasChild("reader"))
    {
        throw Error(makeMissingFieldMessage("reader"));
    }
    parseReaderNode(root.subConfig("reader"));
}

void ReaderConfig::parseReaderNode(const Config& readerNode)
{
    if (!readerNode.valid())
    {
        throw Error("Reader node is invalid");
    }

    type_ = readerNode.get("type");
    if (type_.empty())
    {
        throw Error(makeMissingFieldMessage("reader.type"));
    }

    if (!readerNode.hasChild("options"))
    {
        throw Error(makeMissingFieldMessage("reader.options"));
    }

    const auto optionsNode = readerNode.subConfig("options");
    if (!optionsNode.hasChild("pv_names"))
    {
        throw Error(makeMissingFieldMessage("reader.options.pv_names"));
    }

    const auto pvNode = optionsNode.subConfig("pv_names");
    const auto rawPvNode = pvNode.raw();
    if (!rawPvNode.is_seq())
    {
        throw Error("reader.options.pv_names must be a sequence");
    }

    pvNames_ = readStringSequence(rawPvNode);
    if (pvNames_.empty())
    {
        throw Error("reader.options.pv_names must not be empty");
    }

    valid_ = true;
}

std::vector<std::string> ReaderConfig::readStringSequence(const c4::yml::ConstNodeRef& node)
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
