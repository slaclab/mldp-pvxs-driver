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

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query {

struct QueryStats {
    std::chrono::milliseconds elapsed{0};
    uint64_t                  rows_from_backend{0};
    uint64_t                  rows_returned{0};
    uint64_t                  rpc_calls{0};
    uint64_t                  bytes_spilled{0};
    uint64_t                  spill_files{0};
    uint64_t                  materialized_bytes{0};
    uint64_t                  materialized_files{0};
    uint64_t                  peak_memory_bytes{0};
    std::string               plan_summary;
    std::vector<std::string>  plan_warnings;
};

} // namespace mldp_pvxs_driver::query
