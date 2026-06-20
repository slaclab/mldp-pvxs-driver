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
 * @brief Generates BSAS Gen1 HDF5 files in PyTables "fixed" format.
 *
 * Produces files structurally identical to real BSAS Gen1 exports:
 *   /data/axis0         — all column names (string)
 *   /data/axis1         — row indices (int64)
 *   /data/block0_items  — float64 column names
 *   /data/block0_values — float64 data (rows x float64_cols)
 *   /data/block1_items  — int16 column names
 *   /data/block1_values — int16 data (rows x int16_cols)
 *   /data/block2_items  — ["secondsPastEpoch", "nanoseconds"]
 *   /data/block2_values — uint32 timestamps (rows x 2)
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
    };

    static void generate(const std::string& outputPath, const Params& params);
};

} // namespace mldp_pvxs_driver::test::mock
