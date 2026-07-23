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

#include <query/QuerySubcommand.h>

#include <query/ExecutionContext.h>
#include <query/parser/QueryParser.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include <arrow/filesystem/localfs.h>
#include <arrow/memory_pool.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::cli {

namespace {

void prepareQueryable(std::string_view type, const config::Config& cfg)
{
    using query::QueryableFactory;
    using query::impl::mldp::MLDPAnnotationQueryClient;
    using query::impl::mldp::MLDPQueryClient;

    if (type == "mldp")
    {
        QueryableFactory::instance().prepare<MLDPQueryClient>(cfg);
        return;
    }
    if (type == "mldp-pv-metadata")
    {
        QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(cfg);
        return;
    }
    throw std::runtime_error("Unknown queryable type: " + std::string(type));
}

} // namespace

void prepareQuerySubcommand(const config::Config& config)
{
    query::QueryableFactory::instance().reset();
    if (!config.hasChild("queryable"))
    {
        return;
    }

    if (config.isSequence("queryable"))
    {
        for (const auto& entry : config.subConfig("queryable"))
        {
            const auto type = entry.get("type", "");
            if (type.empty())
            {
                throw std::runtime_error("queryable entry missing 'type' field");
            }
            prepareQueryable(type, entry);
        }
        return;
    }

    for (const auto& [type, entry] : config.subConfig("queryable").front().namedSubConfig())
    {
        prepareQueryable(type, entry);
    }
}

int runQueryRepl(std::istream& input, std::ostream& output)
{
    auto spill_file_system = std::make_shared<arrow::fs::LocalFileSystem>();
    [[maybe_unused]] query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
        .spill = std::make_shared<query::SpillManager>(spill_file_system, ".mldp-query-spill"),
        .memory_limit_bytes = 256ULL * 1024ULL * 1024ULL,
        .join_batch_size = 8192,
        .spill_fs = std::move(spill_file_system),
        .spill_dir = ".mldp-query-spill",
    };

    std::string line;
    while (std::getline(input, line))
    {
        if (line == "quit" || line == "exit")
        {
            return 0;
        }
        if (!line.empty())
        {
            try
            {
                [[maybe_unused]] auto parsed = query::parseQuery(line);
                output << "Query execution is not implemented yet.\n";
            }
            catch (const query::ParseError& error)
            {
                output << "Parse error at " << error.line() << ":" << error.column() << " - " << error.what() << "\n";
            }
        }
    }
    return 0;
}

} // namespace mldp_pvxs_driver::cli
