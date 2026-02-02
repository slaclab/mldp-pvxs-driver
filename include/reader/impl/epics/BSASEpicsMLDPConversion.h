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

#include <BS_thread_pool.hpp>
#include <functional>
#include <pvxs/client.h>
#include <reader/impl/epics/EpicsMLDPConversion.h>
#include <string>
#include <util/bus/IEventBusPush.h>

#include <util/log/Logger.h>

namespace mldp_pvxs_driver::reader::impl::epics {

class BSASEpicsMLDPConversion : public EpicsMLDPConversion
{
public:
    using ColumnEmitFn = std::function<void(std::string colName,
                                             std::vector<mldp_pvxs_driver::util::bus::IEventBusPush::EventValue> events)>;

    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&                    log,
                                          const std::string&                                      tablePvName,
                                          const pvxs::Value&                                      epicsValue,
                                          const std::string&                                      tsSecondsField,
                                          const std::string&                                      tsNanosField,
                                          mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch* outBatch,
                                          size_t&                                                 outEmitted);

    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                          const std::string&                    tablePvName,
                                          const pvxs::Value&                    epicsValue,
                                          const std::string&                    tsSecondsField,
                                          const std::string&                    tsNanosField,
                                          ColumnEmitFn                          emitColumn,
                                          size_t&                               outEmitted);

    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                          const std::string&                    tablePvName,
                                          const pvxs::Value&                    epicsValue,
                                          const std::string&                    tsSecondsField,
                                          const std::string&                    tsNanosField,
                                          ColumnEmitFn                          emitColumn,
                                          size_t&                               outEmitted,
                                          BS::light_thread_pool*                pool);
};

} // namespace mldp_pvxs_driver::reader::impl::epics
