//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/pvxs/EpicsPVXSBsasGen1Reader.h>

using namespace mldp_pvxs_driver::reader::impl::epics;

EpicsPVXSBsasGen1Reader::EpicsPVXSBsasGen1Reader(std::shared_ptr<util::bus::IDataBus> bus,
                                                 std::shared_ptr<metrics::Metrics>    metrics,
                                                 const config::Config&                cfg)
    : EpicsPVXSBsasTableReaderBase(std::move(bus), std::move(metrics), cfg,
                                   EpicsPVXSBsasTableReaderConfig::Generation::Gen1, kReaderType)
{
    addPV(pvNames());
}
