//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file AlgorithmOutput.h
 * @brief Value type describing one algorithm-produced output payload.
 */

#pragma once

#include <string>

#include <util/bus/IDataBus.h>

namespace mldp_pvxs_driver::processor {

/**
 * @struct AlgorithmOutput
 * @brief One virtual-PV emission returned by an algorithm computation.
 * @details
 * The @ref output_source value must also be copied into the identity field of
 * the emitted payload variant so downstream bus consumers can associate the
 * payload with the generated virtual source.
 */
struct AlgorithmOutput
{
    std::string             output_source; ///< Generated source name for this output payload.
    util::bus::BatchPayload payload;       ///< Bus payload to publish for @ref output_source.
};

} // namespace mldp_pvxs_driver::processor
