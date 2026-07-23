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
#include <replxx.hxx>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

using namespace mldp_pvxs_driver::cli;

namespace {

mldp_pvxs_driver::config::Config selectQueryablePoolConfig(
    const std::string_view                  type,
    const mldp_pvxs_driver::config::Config& entry)
{
    const std::string_view pool_key = type == "mldp"
                                          ? "mldp-pool"
                                      : type == "mldp-pv-metadata"
                                          ? "mldp-pv-metadata-pool"
                                          : "mldp-annotation-pool";
    const auto             pools = entry.subConfig(std::string(pool_key));
    if (pools.empty())
    {
        return entry;
    }
    if (pools.size() != 1)
    {
        throw std::runtime_error(
            "queryable." + std::string(type) + "." + std::string(pool_key) + " must contain one pool configuration");
    }
    return pools.front();
}

void prepareQueryable(std::string_view type, const mldp_pvxs_driver::config::Config& cfg)
{
    using mldp_pvxs_driver::query::QueryableFactory;
    using mldp_pvxs_driver::query::impl::mldp::MLDPAnnotationQueryClient;
    using mldp_pvxs_driver::query::impl::mldp::MLDPQueryClient;

    if (type == "mldp")
    {
        QueryableFactory::instance().prepare<MLDPQueryClient>(selectQueryablePoolConfig(type, cfg));
        return;
    }
    if (type == "mldp-annotation" || type == "mldp-pv-metadata")
    {
        QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(selectQueryablePoolConfig(type, cfg));
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

void printQueryError(const std::exception& ex, std::ostream& error)
{
    if (const auto* parse_error = dynamic_cast<const mldp_pvxs_driver::query::ParseError*>(&ex))
    {
        error << "Parse error at " << parse_error->line()
              << ":" << parse_error->column()
              << " - " << parse_error->what() << "\n";
        return;
    }
    if (const auto* planner_error = dynamic_cast<const mldp_pvxs_driver::query::plan::PlannerException*>(&ex))
    {
        error << mldp_pvxs_driver::query::plan::plannerErrorWhat(planner_error->error()) << "\n";
        return;
    }
    error << "Query error: " << ex.what() << "\n";
}

std::string trim(std::string_view value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
    {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

std::optional<std::size_t> statementTerminator(std::string_view line)
{
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index)
    {
        const char character = line[index];
        if (quote != '\0')
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (character == '\\')
            {
                escaped = true;
            }
            else if (character == quote)
            {
                quote = '\0';
            }
            continue;
        }
        if (character == '\'' || character == '\"')
        {
            quote = character;
        }
        else if (character == ';')
        {
            return index;
        }
    }
    return std::nullopt;
}

bool isReplCommand(std::string_view command)
{
    return command == ".help" || command == ".clear" || command == ".quit" || command == ".exit";
}

std::filesystem::path replHistoryPath()
{
    if (const auto* state_home = std::getenv("XDG_STATE_HOME"); state_home != nullptr && *state_home != '\0')
    {
        return std::filesystem::path(state_home) / "mldp-pvxs-driver" / "query-history";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        return std::filesystem::path(home) / ".local" / "state" / "mldp-pvxs-driver" / "query-history";
    }
    return {};
}

class ReplLineEditor
{
public:
    explicit ReplLineEditor(std::istream& input)
        : input_(input)
    {
        if (&input != &std::cin || ::isatty(STDIN_FILENO) == 0)
        {
            return;
        }

        repl_ = std::make_unique<replxx::Replxx>();
        repl_->set_max_history_size(1000);
        repl_->set_word_break_characters(" \t\n\"'");
        repl_->set_completion_callback(
            [](const std::string& context, int& context_length) -> replxx::Replxx::completions_t
            {
                std::vector<std::string> candidates{
                    "SELECT", "FROM", "WHERE", "AND", "IN", "LIKE", "BETWEEN", "LIMIT", "PAGE", "TOKEN",
                    "SHOW", "TABLES", "DESCRIBE", "EXPLAIN", "AS", "INNER", "LEFT", "OUTER", "JOIN", "ON",
                    "NOW", "PREFIX", "CONTAINS", ".help", ".clear", ".quit", ".exit"};
                for (const auto& table : mldp_pvxs_driver::query::QueryableFactory::instance().registeredTables())
                {
                    candidates.push_back(table);
                }

                const auto prefix_length = std::min(static_cast<std::size_t>(std::max(context_length, 0)), context.size());
                const auto prefix = context.substr(context.size() - prefix_length);
                candidates.erase(
                    std::remove_if(candidates.begin(), candidates.end(), [&](const std::string& candidate)
                                   {
                                       return candidate.rfind(prefix, 0) != 0;
                                   }),
                    candidates.end());
                replxx::Replxx::completions_t completions;
                completions.reserve(candidates.size());
                for (const auto& candidate : candidates)
                {
                    completions.emplace_back(candidate);
                }
                return completions;
            });

        history_path_ = replHistoryPath();
        if (!history_path_.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(history_path_.parent_path(), error);
            if (!error)
            {
                (void)repl_->history_load(history_path_.string());
            }
        }
    }

    ~ReplLineEditor()
    {
        if (repl_ && !history_path_.empty())
        {
            (void)repl_->history_save(history_path_.string());
        }
    }

    std::optional<std::string> read(std::string_view prompt, std::ostream& output, bool& interrupted)
    {
        interrupted = false;
        if (!repl_)
        {
            output << prompt << std::flush;
            std::string line;
            if (!std::getline(input_, line))
            {
                return std::nullopt;
            }
            return line;
        }

        errno = 0;
        const auto* line = repl_->input(std::string(prompt).c_str());
        if (line == nullptr)
        {
            interrupted = errno == EINTR;
            return std::nullopt;
        }
        return std::string(line);
    }

    void addHistory(std::string_view entry)
    {
        if (repl_ && !entry.empty())
        {
            repl_->history_add(std::string(entry));
        }
    }

private:
    std::istream&                   input_;
    std::unique_ptr<replxx::Replxx> repl_;
    std::filesystem::path           history_path_;
};

int runRepl(const QueryCliOptions&                    options,
            const mldp_pvxs_driver::cli::QueryRunner& runner,
            std::istream&                             input,
            std::ostream&                             output,
            std::ostream&                             error)
{
    std::string    buffer;
    ReplLineEditor editor(input);
    while (true)
    {
        bool       interrupted = false;
        const auto line = editor.read(buffer.empty() ? "mldp> " : "...> ", output, interrupted);
        if (!line)
        {
            if (interrupted)
            {
                buffer.clear();
                output << "\n";
                continue;
            }
            if (!buffer.empty())
            {
                error << "Query error: incomplete SQL statement discarded at end of input\n";
            }
            return 0;
        }

        const auto command = trim(*line);
        if (command == ".clear")
        {
            editor.addHistory(command);
            buffer.clear();
            continue;
        }
        if (!buffer.empty() && isReplCommand(command))
        {
            error << "Query error: REPL commands are only available when no SQL is buffered\n";
            continue;
        }
        if (buffer.empty() && (command == ".quit" || command == ".exit"))
        {
            editor.addHistory(command);
            return 0;
        }
        if (buffer.empty() && command == ".help")
        {
            editor.addHistory(command);
            output << "Enter one SQL statement terminated by ';'.\n"
                   << "Commands: .help, .clear, .quit, .exit\n"
                   << "Editing: arrows, Ctrl-A/Ctrl-E, Ctrl-W, Ctrl-U/Ctrl-K, history, and tab completion.\n";
            continue;
        }
        if (buffer.empty() && !command.empty() && command.front() == '.')
        {
            editor.addHistory(command);
            error << "Query error: unknown REPL command '" << command << "'\n";
            continue;
        }

        const auto terminator = statementTerminator(*line);
        if (terminator)
        {
            if (!buffer.empty())
            {
                buffer.push_back('\n');
            }
            buffer.append(*line, 0, *terminator);
            if (!trim(std::string_view(*line).substr(*terminator + 1)).empty())
            {
                error << "Query error: only one SQL statement may be submitted at a time\n";
                buffer.clear();
                continue;
            }
            const auto sql = trim(buffer);
            buffer.clear();
            if (sql.empty())
            {
                error << "Query error: empty SQL statement\n";
                continue;
            }
            editor.addHistory(sql);
            try
            {
                (void)runner.run(options, sql, output);
            }
            catch (const std::exception& ex)
            {
                printQueryError(ex, error);
            }
            continue;
        }
        if (!buffer.empty())
        {
            buffer.push_back('\n');
        }
        buffer += *line;
    }
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

int mldp_pvxs_driver::cli::QueryRunner::run(const QueryCliOptions& options, const std::string_view sql, std::ostream& output) const
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
                                                 std::istream&                     input,
                                                 std::ostream&                    output,
                                                 std::ostream&                    error) const
{
    try
    {
        QueryCliOptions options;
        parseQueryArguments(argc, argv, options);
        const bool interactive = options.sql.empty() && options.sql_file.empty();
        const auto sql = interactive ? std::string{} : loadSql(options);
        // Preserve one-shot error precedence: syntax errors are reported before
        // missing runtime queryable configuration.
        if (!interactive)
        {
            (void)query::parseQuery(sql);
        }
        const auto config = global_config_sources.empty()
            ? mldp_pvxs_driver::config::Config::configFromYamlString("{}\n")
            : config::loadMergedConfigSources(global_config_sources);

        QuerySubcommandPreparer preparer;
        preparer.prepare(config);
        QueryRunner runner;
        if (interactive)
        {
            return runRepl(options, runner, input, output, error);
        }
        return runner.run(options, sql, output);
    }
    catch (const std::exception& ex)
    {
        printQueryError(ex, error);
    }
    return 1;
}
