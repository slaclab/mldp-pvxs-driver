//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <config/Config.h>

#include <cstdint>
#include <iosfwd>
#include <string>

namespace mldp_pvxs_driver::cli {

struct QueryCliOptions {
    uint64_t    memory_mb{256};
    std::string spill_dir{};
    uint32_t    spill_partitions{16};
    uint32_t    join_batch_size{100};
};

void prepareQuerySubcommand(const config::Config& config);
int runQueryRepl(std::istream& input, std::ostream& output, const QueryCliOptions& options = {});

} // namespace mldp_pvxs_driver::cli
