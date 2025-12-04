#pragma once

#include <ingestion.grpc.pb.h>
#include <pvxs/client.h>
#include <pvxs/nt.h>

namespace mldp_pvxs_driver::reader::impl::epics {
class EpicsMLDPConversion
{
public:
        static void convertPVToProtoValue(const pvxs::Value& pvValue, DataValue* protoValue);
};
} // namespace mldp_pvxs_driver::reader::impl::epics