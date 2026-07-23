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
#include <query/QueryFormatter.h>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::cli {

struct QueryCliOptions {
    std::string       sql{};
    std::string       sql_file{};
    QueryOutputFormat format{QueryOutputFormat::Table};
    bool              no_stats{false};
    uint64_t    memory_mb{256};
    std::string spill_dir{};
    uint32_t    spill_partitions{16};
    uint32_t    join_batch_size{100};
};

class QuerySubcommandPreparer
{
public:
    void prepare(const config::Config& config) const;
};

class QueryRunner
{
public:
    int run(const QueryCliOptions& options, std::ostream& output) const;
};

class QuerySubcommand
{
public:
    int run(int argc,
            char** argv,
            const std::vector<std::string>& global_config_sources,
            std::ostream&                    output,
            std::ostream&                    error) const;
};

} // namespace mldp_pvxs_driver::cli
