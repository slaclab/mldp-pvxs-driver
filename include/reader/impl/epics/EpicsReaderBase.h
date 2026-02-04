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

#include <reader/Reader.h>
#include <reader/impl/epics/EpicsReaderConfig.h>
#include <util/bus/IEventBusPush.h>
#include <util/log/Logger.h>

#include <BS_thread_pool.hpp>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace mldp_pvxs_driver::reader::impl::epics {

using PVSet = std::set<std::string>;

class EpicsReaderBase : public reader::Reader
{
public:
    EpicsReaderBase(std::shared_ptr<util::bus::IEventBusPush> bus,
                    std::shared_ptr<metrics::Metrics>         metrics,
                    const EpicsReaderConfig&                  cfg,
                    std::shared_ptr<util::log::ILogger>       logger);
    ~EpicsReaderBase() override;

    std::string name() const override { return name_; }

protected:
    struct PVRuntimeConfig
    {
        enum class Mode
        {
            Default,
            NtTableRowTs,
        };

        Mode        mode = Mode::Default;
        std::string tsSecondsField;
        std::string tsNanosField;
    };

    const PVRuntimeConfig* runtimeConfigFor(const std::string& pvName) const;
    const PVSet&           pvNames() const { return pvNames_; }

    std::shared_ptr<util::log::ILogger> logger_;
    EpicsReaderConfig                   config_;
    std::string                         name_;
    std::atomic<bool>                   running_{true};
    std::shared_ptr<BS::light_thread_pool> reader_pool_;
    std::unordered_map<std::string, PVRuntimeConfig> pvRuntimeByName_;
    PVSet                                           pvNames_;
};

} // namespace mldp_pvxs_driver::reader::impl::epics
