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
 * @file AlignmentPolicy.h
 * @brief Supported strategies for aligning source updates into snapshots.
 */

#pragma once

namespace mldp_pvxs_driver::processor {

/**
 * @enum AlignmentPolicy
 * @brief Defines how buffered source data is combined before computation.
 */
enum class AlignmentPolicy
{
    LatestValue, ///< Use the latest batch available from each source.
    AllUpdated,  ///< Wait until every source has produced a fresh update.
    Interpolate, ///< Interpolate source data onto a shared reference time.
};

} // namespace mldp_pvxs_driver::processor
