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
#include <query/QueryExecutor.h>
#include <query/QueryPlanner.h>
#include <query/parser/QueryParser.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>
#include <query/plan/PlannerError.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include <arrow/filesystem/localfs.h>
#include <arrow/memory_pool.h>

#include <filesystem>
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

int runQueryRepl(std::istream& input, std::ostream& output, const QueryCliOptions& options)
{
    const auto spill_dir = options.spill_dir.empty()
        ? (std::filesystem::temp_directory_path() / "mldp-query-spill").string()
        : options.spill_dir;
    auto spill_file_system = std::make_shared<arrow::fs::LocalFileSystem>();
    [[maybe_unused]] query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
        .spill = std::make_shared<query::SpillManager>(spill_file_system, spill_dir),
        .memory_limit_bytes = options.memory_mb * 1024ULL * 1024ULL,
        .spill_partitions = options.spill_partitions,
        .join_batch_size = options.join_batch_size,
        .spill_fs = std::move(spill_file_system),
        .spill_dir = spill_dir,
    };

    std::string line;
    const query::QueryPlanner planner;
    const query::QueryExecutor executor;
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
                auto parsed = query::parseQuery(line);
                auto physical = planner.plan(parsed);
                auto result = executor.execute(physical, context);
                output << "Returned " << result.stats.rows_returned << " row(s)\n";
            }
            catch (const query::ParseError& error)
            {
                output << "Parse error at " << error.line() << ":" << error.column() << " - " << error.what() << "\n";
            }
            catch (const query::plan::PlannerException& error)
            {
                output << query::plan::plannerErrorWhat(error.error()) << "\n";
            }
            catch (const std::exception& error)
            {
                output << "Execution error: " << error.what() << "\n";
            }
        }
    }
    return 0;
}

} // namespace mldp_pvxs_driver::cli
