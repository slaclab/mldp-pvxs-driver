//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/QuerySubcommand.h>

#include <config/ConfigSource.h>
#include <query/ExecutionContext.h>
#include <query/QueryExecutor.h>
#include <query/QueryFormatter.h>
#include <query/QueryPlanner.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <query/impl/mldp/MLDPQueryClient.h>
#include <query/parser/QueryParser.h>
#include <query/plan/PlannerError.h>

#include <arrow/filesystem/localfs.h>
#include <arrow/memory_pool.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

using namespace mldp_pvxs_driver::cli;

namespace {

void prepareQueryable(std::string_view type, const mldp_pvxs_driver::config::Config& cfg)
{
    using mldp_pvxs_driver::query::QueryableFactory;
    using mldp_pvxs_driver::query::impl::mldp::MLDPAnnotationQueryClient;
    using mldp_pvxs_driver::query::impl::mldp::MLDPQueryClient;

    if (type == "mldp")
    {
        QueryableFactory::instance().prepare<MLDPQueryClient>(cfg);
        return;
    }
    if (type == "mldp-annotation" || type == "mldp-pv-metadata")
    {
        QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(cfg);
        return;
    }
    throw std::runtime_error("Unknown queryable type: " + std::string(type));
}

std::string loadSql(const QueryCliOptions& options)
{
    if (!options.sql.empty() && !options.sql_file.empty())
    {
        throw std::runtime_error("Provide SQL either as positional text or --file, not both");
    }
    if (!options.sql.empty())
    {
        return options.sql;
    }
    if (options.sql_file.empty())
    {
        throw std::runtime_error("Missing SQL input: pass positional SQL text or --file");
    }

    std::ifstream input(options.sql_file);
    if (!input)
    {
        throw std::runtime_error("Cannot open SQL file: " + options.sql_file);
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

QueryOutputFormat parseFormat(std::string_view value)
{
    if (value == "table")
        return QueryOutputFormat::Table;
    if (value == "json")
        return QueryOutputFormat::Json;
    if (value == "csv")
        return QueryOutputFormat::Csv;
    if (value == "arrow")
        return QueryOutputFormat::Arrow;
    throw std::runtime_error("Invalid --format value '" + std::string(value) + "' (expected: table,json,csv,arrow)");
}

void parseQueryArguments(int argc, char** argv, QueryCliOptions& options)
{
    // argv[0] is "query"
    for (int index = 1; index < argc; ++index)
    {
        const auto arg = std::string_view{argv[index]};
        if (arg == "-c" || arg == "--config")
        {
            throw std::runtime_error("-c/--config is a global option; place it before 'query'");
        }
        if (arg == "--file")
        {
            if (++index >= argc)
            {
                throw std::runtime_error("--file requires a path");
            }
            options.sql_file = argv[index];
            continue;
        }
        if (arg == "--format")
        {
            if (++index >= argc)
            {
                throw std::runtime_error("--format requires one of: table,json,csv,arrow");
            }
            options.format = parseFormat(argv[index]);
            continue;
        }
        if (arg == "--no-stats")
        {
            options.no_stats = true;
            continue;
        }
        if (arg == "--memory-mb")
        {
            if (++index >= argc)
            {
                throw std::runtime_error("--memory-mb requires a numeric value");
            }
            options.memory_mb = static_cast<uint64_t>(std::stoull(argv[index]));
            continue;
        }
        if (arg == "--spill-dir")
        {
            if (++index >= argc)
            {
                throw std::runtime_error("--spill-dir requires a path");
            }
            options.spill_dir = argv[index];
            continue;
        }
        if (arg == "--spill-partitions")
        {
            if (++index >= argc)
            {
                throw std::runtime_error("--spill-partitions requires a numeric value");
            }
            options.spill_partitions = static_cast<uint32_t>(std::stoul(argv[index]));
            continue;
        }
        if (arg == "--join-batch-size")
        {
            if (++index >= argc)
            {
                throw std::runtime_error("--join-batch-size requires a numeric value");
            }
            options.join_batch_size = static_cast<uint32_t>(std::stoul(argv[index]));
            continue;
        }
        if (!arg.empty() && arg.front() == '-')
        {
            throw std::runtime_error("Unknown query option: " + std::string(arg));
        }
        if (!options.sql.empty())
        {
            throw std::runtime_error("Only one positional SQL argument is supported");
        }
        options.sql = std::string(arg);
    }
}

struct SpillCleanupGuard
{
    std::shared_ptr<mldp_pvxs_driver::query::SpillManager> spill;
    ~SpillCleanupGuard()
    {
        if (spill)
        {
            (void)spill->cleanup();
        }
    }
};

} // namespace

void mldp_pvxs_driver::cli::QuerySubcommandPreparer::prepare(const mldp_pvxs_driver::config::Config& config) const
{
    query::QueryableFactory::instance().reset();
    if (!config.hasChild("queryable"))
    {
        throw std::runtime_error(
            "Missing 'queryable' configuration. "
            "Provide one or more queryable entries via -c/--config before 'query'.");
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

int mldp_pvxs_driver::cli::QueryRunner::run(const QueryCliOptions& options, std::ostream& output) const
{
    const auto spill_dir = options.spill_dir.empty()
        ? (std::filesystem::temp_directory_path() / "mldp-query-spill").string()
        : options.spill_dir;
    auto spill_file_system = std::make_shared<arrow::fs::LocalFileSystem>();
    auto spill = std::make_shared<query::SpillManager>(spill_file_system, spill_dir);
    SpillCleanupGuard cleanup{spill};

    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
        .spill = spill,
        .memory_limit_bytes = options.memory_mb * 1024ULL * 1024ULL,
        .spill_partitions = options.spill_partitions,
        .join_batch_size = options.join_batch_size,
        .spill_fs = std::move(spill_file_system),
        .spill_dir = spill_dir,
    };

    const auto sql = loadSql(options);
    const query::QueryPlanner planner;
    const query::QueryExecutor executor;
    auto parsed = query::parseQuery(sql);
    auto physical = planner.plan(parsed);
    auto result = executor.execute(physical, context);
    formatQueryResult(result, options.format, output);
    if (!options.no_stats)
    {
        printQueryStats(result.stats, output);
    }
    return 0;
}

int mldp_pvxs_driver::cli::QuerySubcommand::run(int argc,
                                                 char** argv,
                                                 const std::vector<std::string>& global_config_sources,
                                                 std::ostream&                    output,
                                                 std::ostream&                    error) const
{
    try
    {
        QueryCliOptions options;
        parseQueryArguments(argc, argv, options);
        const auto config = global_config_sources.empty()
            ? mldp_pvxs_driver::config::Config::configFromYamlString("{}\n")
            : config::loadMergedConfigSources(global_config_sources);

        // Validate SQL before checking runtime queryable configuration so syntax
        // errors are reported as parse errors even when no queryable is configured.
        (void)query::parseQuery(loadSql(options));

        QuerySubcommandPreparer preparer;
        preparer.prepare(config);
        QueryRunner runner;
        return runner.run(options, output);
    }
    catch (const query::ParseError& parse_error)
    {
        error << "Parse error at " << parse_error.line()
              << ":" << parse_error.column()
              << " - " << parse_error.what() << "\n";
    }
    catch (const query::plan::PlannerException& planner_error)
    {
        error << query::plan::plannerErrorWhat(planner_error.error()) << "\n";
    }
    catch (const std::exception& ex)
    {
        error << "Query error: " << ex.what() << "\n";
    }
    return 1;
}
