#pragma once

#include <pvxs/client.h>
#include <reader/impl/epics/EpicsMLDPConversion.h>
#include <string>
#include <util/bus/IEventBusPush.h>

namespace mldp_pvxs_driver::reader::impl::epics {

class BSASEpicsMLDPConversion : public EpicsMLDPConversion
{
public:
    static bool tryBuildNtTableRowTsBatch(const std::string& tablePvName,
                                         const pvxs::Value& epicsValue,
                                         const std::string& tsSecondsField,
                                         const std::string& tsNanosField,
                                         mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch* outBatch,
                                         size_t* outEmitted);
};

} // namespace mldp_pvxs_driver::reader::impl::epics
