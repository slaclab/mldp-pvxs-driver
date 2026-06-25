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

#include <cstddef>
#include <string>

namespace mldp_pvxs_driver::test::mock {

/**
 * @brief Generates BSAS Gen1 HDF5 files in flat format.
 *
 * Produces files structurally identical to real BSAS Gen1 exports:
 *   /SIG_0000         — float64 dataset shape (N,1) with @MATLAB_class and @label
 *   /FLAG_00          — int16 dataset shape (N,1) with @MATLAB_class and @label
 *   /secondsPastEpoch — uint32 dataset shape (N,1)
 *   /nanoseconds      — uint32 dataset shape (N,1)
 */
class BsasGen1HDF5Mock
{
public:
    struct Params
    {
        std::size_t numFloatCols = 10;
        std::size_t numIntCols   = 4;
        std::size_t numRows      = 20;
        uint32_t    baseEpoch    = 1700000000u;
        std::string floatColPrefix = "SIG_";
        std::string intColPrefix = "FLAG_";
    };

    static void generate(const std::string& outputPath, const Params& params);
};

} // namespace mldp_pvxs_driver::test::mock
