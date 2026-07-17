//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <processor/ChannelProcessorFactory.h>

#include <BS_thread_pool.hpp>
#include <stdexcept>
#include <unordered_map>

namespace mldp_pvxs_driver::processor {

namespace {

    using Registry = std::unordered_map<std::string, ChannelProcessorFactory::ProcessorFactory>;

    Registry& registry()
    {
        static Registry instance;
        return instance;
    }

} // namespace

std::vector<IChannelProcessorUPtr> ChannelProcessorFactory::create(
    const std::string&                     type,
    const config::Config&                  cfg,
    std::shared_ptr<util::bus::IDataBus>   bus,
    std::shared_ptr<metrics::Metrics>      metrics,
    std::shared_ptr<BS::light_thread_pool> thread_pool)
{
    auto& factory = lookup(type);
    return factory(cfg, std::move(bus), std::move(metrics), std::move(thread_pool));
}

bool ChannelProcessorFactory::registerType(const std::string& type, ProcessorFactory factory)
{
    registry()[type] = std::move(factory);
    return true;
}

bool ChannelProcessorFactory::isRegistered(const std::string& type)
{
    return registry().contains(type);
}

ChannelProcessorFactory::ProcessorFactory& ChannelProcessorFactory::lookup(const std::string& type)
{
    auto& reg = registry();
    auto  it = reg.find(type);
    if (it == reg.end())
    {
        throw std::runtime_error("Unknown channel processor type: " + type);
    }

    return it->second;
}

} // namespace mldp_pvxs_driver::processor
