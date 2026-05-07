//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/hdf5/HDF5Writer.h>
#include <writer/hdf5/HDF5WriterMerge.h>
#include <writer/hdf5/HDF5WriterPerSource.h>

using namespace mldp_pvxs_driver::writer;

HDF5Writer::HDF5Writer(const config::Config&             node,
                       std::shared_ptr<metrics::Metrics> metrics)
{
    auto cfg = HDF5WriterConfig::parse(node);
    if (cfg.mergeRootSources)
        impl_ = std::make_unique<HDF5WriterMerge>(std::move(cfg), std::move(metrics));
    else
        impl_ = std::make_unique<HDF5WriterPerSource>(std::move(cfg), std::move(metrics));
}

HDF5Writer::HDF5Writer(HDF5WriterConfig config)
{
    if (config.mergeRootSources)
        impl_ = std::make_unique<HDF5WriterMerge>(std::move(config));
    else
        impl_ = std::make_unique<HDF5WriterPerSource>(std::move(config));
}

std::string HDF5Writer::name() const              { return impl_->name(); }
void        HDF5Writer::start()                   { impl_->start(); }
bool        HDF5Writer::push(util::bus::IDataBus::EventBatch b) noexcept { return impl_->push(std::move(b)); }
void        HDF5Writer::stop() noexcept           { impl_->stop(); }
