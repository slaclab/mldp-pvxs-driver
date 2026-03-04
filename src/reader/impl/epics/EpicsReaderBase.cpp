//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/EpicsReaderBase.h>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;

EpicsReaderBase::EpicsReaderBase(std::shared_ptr<util::bus::IDataBus> bus,
                                 std::shared_ptr<metrics::Metrics>    metrics,
                                 const EpicsReaderConfig&             cfg,
                                 std::shared_ptr<util::log::ILogger>  logger)
    : Reader(std::move(bus), std::move(metrics))
    , logger_(std::move(logger))
    , config_(cfg)
    , name_(config_.name())
    , reader_pool_(std::make_shared<BS::light_thread_pool>(config_.threadPoolSize()))
{
    for (const auto& pvConfig : config_.pvs())
    {
        pvNames_.insert(pvConfig.name);
        PVRuntimeConfig runtime;
        if (pvConfig.nttableRowTs.has_value())
        {
            runtime.mode = PVRuntimeConfig::Mode::SlacBsasTable;
            runtime.tsSecondsField = pvConfig.nttableRowTs->tsSecondsField;
            runtime.tsNanosField = pvConfig.nttableRowTs->tsNanosField;
        }
        pvRuntimeByName_.emplace(pvConfig.name, std::move(runtime));
    }
}

EpicsReaderBase::~EpicsReaderBase()
{
    running_ = false;
    if (reader_pool_)
    {
        reader_pool_->wait();
    }
}

const EpicsReaderBase::PVRuntimeConfig* EpicsReaderBase::runtimeConfigFor(const std::string& pvName) const
{
    const auto it = pvRuntimeByName_.find(pvName);
    return (it != pvRuntimeByName_.end()) ? &it->second : nullptr;
}
