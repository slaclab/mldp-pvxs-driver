//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/shared/EpicsReaderConfig.h>

#include <config/ConfigSource.h>
#include <util/log/Logger.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <unordered_set>
#include <utility>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::reader::impl::epics;

namespace {
std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
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
        throw EpicsReaderConfig::Error(context + "." + key + " must be a scalar");
    }
}

std::optional<EpicsReaderConfig::PVConfig::NTTableRowTimestampOptions> parseNtTableRowTsOption(
    const std::string& pvName,
    const Config&      optionCfg)
{
    using namespace mldp_pvxs_driver::reader::impl::epics;

    const std::string optionContext = "pvs[].option (pv '" + pvName + "')";

    if (!optionCfg.valid())
    {
        return std::nullopt;
    }

    // Only interpret the subtree when it advertises a supported type.
    if (!optionCfg.hasChild(OptionTypeKey))
    {
        return std::nullopt;
    }

    requireScalarChild(optionCfg, OptionTypeKey, optionContext);
    const auto type = optionCfg.get(OptionTypeKey);
    if (toLower(type) != "slac-bsas-table")
    {
        return std::nullopt;
    }

    EpicsReaderConfig::PVConfig::NTTableRowTimestampOptions opts;

    requireScalarChild(optionCfg, TsSecondsKey, optionContext);
    requireScalarChild(optionCfg, TsNanosKey, optionContext);
    opts.tsSecondsField = optionCfg.get(TsSecondsKey, opts.tsSecondsField);
    opts.tsNanosField = optionCfg.get(TsNanosKey, opts.tsNanosField);
    if (opts.tsSecondsField.empty())
    {
        throw EpicsReaderConfig::Error(optionContext + "." + std::string(TsSecondsKey) + " must not be empty");
    }
    if (opts.tsNanosField.empty())
    {
        throw EpicsReaderConfig::Error(optionContext + "." + std::string(TsNanosKey) + " must not be empty");
    }

    if (optionCfg.hasChild(SourceNameKey))
    {
        throw EpicsReaderConfig::Error(optionContext + "." + std::string(SourceNameKey) + " is not supported for type 'slac-bsas-table'; source name always equals the column field name");
    }

    if (optionCfg.hasChild(ColumnBatchSizeOptionKey))
    {
        opts.columnBatchSize = static_cast<std::size_t>(optionCfg.getInt(ColumnBatchSizeOptionKey, 1));
    }

    return opts;
}

/// Build a PVConfig from a name plus optional `option`/`metadata` nodes.
/// Shared by inline `pvs[]` entries and `pvs-files[]` entries.
EpicsReaderConfig::PVConfig buildPvConfig(std::string                  pvName,
                                          const std::optional<Config>& optionNode,
                                          const std::optional<Config>& metadataNode,
                                          const std::string&           context)
{
    std::string                                                      option;
    std::optional<Config>                                            optionConfig;
    std::optional<EpicsReaderConfig::PVConfig::NTTableRowTimestampOptions> nttableRowTs;
    if (optionNode)
    {
        const auto raw = optionNode->raw();
        if (raw.is_map() || raw.is_seq())
        {
            optionConfig = *optionNode;
            nttableRowTs = parseNtTableRowTsOption(pvName, *optionConfig);
        }
        else if (raw.has_val())
        {
            *optionNode >> option;
        }
        else
        {
            throw EpicsReaderConfig::Error(context + ".option must be a scalar or map");
        }
    }

    std::unordered_map<std::string, std::string> pvMetadata;
    if (metadataNode)
    {
        std::map<std::string, std::string> m;
        *metadataNode >> m;
        pvMetadata.insert(m.begin(), m.end());
    }
    return {std::move(pvName), std::move(option), std::move(optionConfig), nttableRowTs, std::move(pvMetadata)};
}

/// Fetch the first child node for @p key, if present.
std::optional<Config> optionalChild(const Config& node, const char* key, const std::string& context)
{
    if (!node.hasChild(key))
    {
        return std::nullopt;
    }
    const auto nodes = node.subConfig(key);
    if (nodes.empty())
    {
        throw EpicsReaderConfig::Error(makeMissingFieldMessage(context + "." + key));
    }
    return nodes.front();
}

std::string trim(const std::string& value)
{
    const auto isSpace = [](unsigned char c)
    {
        return std::isspace(c) != 0;
    };
    auto begin = std::find_if_not(value.begin(), value.end(), isSpace);
    auto end = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    return begin < end ? std::string(begin, end) : std::string{};
}

/// Read a PV list file: one PV per line, blank lines and `#` comment lines ignored.
std::vector<std::string> readPvListFile(const std::filesystem::path& file)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
    {
        throw EpicsReaderConfig::Error("PV list file '" + file.string() + "' does not exist or is not a regular file");
    }
    std::ifstream in(file);
    if (!in)
    {
        throw EpicsReaderConfig::Error("Could not open PV list file '" + file.string() + "'");
    }

    std::vector<std::string> names;
    std::string              line;
    while (std::getline(in, line))
    {
        auto name = trim(line);
        if (name.empty() || name.front() == '#')
        {
            continue;
        }
        names.push_back(std::move(name));
    }
    if (in.bad())
    {
        throw EpicsReaderConfig::Error("Error reading PV list file '" + file.string() + "'");
    }
    return names;
}
} // namespace

EpicsReaderConfig::EpicsReaderConfig() = default;

EpicsReaderConfig::EpicsReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry)
    : EpicsReaderConfig(readerEntry, ::mldp_pvxs_driver::config::configBaseDir())
{
}

EpicsReaderConfig::EpicsReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry,
                                     const std::filesystem::path&               baseDir)
{
    if (!readerEntry.valid())
    {
        throw Error("Reader entry is invalid");
    }

    parse(readerEntry, baseDir);
}

bool EpicsReaderConfig::valid() const
{
    return valid_;
}

const std::string& EpicsReaderConfig::name() const
{
    return name_;
}

unsigned int EpicsReaderConfig::threadPoolSize() const
{
    return thread_pool_size_;
}

std::size_t EpicsReaderConfig::columnBatchSize() const
{
    return column_batch_size_;
}

const std::vector<EpicsReaderConfig::PVConfig>& EpicsReaderConfig::pvs() const
{
    return pvs_;
}

const std::vector<std::string>& EpicsReaderConfig::pvNames() const
{
    return pvNames_;
}

const std::vector<std::filesystem::path>& EpicsReaderConfig::pvFiles() const
{
    return pvFiles_;
}

void EpicsReaderConfig::parse(const Config& readerEntry, const std::filesystem::path& baseDir)
{
    using namespace mldp_pvxs_driver::reader::impl::epics;

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

    thread_pool_size_ = static_cast<unsigned int>(readerEntry.getInt(ThreadPoolKey, 1));
    column_batch_size_ = static_cast<std::size_t>(readerEntry.getInt(ColumnBatchSizeKey, 50));

    if (readerEntry.hasChild(BackendKey))
    {
        throw Error("backend is not supported; choose epics-pvxs or epics-base reader type");
    }

    // Parse optional reader-level static metadata
    if (readerEntry.hasChild(kMetadataKey))
    {
        std::map<std::string, std::string> m;
        readerEntry.subConfig(kMetadataKey).front() >> m;
        static_metadata_.insert(m.begin(), m.end());
    }

    const bool hasPvs = readerEntry.hasChild(PvsKey);
    const bool hasPvsFiles = readerEntry.hasChild(PvsFilesKey);
    if (!hasPvs && !hasPvsFiles)
    {
        return;
    }

    pvs_.clear();
    pvNames_.clear();
    pvFiles_.clear();
    std::unordered_set<std::string> seen;

    const auto addPv = [&](PVConfig pv, const std::string& source)
    {
        if (!seen.insert(pv.name).second)
        {
            throw Error("reader '" + name_ + "': duplicate PV '" + pv.name + "' (" + source + ")");
        }
        pvNames_.push_back(pv.name);
        pvs_.push_back(std::move(pv));
    };

    if (hasPvs)
    {
        if (!readerEntry.isSequence(PvsKey))
        {
            throw Error("pvs must be a sequence");
        }

        const auto pvNodes = readerEntry.subConfig(PvsKey);
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
                throw Error(makeMissingFieldMessage(std::string(PvsKey) + "[].name"));
            }

            const auto pvNameNodes = pvNode.subConfig(PvNameKey);
            if (pvNameNodes.empty())
            {
                throw Error(makeMissingFieldMessage(std::string(PvsKey) + "[].name"));
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

            const std::string context = std::string(PvsKey) + "[]";
            addPv(buildPvConfig(std::move(pvName),
                                optionalChild(pvNode, PvOptionKey, context),
                                optionalChild(pvNode, kMetadataKey, context),
                                context),
                  "inline pvs");
        }
    }

    if (hasPvsFiles)
    {
        if (!readerEntry.isSequence(PvsFilesKey))
        {
            throw Error("reader '" + name_ + "': pvs-files must be a sequence");
        }

        const auto fileNodes = readerEntry.subConfig(PvsFilesKey);
        for (std::size_t idx = 0; idx < fileNodes.size(); ++idx)
        {
            const auto&       fileNode = fileNodes[idx];
            const std::string context = "reader '" + name_ + "': pvs-files[" + std::to_string(idx) + "]";
            if (!fileNode.raw().is_map())
            {
                throw Error(context + " must be a map");
            }
            if (!fileNode.hasChild(PvsFilePathKey))
            {
                throw Error(context + " is missing required field 'path'");
            }
            const auto pathNodes = fileNode.subConfig(PvsFilePathKey);
            if (pathNodes.empty() || !pathNodes.front().raw().has_val())
            {
                throw Error(context + ".path must be a scalar");
            }
            std::string pathStr;
            pathNodes.front() >> pathStr;
            pathStr = trim(pathStr);
            if (pathStr.empty())
            {
                throw Error(context + ".path must not be empty");
            }

            std::filesystem::path filePath{pathStr};
            if (filePath.is_relative() && !baseDir.empty())
            {
                filePath = baseDir / filePath;
            }
            filePath = filePath.lexically_normal();

            const auto optionNode = optionalChild(fileNode, PvOptionKey, context);
            const auto metadataNode = optionalChild(fileNode, kMetadataKey, context);

            auto names = readPvListFile(filePath);
            if (names.empty())
            {
                auto logger = ::mldp_pvxs_driver::util::log::newLogger("epics_reader_config:" + name_);
                ::mldp_pvxs_driver::util::log::warnf(*logger, "PV list file '{}' is empty", filePath.string());
            }
            for (auto& pvName : names)
            {
                addPv(buildPvConfig(std::move(pvName), optionNode, metadataNode, context),
                      "file '" + filePath.string() + "'");
            }
            pvFiles_.push_back(std::move(filePath));
        }
    }

    valid_ = true;
}
