//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// ReaderFactory.hpp
#pragma once

#include <config/Config.h>
#include <reader/Reader.h>
#include <util/bus/IDataBus.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration to resolve circular dependency
namespace mldp_pvxs_driver::config {
class Config;
}

namespace mldp_pvxs_driver::metrics {
class Metrics;
}

namespace mldp_pvxs_driver::reader {

/**
 * @brief Central factory that tracks available reader implementations and builds them on demand.
 */
class ReaderFactory
{
public:
    /** @brief Type used to create reader instances for a configured event bus. */
    using CreatorFn = std::function<std::unique_ptr<Reader>(
        std::shared_ptr<util::bus::IDataBus>,
        std::shared_ptr<mldp_pvxs_driver::metrics::Metrics>,
        const mldp_pvxs_driver::config::Config&)>;

    /**
     * @brief Register a builder for the reader type identified by \p type.
     * @param type Identifier used when calling @c create.
     * @param fn Factory callback that constructs the reader.
     */
    static void registerType(const std::string& type, CreatorFn fn);

    /**
     * @brief Construct a reader of \p type that is connected to \p bus and configured via \p cfg.
     * @throws std::out_of_range if the requested type has not been registered.
     */
    static std::unique_ptr<Reader> create(
        const std::string&                                  type,
        std::shared_ptr<util::bus::IDataBus>                bus,
        const ::mldp_pvxs_driver::config::Config&           cfg,
        std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics = nullptr);

    /**
     * @brief Get list of all registered reader types.
     * @return Vector of type identifiers.
     */
    static std::vector<std::string> registeredTypes();

private:
    /**
     * @brief Mutable registry that holds all registered creator functions and their identifiers.
     * @return Reference to the singleton registry map.
     */
    static std::unordered_map<std::string, CreatorFn>& registry();
};

// Helper that registers Reader implementations with the factory during static initialization.
template <typename ReaderT>
class ReaderRegistrator
{
public:
    explicit ReaderRegistrator(const char* typeName)
    {
        ReaderFactory::registerType(
            typeName,
            [](std::shared_ptr<util::bus::IDataBus>                bus,
               std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics,
               const mldp_pvxs_driver::config::Config&             cfg)
            {
                return std::make_unique<ReaderT>(std::move(bus), std::move(metrics), cfg);
            });
    }
};

/// Macro that binds \c CLASSNAME into the factory with the provided \c TYPE_STRING identifier.
#define REGISTER_READER(TYPE_STRING, CLASSNAME) \
    static inline ReaderRegistrator<CLASSNAME> registrator_{TYPE_STRING};

} // namespace mldp_pvxs_driver::reader
