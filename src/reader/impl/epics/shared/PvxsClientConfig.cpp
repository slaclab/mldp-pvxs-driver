//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/shared/PvxsClientConfig.h>

#include <exception>
#include <string_view>

namespace mldp_pvxs_driver::reader::impl::epics {

namespace {

    constexpr std::string_view kEnvironmentKey{"environment"};
    constexpr std::string_view kPvxsEnvironmentPrefix{"EPICS_PVA_"};

    std::string prefix(const std::string& readerType)
    {
        return readerType + " reader: 'environment'";
    }

    pvxs::client::Config::defs_t overrides(const config::Config& readerEntry, const std::string& readerType)
    {
        if (!readerEntry.hasChild(std::string(kEnvironmentKey)))
            return {};

        const auto environment = readerEntry.raw()[kEnvironmentKey.data()];
        if (!environment.is_map())
            throw PvxsClientConfig::Error(prefix(readerType) + " must be a map");

        pvxs::client::Config::defs_t values;
        for (const auto child : environment.children())
        {
            if (!child.has_key() || !child.has_val())
                throw PvxsClientConfig::Error(prefix(readerType) + " values must be strings");

            std::string key{child.key().str, child.key().len};
            if (!key.starts_with(kPvxsEnvironmentPrefix))
                throw PvxsClientConfig::Error(prefix(readerType) + " keys must start with 'EPICS_PVA_': '" + key + "'");

            std::string value;
            child >> value;
            values.emplace(std::move(key), std::move(value));
        }
        return values;
    }

} // namespace

void PvxsClientConfig::validate(const config::Config& readerEntry, const std::string& readerType)
{
    static_cast<void>(overrides(readerEntry, readerType));
}

pvxs::client::Config PvxsClientConfig::buildConfig(const config::Config& readerEntry, const std::string& readerType)
{
    try
    {
        auto config = pvxs::client::Config::fromEnv();
        config.applyDefs(overrides(readerEntry, readerType));
        return config;
    }
    catch (const Error&)
    {
        throw;
    }
    catch (const std::exception& error)
    {
        throw Error(prefix(readerType) + " is invalid: " + error.what());
    }
}

pvxs::client::Context PvxsClientConfig::buildContext(const config::Config& readerEntry, const std::string& readerType)
{
    return buildConfig(readerEntry, readerType).build();
}

} // namespace mldp_pvxs_driver::reader::impl::epics
