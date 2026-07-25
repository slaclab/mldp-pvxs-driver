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
#include <query/ConsoleFooter.h>
#include <query/ExecutionContext.h>
#include <query/QueryExecutor.h>
#include <query/QueryFormatter.h>
#include <query/QueryPlanner.h>
#include <query/QueryProgress.h>
#include <query/QueryTableCatalog.h>
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
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include <sys/ioctl.h>

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
    return command == ".help" || command == ".clear" || command == ".format" || command.starts_with(".format ") ||
           command.starts_with(".format\t") || command == ".history" || command == "history" || command == "\\x" ||
           command == "\\expanded" || command.starts_with("\\expanded ") || command == ".table-fit" ||
           command.starts_with(".table-fit ") || command == ".quit" || command == ".exit";
}

std::optional<std::size_t> terminalWidth(const std::ostream& output)
{
    if (&output != &std::cout || ::isatty(STDOUT_FILENO) == 0) return std::nullopt;
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col == 0) return std::nullopt;
    return size.ws_col;
}

std::string formatName(const QueryOutputFormat format)
{
    switch (format)
    {
        case QueryOutputFormat::Table: return "table";
        case QueryOutputFormat::Json: return "json";
        case QueryOutputFormat::Csv: return "csv";
        case QueryOutputFormat::Arrow: return "arrow";
    }
    throw std::runtime_error("Unknown query output format");
}

std::string uppercase(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character)
                   {
                       return static_cast<char>(std::toupper(character));
                   });
    return result;
}

bool isUserHistoryEntry(std::string_view entry)
{
    const auto command = trim(entry);
    if (command.empty() || command.starts_with("mldp>") || command.starts_with("...>") ||
        command.starts_with("Query error:") || command.starts_with("Parse error") ||
        command.starts_with("Planner error") || command.starts_with("-- "))
    {
        return false;
    }
    if (command.front() == '.' || command == "history")
    {
        return true;
    }

    const auto first_space = command.find_first_of(" \t\r\n");
    const auto keyword = uppercase(command.substr(0, first_space));
    return keyword == "SELECT" || keyword == "SHOW" || keyword == "DESCRIBE" || keyword == "DESC" || keyword == "EXPLAIN" || keyword == "CREATE" || keyword == "DROP";
}

bool startsWithIgnoreCase(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && uppercase(value.substr(0, prefix.size())) == uppercase(prefix);
}

bool isCompletionBreak(const char character)
{
    return std::isspace(static_cast<unsigned char>(character)) || character == ',' || character == '(' || character == ')' ||
           character == ';' || character == '=' || character == '<' || character == '>' || character == '*';
}

bool isInsideQuotedLiteral(std::string_view input)
{
    char quote = '\0';
    bool escaped = false;
    for (const char character : input)
    {
        if (quote != '\0')
        {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == quote) quote = '\0';
        }
        else if (character == '\'' || character == '\"')
        {
            quote = character;
        }
    }
    return quote != '\0';
}

std::string completionToken(std::string_view input)
{
    const auto token_start = input.find_last_of(" \t\r\n,();=<>*");
    return std::string(input.substr(token_start == std::string_view::npos ? 0 : token_start + 1));
}

std::vector<std::string> wordsBeforeToken(std::string_view input)
{
    std::vector<std::string> words;
    std::string word;
    const auto token = completionToken(input);
    const auto preceding = input.substr(0, input.size() - token.size());
    for (const char character : preceding)
    {
        if (isCompletionBreak(character))
        {
            if (!word.empty())
            {
                words.push_back(word);
                word.clear();
            }
        }
        else
        {
            word.push_back(character);
        }
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

std::vector<std::pair<std::string, std::string>> tableAliases(std::string_view input)
{
    const auto words = wordsBeforeToken(input);
    std::vector<std::pair<std::string, std::string>> aliases;
    for (std::size_t index = 0; index < words.size(); ++index)
    {
        const auto keyword = uppercase(words[index]);
        if ((keyword != "FROM" && keyword != "JOIN") || index + 1 >= words.size()) continue;
        const auto& table = words[++index];
        std::string alias = table;
        if (index + 2 < words.size() && uppercase(words[index + 1]) == "AS") alias = words[index += 2];
        else if (index + 1 < words.size())
        {
            const auto next = uppercase(words[index + 1]);
            if (next != "WHERE" && next != "INNER" && next != "LEFT" && next != "OUTER" && next != "JOIN" && next != "ON" && next != "LIMIT") alias = words[++index];
        }
        aliases.emplace_back(alias, table);
    }
    return aliases;
}

std::vector<std::string> columnsForTable(const std::string_view table,
                                         const std::shared_ptr<mldp_pvxs_driver::query::QueryTableCatalog>& table_catalog)
{
    if (table_catalog)
    {
        if (const auto catalog_table = table_catalog->find(std::string(table)))
        {
            std::vector<std::string> columns;
            columns.reserve(catalog_table->schema->num_fields());
            for (const auto& field : catalog_table->schema->fields()) columns.push_back(field->name());
            return columns;
        }
    }
    try
    {
        auto queryable = mldp_pvxs_driver::query::QueryableFactory::instance().createByTable(std::string(table));
        std::vector<std::string> columns;
        for (const auto& column : queryable->tableSchema(table))
            if (column.is_output) columns.push_back(column.name);
        return columns;
    }
    catch (const std::exception&)
    {
        return {};
    }
}

bool expectsTable(const std::vector<std::string>& words)
{
    if (words.empty()) return false;
    const auto keyword = uppercase(words.back());
    if (keyword == "FROM" || keyword == "JOIN" || keyword == "DESCRIBE" || keyword == "DESC") return true;
    return keyword == "TABLE" && words.size() >= 2 && uppercase(words[words.size() - 2]) == "DROP";
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
    ReplLineEditor(std::istream& input, std::shared_ptr<mldp_pvxs_driver::query::QueryTableCatalog> table_catalog)
        : input_(input)
        , table_catalog_(std::move(table_catalog))
    {
        if (&input != &std::cin || ::isatty(STDIN_FILENO) == 0)
        {
            return;
        }

        repl_ = std::make_unique<replxx::Replxx>();
        (void)repl_->install_window_change_handler();
        repl_->set_max_history_size(1000);
        repl_->set_word_break_characters(" \t\n,();=<>*\"'");
        repl_->set_double_tab_completion(false);
        repl_->set_complete_on_empty(true);
        repl_->set_beep_on_ambiguous_completion(false);
        repl_->bind_key_internal(replxx::Replxx::KEY::TAB, "complete_line");
        repl_->set_completion_callback(
            [this](const std::string& context, int& context_length) -> replxx::Replxx::completions_t
            {
                replxx::Replxx::completions_t completions;
                context_length = mldp_pvxs_driver::cli::detail::replCompletionContextLength(context);
                for (const auto& candidate : mldp_pvxs_driver::cli::detail::replCompletions(context, table_catalog_))
                    completions.emplace_back(candidate);
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
                sanitizeHistory();
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
        if (entry.empty())
        {
            return;
        }
        session_history_.emplace_back(entry);
        if (repl_)
        {
            repl_->history_add(std::string(entry));
        }
    }

    std::vector<std::string> history() const
    {
        if (!repl_)
        {
            return session_history_;
        }

        std::vector<std::string> entries;
        auto                     scan = repl_->history_scan();
        while (scan.next())
        {
            entries.push_back(scan.get().text());
        }
        return entries;
    }

    bool clearScreen()
    {
        if (!repl_)
        {
            return false;
        }
        repl_->clear_screen();
        return true;
    }

private:
    void sanitizeHistory()
    {
        std::vector<std::string> entries;
        auto                     scan = repl_->history_scan();
        while (scan.next())
        {
            const auto& entry = scan.get().text();
            if (isUserHistoryEntry(entry))
            {
                entries.push_back(entry);
            }
        }
        if (entries.size() == static_cast<std::size_t>(repl_->history_size()))
        {
            return;
        }

        repl_->history_clear();
        for (const auto& entry : entries)
        {
            repl_->history_add(entry);
        }
        std::ofstream(history_path_, std::ios::trunc).close();
        (void)repl_->history_save(history_path_.string());
    }

    std::istream&                   input_;
    std::unique_ptr<replxx::Replxx> repl_;
    std::filesystem::path           history_path_;
    std::vector<std::string>        session_history_;
    std::shared_ptr<mldp_pvxs_driver::query::QueryTableCatalog> table_catalog_;
};

QueryOutputFormat parseFormat(std::string_view value);

int runRepl(QueryCliOptions                           options,
            const mldp_pvxs_driver::cli::QueryRunner& runner,
            std::istream&                             input,
            std::ostream&                             output,
            std::ostream&                             error)
{
    std::string buffer;
    ReplLineEditor editor(input, runner.completionCatalog(options));
    std::optional<TerminalLayout> terminal;
    if (&input == &std::cin && &output == &std::cout && ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0)
    {
        terminal.emplace(output);
        if (!terminal->initialize())
        {
            terminal.reset();
        }
    }
    ConsoleStatus status;
    while (true)
    {
        if (terminal)
        {
            terminal->refreshAtSafeBoundary();
            terminal->setStatus(status);
            terminal->redrawFooter();
        }
        bool       interrupted = false;
        const auto line = editor.read(buffer.empty() ? "mldp> " : "...> ", output, interrupted);
        if (!line)
        {
            if (interrupted)
            {
                buffer.clear();
                output << "\n";
                status.error.clear();
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
            if (!editor.clearScreen())
            {
                output << "Buffered statement cleared.\n";
            }
            if (terminal)
            {
                terminal->refreshAtSafeBoundary();
            }
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
                   << "Commands: .help, .clear, .format [table|json|csv|arrow], .table-fit [on|off], .history, .quit, .exit\n"
                   << "Display: \\expanded [on|off], \\x (toggle), or terminate a query with \\G for one expanded result.\n"
                   << "Editing: arrows, Ctrl-A/Ctrl-E, Ctrl-W, Ctrl-U/Ctrl-K, Ctrl-L (clear screen), history, and tab completion.\n";
            continue;
        }
        if (buffer.empty() && (command == ".history" || command == "history"))
        {
            const auto entries = editor.history();
            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                output << std::setw(4) << (index + 1) << "  " << entries[index] << "\n";
            }
            continue;
        }
        if (buffer.empty() && (command == "\\x" || command == "\\expanded" || command.starts_with("\\expanded ")))
        {
            const auto argument = trim(std::string_view(command).substr(command == "\\x" ? 2 : std::string_view("\\expanded").size()));
            if (command == "\\x") options.expanded = !options.expanded;
            else if (argument.empty()) { output << "Expanded display: " << (options.expanded ? "on" : "off") << "\n"; continue; }
            else if (argument == "on") options.expanded = true;
            else if (argument == "off") options.expanded = false;
            else { error << "Query error: \\expanded accepts on or off\n"; continue; }
            output << "Expanded display: " << (options.expanded ? "on" : "off") << "\n";
            continue;
        }
        if (buffer.empty() && (command == ".table-fit" || startsWithIgnoreCase(command, ".table-fit ") || startsWithIgnoreCase(command, ".table-fit\t")))
        {
            editor.addHistory(command);
            const auto argument = trim(std::string_view(command).substr(std::string_view(".table-fit").size()));
            if (argument.empty())
            {
                output << "Table fit: " << (options.table_fit ? "on" : "off") << "\n";
                continue;
            }
            if (argument == "on") options.table_fit = true;
            else if (argument == "off") options.table_fit = false;
            else { error << "Query error: .table-fit accepts on or off\n"; continue; }
            output << "Table fit: " << (options.table_fit ? "on" : "off") << "\n";
            continue;
        }
        if (buffer.empty() && (command == ".format" || startsWithIgnoreCase(command, ".format ") || startsWithIgnoreCase(command, ".format\t")))
        {
            editor.addHistory(command);
            const auto argument = trim(std::string_view(command).substr(std::string_view(".format").size()));
            if (argument.empty())
            {
                output << "Output format: " << formatName(options.format) << "\n";
                continue;
            }
            if (argument.find_first_of(" \t\r\n") != std::string::npos)
            {
                error << "Query error: .format accepts exactly one style: table,json,csv,arrow\n";
                continue;
            }
            try
            {
                options.format = parseFormat(argument);
                output << "Output format: " << formatName(options.format) << "\n";
            }
            catch (const std::exception& ex)
            {
                error << "Query error: " << ex.what() << "\n";
            }
            continue;
        }
        if (buffer.empty() && !command.empty() && command.front() == '.')
        {
            editor.addHistory(command);
            error << "Query error: unknown REPL command '" << command << "'\n";
            continue;
        }

        auto line_for_sql = *line;
        bool expanded_once = false;
        std::optional<std::size_t> terminator;
        if (const auto marker = line_for_sql.find("\\G"); marker != std::string::npos && trim(std::string_view(line_for_sql).substr(marker + 2)).empty())
        {
            expanded_once = true;
            line_for_sql.erase(marker, 2);
            line_for_sql = trim(line_for_sql);
            if (!line_for_sql.empty() && line_for_sql.back() == ';')
            {
                line_for_sql.pop_back();
            }
            terminator = line_for_sql.size();
        }
        else
        {
            terminator = statementTerminator(line_for_sql);
        }
        if (terminator)
        {
            if (!buffer.empty())
            {
                buffer.push_back('\n');
            }
            buffer.append(line_for_sql, 0, *terminator);
            if (!expanded_once && !trim(std::string_view(line_for_sql).substr(*terminator + 1)).empty())
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
            auto progress = std::make_shared<mldp_pvxs_driver::query::QueryProgressTracker>();
            status.query_running = true;
            status.progress = progress->snapshot();
            status.error.clear();
            if (terminal)
            {
                terminal->setStatus(status);
                terminal->redrawFooter();
            }
            try
            {
                auto query_options = options;
                query_options.expanded = options.expanded || expanded_once;
                mldp_pvxs_driver::query::QueryStats completed_stats;
                (void)runner.run(query_options,
                                 sql,
                                 output,
                                 progress,
                                 terminal ? std::optional<std::size_t>(static_cast<std::size_t>(terminal->columns())) : std::nullopt,
                                 !terminal,
                                 &completed_stats);
                status.query_running = false;
                status.progress = progress->snapshot();
                status.completed_stats = std::move(completed_stats);
            }
            catch (const std::exception& ex)
            {
                printQueryError(ex, error);
                status.query_running = false;
                status.progress = progress->snapshot();
                status.error = ex.what();
                status.completed_stats.reset();
            }
            if (terminal)
            {
                terminal->refreshAtSafeBoundary();
                terminal->setStatus(status);
                terminal->redrawFooter();
            }
            continue;
        }
        if (!buffer.empty())
        {
            buffer.push_back('\n');
        }
        buffer += line_for_sql;
    }
}

QueryOutputFormat parseFormat(std::string_view value)
{
    const auto normalized = uppercase(value);
    if (normalized == "TABLE")
        return QueryOutputFormat::Table;
    if (normalized == "JSON")
        return QueryOutputFormat::Json;
    if (normalized == "CSV")
        return QueryOutputFormat::Csv;
    if (normalized == "ARROW")
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
        if (arg == "--table-fit")
        {
            options.table_fit = true;
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
        if (arg == "--table-catalog-dir")
        {
            if (++index >= argc)
            {
                throw std::runtime_error("--table-catalog-dir requires a path");
            }
            options.table_catalog_dir = argv[index];
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

std::vector<std::string> mldp_pvxs_driver::cli::detail::replCompletions(
    const std::string_view input,
    const std::shared_ptr<query::QueryTableCatalog>& table_catalog)
{
    if (isInsideQuotedLiteral(input)) return {};

    const auto token = completionToken(input);
    const auto words = wordsBeforeToken(input);
    std::vector<std::string> candidates;
    const auto trimmed = trim(input);
    if (startsWithIgnoreCase(trimmed, ".format"))
    {
        candidates = token.empty() || token.front() == '.'
            ? std::vector<std::string>{".format"}
            : std::vector<std::string>{"table", "json", "csv", "arrow"};
    }
    else if (!token.empty() && token.front() == '.')
    {
        candidates = {".help", ".clear", ".format", ".table-fit", ".history", "\\expanded", "\\x", ".quit", ".exit"};
    }
    else if (expectsTable(words))
    {
        for (const auto& table : query::QueryableFactory::instance().registeredTables())
            candidates.push_back(table);
        if (table_catalog)
            for (const auto& table : table_catalog->tableNames()) candidates.push_back(table);
    }
    else if (const auto dot = token.find('.'); dot != std::string::npos)
    {
        const auto alias = token.substr(0, dot);
        const auto column_prefix = token.substr(dot + 1);
        for (const auto& [candidate_alias, table] : tableAliases(input))
            if (startsWithIgnoreCase(candidate_alias, alias))
                for (const auto& column : columnsForTable(table, table_catalog)) candidates.push_back(candidate_alias + "." + column);
        (void)column_prefix;
    }
    else
    {
        candidates = {"SELECT", "FROM", "WHERE", "AND", "OR", "IN", "LIKE", "BETWEEN", "ORDER", "BY", "ASC", "DESC", "LIMIT", "PAGE", "TOKEN",
                      "SHOW", "TABLES", "DESCRIBE", "DESC", "EXPLAIN", "CREATE", "DROP", "TEMP", "TABLE", "AS", "INNER", "LEFT", "OUTER", "JOIN", "ON", "NOW", "PREFIX", "CONTAINS"};
        const auto aliases = tableAliases(input);
        if (aliases.size() == 1)
            for (const auto& column : columnsForTable(aliases.front().second, table_catalog)) candidates.push_back(column);
    }

    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&token](const std::string& candidate)
                                    { return !startsWithIgnoreCase(candidate, token); }), candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

std::vector<std::string> mldp_pvxs_driver::cli::detail::replCompletions(const std::string_view input)
{
    return replCompletions(input, nullptr);
}

int mldp_pvxs_driver::cli::detail::replCompletionContextLength(const std::string_view input)
{
    return static_cast<int>(completionToken(input).size());
}

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

int mldp_pvxs_driver::cli::QueryRunner::run(const QueryCliOptions& options,
                                             const std::string_view sql,
                                             std::ostream&          output,
                                             std::shared_ptr<query::QueryProgressTracker> progress,
                                             std::optional<std::size_t> viewport_width,
                                             const bool print_stats,
                                             query::QueryStats* completed_stats) const
{
    const auto spill_dir = options.spill_dir.empty()
        ? (std::filesystem::temp_directory_path() / "mldp-query-spill").string()
        : options.spill_dir;
    auto spill_file_system = std::make_shared<arrow::fs::LocalFileSystem>();
    auto spill = std::make_shared<query::SpillManager>(spill_file_system, spill_dir);
    SpillCleanupGuard cleanup{spill};
    const auto catalog_dir = options.table_catalog_dir.empty()
        ? (std::filesystem::temp_directory_path() / "mldp-query-catalog").string()
        : options.table_catalog_dir;
    if (!table_catalog_ || table_catalog_dir_ != catalog_dir)
    {
        table_catalog_ = std::make_shared<query::QueryTableCatalog>(spill_file_system, catalog_dir);
        table_catalog_dir_ = catalog_dir;
    }

    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
        .spill = spill,
        .memory_limit_bytes = options.memory_mb * 1024ULL * 1024ULL,
        .spill_partitions = options.spill_partitions,
        .join_batch_size = options.join_batch_size,
        .spill_fs = std::move(spill_file_system),
        .spill_dir = spill_dir,
        .table_catalog = table_catalog_,
        .progress = progress,
    };

    if (progress)
    {
        progress->setPhase(query::QueryProgressPhase::Parsing);
    }
    auto parsed = query::parseQuery(sql);
    if (progress)
    {
        progress->setPhase(query::QueryProgressPhase::Planning);
    }
    const query::QueryPlanner planner(table_catalog_);
    const query::QueryExecutor executor;
    auto physical = planner.plan(parsed);
    auto result = executor.execute(physical, context);
    if (completed_stats != nullptr)
    {
        *completed_stats = result.stats;
    }
    if (progress)
    {
        progress->setPhase(query::QueryProgressPhase::Formatting);
    }
    auto table_width = viewport_width;
    if (options.table_fit && !table_width)
    {
        table_width = terminalWidth(output);
    }
    formatQueryResult(result,
                      options.format,
                      output,
                      options.expanded,
                      TableRenderOptions{.viewport_width = options.table_fit ? table_width : std::nullopt});
    if (!options.no_stats && print_stats)
    {
        printQueryStats(result.stats, output);
    }
    if (progress)
    {
        progress->setPhase(query::QueryProgressPhase::Complete);
    }
    return 0;
}

std::shared_ptr<mldp_pvxs_driver::query::QueryTableCatalog>
mldp_pvxs_driver::cli::QueryRunner::completionCatalog(const QueryCliOptions& options) const
{
    const auto catalog_dir = options.table_catalog_dir.empty()
        ? (std::filesystem::temp_directory_path() / "mldp-query-catalog").string()
        : options.table_catalog_dir;
    if (!table_catalog_ || table_catalog_dir_ != catalog_dir)
    {
        table_catalog_ = std::make_shared<query::QueryTableCatalog>(std::make_shared<arrow::fs::LocalFileSystem>(), catalog_dir);
        table_catalog_dir_ = catalog_dir;
    }
    return table_catalog_;
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

        if (queryable_preparer_)
        {
            queryable_preparer_(config);
        }
        else
        {
            QuerySubcommandPreparer preparer;
            preparer.prepare(config);
        }
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

mldp_pvxs_driver::cli::QuerySubcommand::QuerySubcommand(QueryablePreparer queryable_preparer)
    : queryable_preparer_(std::move(queryable_preparer))
{
}
