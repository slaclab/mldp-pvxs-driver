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
#include <reader/IReader.h>
#include <util/bus/IDataBus.h>
#include <util/factory/Factory.h>

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
 *
 * Inherits registry and registration machinery from Factory<>; this class only
 * provides the public create() overload whose argument order differs from the
 * internal CreatorFn signature.
 */
class ReaderFactory
    : public util::factory::Factory<
          ReaderFactory,
          Reader,
          std::shared_ptr<util::bus::IDataBus>,
          std::shared_ptr<mldp_pvxs_driver::metrics::Metrics>,
          const mldp_pvxs_driver::config::Config&>
{
public:
    /** @brief Discriminator used by the base Factory template. */
    static constexpr std::string_view kTypeName = "reader";

    /**
     * @brief Construct a reader of \p type connected to \p bus and configured via \p cfg.
     *
     * The public API exposes (type, bus, cfg, metrics) for historical compatibility,
     * while the internal CreatorFn receives (bus, metrics, cfg).  This wrapper
     * reorders the arguments before forwarding to the base.
     *
     * @throws std::out_of_range if the requested type has not been registered.
     */
    static std::unique_ptr<Reader> create(
        const std::string&                                  type,
        std::shared_ptr<util::bus::IDataBus>                bus,
        const ::mldp_pvxs_driver::config::Config&           cfg,
        std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics = nullptr);
};

/**
 * @brief Helper that registers a Reader implementation with ReaderFactory during static init.
 *
 * Wraps the base FactoryRegistrator pattern and hard-codes the constructor
 * argument order expected by all Reader subclasses: (bus, metrics, cfg).
 */
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
