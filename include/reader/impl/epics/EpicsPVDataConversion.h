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

#include <ingestion.grpc.pb.h>
#include <pv/pvData.h>
#include <util/bus/IEventBusPush.h>
#include <util/log/Logger.h>

#include <functional>
#include <string>

namespace mldp_pvxs_driver::reader::impl::epics {

class EpicsPVDataConversion
{
public:
    using ColumnEmitFn = std::function<void(std::string colName,
                                            std::vector<mldp_pvxs_driver::util::bus::IEventBusPush::EventValue> events)>;

    static void convertPVToProtoValue(const ::epics::pvData::PVField& pvField, DataValue* protoValue);

    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&                    log,
                                          const std::string&                                      tablePvName,
                                          const ::epics::pvData::PVStructurePtr&                    epicsValue,
                                          const std::string&                                      tsSecondsField,
                                          const std::string&                                      tsNanosField,
                                          mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch* outBatch,
                                          size_t&                                                 outEmitted);

    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                          const std::string&                    tablePvName,
                                          const ::epics::pvData::PVStructurePtr&  epicsValue,
                                          const std::string&                    tsSecondsField,
                                          const std::string&                    tsNanosField,
                                          ColumnEmitFn                          emitColumn,
                                          size_t&                               outEmitted);
};

} // namespace mldp_pvxs_driver::reader::impl::epics
