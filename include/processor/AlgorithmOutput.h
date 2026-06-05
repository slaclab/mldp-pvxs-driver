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

#include <string>

#include <util/bus/IDataBus.h>

namespace mldp_pvxs_driver::processor {

// One virtual-PV emission from IAlgorithm::compute().
// output_source must be set as the identity field inside payload:
//   TimeSeriesPayload.root_source_name = output_source
//   SourceMetadataPayload.root_source_name = output_source
//   ConfigurationPayload.root_source_name = output_source
//   ConfigurationActivationPayload.configuration_name = output_source
struct AlgorithmOutput
{
    std::string             output_source;
    util::bus::BatchPayload payload;
};

} // namespace mldp_pvxs_driver::processor
