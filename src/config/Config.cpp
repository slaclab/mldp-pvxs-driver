//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "rapidyaml-0.10.0.hpp"
#include <cctype>
#include <cerrno>
#include <config/Config.h>
#include <cstdlib>
#include <string_view>
#include <vector>
#include <fstream>
#include <sstream>

using namespace mldp_pvxs_driver::config;

namespace {
bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }

    for (size_t i = 0; i < lhs.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
        {
            return false;
        }
    }
    return true;
}

bool tryParseBool(const std::string& input, bool& value)
{
    if (equalsIgnoreCase(input, "true"))
    {
        value = true;
        return true;
    }
    if (equalsIgnoreCase(input, "false"))
    {
        value = false;
        return true;
    }
    return false;
}

bool tryParseInteger(const std::string& input, long long& value)
{
    if (input.empty())
    {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoll(input.c_str(), &end, 0);
    if (errno != 0 || end != input.c_str() + input.size())
    {
        return false;
    }

    value = parsed;
    return true;
}

bool tryParseDouble(const std::string& input, double& value)
{
    if (input.empty())
    {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtod(input.c_str(), &end);
    if (errno != 0 || end != input.c_str() + input.size())
    {
        return false;
    }

    value = parsed;
    return true;
}
} // namespace

Config Config::configFromFile(const std::string &filename)
{
    std::ostringstream contents;
    {
        std::ifstream file{filename};
        if (file.fail())
        {
            throw std::runtime_error{"Could not open file"};
        }
        file >> contents.rdbuf();
        if (file.fail() && !file.eof())
        {
            throw std::runtime_error{"Could not read file"};
        }
    }
    const std::string& yaml = contents.str();
    return Config{std::make_shared<ryml::Tree>(ryml::parse_in_arena(c4::to_csubstr(yaml)))};
}

Config::Config(ConfigTreePtr tree)
    : tree_(tree)
    , node_(tree_->rootref()) {}

Config::Config(ConfigTreePtr tree, ryml::ConstNodeRef node)
    : tree_(tree)
    , node_(node) {}

c4::yml::ConstNodeRef Config::raw() const
{
    return node_;
}

bool Config::valid() const
{
    return tree_ && !node_.invalid();
}

bool Config::isSequence(const std::string& key) const
{
    return valid() && node_.has_child(key.c_str()) && node_[key.c_str()].is_seq();
}

bool Config::hasChild(const std::string& key) const
{
    return valid() && node_.has_child(key.c_str());
}

std::string Config::get(const std::string& key,
                        const std::string& def) const
{
    if (!valid() || !node_.has_child(key.c_str()))
        return def;

    ryml::ConstNodeRef child = node_[key.c_str()];
    if (!child.has_val())
        return def;

    std::string out;
    child >> out;
    return out;
}

std::vector<Config> Config::subConfig(const std::string& key) const
{
    std::vector<Config> children;
    if (!valid() || !node_.has_child(key.c_str()))
    {
        return children;
    }

    auto child = node_[key.c_str()];
    if (child.is_seq())
    {
        children.reserve(child.num_children());
        for (const auto element : child.children())
        {
            children.emplace_back(Config{tree_, element});
        }
    }
    else
    {
        children.emplace_back(Config{tree_, child});
    }

    return children;
}

int Config::getInt(const std::string& key, int def) const
{
    if (!valid() || !node_.has_child(key.c_str()))
        return def;

    ryml::ConstNodeRef child = node_[key.c_str()];
    if (!child.has_val())
        return def;

    int v{};
    child >> v;
    return v;
}

double Config::getDouble(const std::string& key, double def) const
{
    if (!valid() || !node_.has_child(key.c_str()))
        return def;

    ryml::ConstNodeRef child = node_[key.c_str()];
    if (!child.has_val())
        return def;

    double v{};
    child >> v;
    return v;
}

bool Config::getBool(const std::string& key, bool def) const
{
    if (!valid() || !node_.has_child(key.c_str()))
        return def;

    ryml::ConstNodeRef child = node_[key.c_str()];
    if (!child.has_val())
        return def;

    bool v{};
    child >> v;
    return v;
}

const Config& Config::operator>>(std::string& out) const
{
    if (!valid() || !node_.has_val())
    {
        out.clear();
        return *this;
    }

    node_ >> out;
    return *this;
}

const Config& Config::operator>>(int& out) const
{
    if (!valid() || !node_.has_val())
    {
        out = {};
        return *this;
    }

    node_ >> out;
    return *this;
}

const Config& Config::operator>>(double& out) const
{
    if (!valid() || !node_.has_val())
    {
        out = {};
        return *this;
    }

    node_ >> out;
    return *this;
}

const Config& Config::operator>>(bool& out) const
{
    if (!valid() || !node_.has_val())
    {
        out = {};
        return *this;
    }

    node_ >> out;
    return *this;
}

const Config& Config::operator>>(std::map<std::string, std::string>& out) const
{
    out.clear();

    if (!valid() || !node_.is_map())
    {
        return *this;
    }

    for (const auto child : node_.children())
    {
        if (!child.has_key() || !child.has_val())
        {
            continue;
        }

        const auto  keyView = child.key();
        std::string key{keyView.str, keyView.len};

        std::string value;
        child >> value;

        out.emplace(std::move(key), std::move(value));
    }

    return *this;
}

const Config& Config::operator>>(std::map<std::string, std::any>& out) const
{
    out.clear();

    if (!valid() || !node_.is_map())
    {
        return *this;
    }

    for (const auto child : node_.children())
    {
        if (!child.has_key())
        {
            continue;
        }

        const auto  keyView = child.key();
        std::string key{keyView.str, keyView.len};

        if (child.is_map())
        {
            std::map<std::string, std::any> nested;
            Config{tree_, child} >> nested;
            out.emplace(std::move(key), std::move(nested));
            continue;
        }

        if (child.is_seq())
        {
            std::vector<std::string> values;
            for (const auto element : child.children())
            {
                std::string entry;
                if (element.has_val())
                {
                    element >> entry;
                }
                values.emplace_back(std::move(entry));
            }
            out.emplace(std::move(key), std::move(values));
            continue;
        }

        if (!child.has_val())
        {
            out.emplace(std::move(key), std::any{});
            continue;
        }

        std::string scalar;
        child >> scalar;

        bool boolValue = false;
        if (tryParseBool(scalar, boolValue))
        {
            out.emplace(std::move(key), boolValue);
            continue;
        }

        long long integerValue = 0;
        if (tryParseInteger(scalar, integerValue))
        {
            out.emplace(std::move(key), integerValue);
            continue;
        }

        double doubleValue = 0.0;
        if (tryParseDouble(scalar, doubleValue))
        {
            out.emplace(std::move(key), doubleValue);
            continue;
        }

        out.emplace(std::move(key), std::move(scalar));
    }

    return *this;
}