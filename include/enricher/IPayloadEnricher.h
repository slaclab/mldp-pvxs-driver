//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#pragma once

#include <config/Config.h>
#include <util/bus/IDataBus.h>

#include <memory>
#include <mutex>
#include <string>

namespace mldp_pvxs_driver::enricher {

class IPayloadEnricher
{
public:
    virtual ~IPayloadEnricher() = default;
    virtual void configure(const config::Config& config) = 0;
    virtual bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept = 0;
    virtual std::string enricherType() const = 0;

    /// Serializes calls to one shared enricher without coupling unrelated enrichers.
    bool run(util::bus::IDataBus::EventBatch& batch) noexcept
    {
        std::lock_guard lock(mutex_);
        return enrich(batch);
    }
private:
    std::mutex mutex_;
};

using IPayloadEnricherPtr = std::shared_ptr<IPayloadEnricher>;

} // namespace mldp_pvxs_driver::enricher
