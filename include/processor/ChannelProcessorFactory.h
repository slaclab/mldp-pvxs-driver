//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file ChannelProcessorFactory.h
 * @brief Registry and helper macro for constructing channel processors by type.
 */

#pragma once

#include <BS_thread_pool.hpp>
#include <config/Config.h>
#include <metrics/Metrics.h>
#include <processor/ChannelProcessor.h>
#include <processor/IChannelProcessor.h>
#include <processor/MLDPChannelProcessorConfig.h>
#include <util/bus/IDataBus.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

class IAlgorithm;

/**
 * @class ChannelProcessorFactory
 * @brief Registry of processor builders keyed by configured algorithm type.
 * @details
 * Each registered type expands one processor config block into one or more
 * runtime channel processor instances.
 */
class ChannelProcessorFactory
{
public:
    using ProcessorFactory = std::function<
        std::vector<IChannelProcessorUPtr>(
            const config::Config&,
            std::shared_ptr<util::bus::IDataBus>,
            std::shared_ptr<metrics::Metrics>,
            std::shared_ptr<BS::light_thread_pool>)>;

    /**
     * @brief Construct processors for the requested type.
     * @throws std::runtime_error when the type is unknown.
     */
    static std::vector<IChannelProcessorUPtr> create(
        const std::string&                     type,
        const config::Config&                  cfg,
        std::shared_ptr<util::bus::IDataBus>   bus,
        std::shared_ptr<metrics::Metrics>      metrics = nullptr,
        std::shared_ptr<BS::light_thread_pool> thread_pool = nullptr);

    /**
     * @brief Register or replace the factory for one processor type.
     * @return Always true to support static-init registration idioms.
     */
    static bool registerType(const std::string& type, ProcessorFactory factory);

    /** @return Whether a processor factory is registered for @p type. */
    static bool isRegistered(const std::string& type);

private:
    static ProcessorFactory& lookup(const std::string& type);
};

} // namespace mldp_pvxs_driver::processor

#define REGISTER_ALGORITHM(TYPE_STRING, CLASSNAME)                                                    \
    static bool reg_##CLASSNAME =                                                                      \
        ::mldp_pvxs_driver::processor::ChannelProcessorFactory::registerType(                          \
            TYPE_STRING,                                                                               \
            [](const ::mldp_pvxs_driver::config::Config&                cfg,                           \
               std::shared_ptr<::mldp_pvxs_driver::util::bus::IDataBus> bus,                          \
               std::shared_ptr<::mldp_pvxs_driver::metrics::Metrics>    metrics,                      \
               std::shared_ptr<BS::light_thread_pool>                   thread_pool)                   \
                -> std::vector<::mldp_pvxs_driver::processor::IChannelProcessorUPtr>                  \
            {                                                                                          \
                auto algorithm = std::make_unique<CLASSNAME>();                                        \
                algorithm->configure(cfg);                                                             \
                std::vector<::mldp_pvxs_driver::processor::IChannelProcessorUPtr> processors;         \
                processors.push_back(                                                                  \
                    std::make_unique<::mldp_pvxs_driver::processor::ChannelProcessor>(                 \
                        ::mldp_pvxs_driver::processor::MLDPChannelProcessorConfig(cfg),                \
                        std::move(algorithm),                                                          \
                        std::move(bus),                                                                \
                        std::move(metrics),                                                            \
                        std::move(thread_pool)));                                                      \
                return processors;                                                                     \
            })
