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
 * @file TriggerPolicy.h
 * @brief Supported policies for deciding when to emit a snapshot.
 */

#pragma once

namespace mldp_pvxs_driver::processor {

/**
 * @enum TriggerPolicy
 * @brief Defines when buffered input is eligible to produce a snapshot.
 */
enum class TriggerPolicy
{
    AnyUpdate, ///< Emit whenever any required source has produced data.
    AllUpdated, ///< Emit only after every required source is freshly updated.
    Interval, ///< Emit on a periodic timer using the latest buffered values.
};

} // namespace mldp_pvxs_driver::processor
