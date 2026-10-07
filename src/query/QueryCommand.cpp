//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/QueryCommand.h>

#include <query/planner/PhysicalPlanner.h>

#include <query/ConsoleFooter.h>
#include <query/NullQueryCommandListener.h>
#include <query/ScopedQueryInterruptHandler.h>
#include <query/QueryPager.h>

#include <config/ConfigSource.h>
#include <query/ExecutionContext.h>
#include <query/QueryCancellation.h>
#include <query/QueryExecutor.h>
#include <query/QueryFormatter.h>
#include <query/QueryPlanner.h>
#include <query/QueryProgress.h>
#include <query/QueryTableCatalog.h>
#include <query/TerminalStyle.h>
#include <query/QueryableFactory.h>
#include <query/ShardTrace.h>
#include <query/SpillManager.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <query/impl/mldp/MLDPQueryClient.h>
#include <query/parser/QueryParser.h>
#include <query/plan/PlannerError.h>

#include <arrow/filesystem/localfs.h>
#include <arrow/memory_pool.h>
#include <replxx.hxx>
#include <mldp_pvxs_driver_version.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

using namespace mldp_pvxs_driver::cli;

namespace {

std::mutex                 g_fallback_output_mutex;

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

void printQueryError(const std::exception& ex, std::ostream& error, const bool color = false)
{
    namespace style = mldp_pvxs_driver::cli::style;
    if (const auto* parse_error = dynamic_cast<const mldp_pvxs_driver::query::ParseError*>(&ex))
    {
        error << style::paint(color, style::red, "Parse error") << " at " << parse_error->line()
              << ":" << parse_error->column()
              << " - " << parse_error->what() << "\n";
        return;
    }
    if (const auto* planner_error = dynamic_cast<const mldp_pvxs_driver::query::plan::PlannerException*>(&ex))
    {
        error << style::paint(color, style::red, mldp_pvxs_driver::query::plan::plannerErrorWhat(planner_error->error())) << "\n";
        return;
    }
    error << style::paint(color, style::red, "Query error:") << " " << ex.what() << "\n";
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
    char        quote = '\0';
    bool        escaped = false;
    std::size_t parentheses = 0;
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
        else if (character == '(')
        {
            ++parentheses;
        }
        else if (character == ')' && parentheses != 0)
        {
            --parentheses;
        }
        else if (character == ';' && parentheses == 0)
        {
            return index;
        }
    }
    return std::nullopt;
}

bool isReplCommand(std::string_view command)
{
    return command == ".help" || command.starts_with(".help ") || command.starts_with(".help\t") || command == ".clear" || command == ".format" || command.starts_with(".format ") ||
           command.starts_with(".format\t") || command == ".pager" || command.starts_with(".pager ") || command.starts_with(".pager\t") ||
           command == ".history" || command == "history" || command == "\\x" ||
           command == "\\expanded" || command.starts_with("\\expanded ") || command == ".table-fit" ||
           command.starts_with(".table-fit ") || command == ".color" || command.starts_with(".color ") || command == ".quit" || command == ".exit";
}

bool startsWithIgnoreCase(std::string_view value, std::string_view prefix);

namespace help {

namespace style = mldp_pvxs_driver::cli::style;

void heading(std::ostream& out, const bool color, const std::string_view text)
{
    out << style::paint(color, style::cyan, text) << "\n";
}

/// Prints one "  <term>  <description>" row with the term padded to @p width.
void row(std::ostream& out, const bool color, const std::string_view term, const std::string_view text, const std::size_t width = 34)
{
    out << "  " << style::paint(color, style::bold, term);
    out << std::string(term.size() < width ? width - term.size() : 1, ' ') << text << "\n";
}

void example(std::ostream& out, const bool color, const std::string_view sql)
{
    out << "  " << style::paint(color, style::green, sql) << "\n";
}

void printCommands(std::ostream& out, const bool color)
{
    heading(out, color, "REPL commands (only when no SQL is buffered)");
    row(out, color, ".help [topic]", "overview, or detailed help on a topic");
    row(out, color, ".clear", "discard the buffered statement and clear the screen");
    row(out, color, ".format [table|json|csv|arrow]", "show or set the output format");
    row(out, color, ".pager [on|off]", "show or toggle paging of long results");
    row(out, color, ".table-fit [on|off]", "show or toggle fitting tables to terminal width");
    row(out, color, ".color [on|off]", "show or toggle ANSI color");
    row(out, color, ".history", "list previous commands");
    row(out, color, "\\expanded [on|off], \\x", "show/set or toggle expanded (one field per line) display");
    row(out, color, "<query> \\G", "run one query with expanded display");
    row(out, color, ".quit, .exit, Ctrl-Q", "leave the REPL");
    out << "  Without an argument, .format/.pager/.table-fit/.color/\\expanded print the current value.\n";
}

void printEditing(std::ostream& out, const bool color)
{
    heading(out, color, "Line editing");
    row(out, color, "Left/Right, Ctrl-A/Ctrl-E", "move cursor; jump to start/end of line", 28);
    row(out, color, "Up/Down", "browse history (.history lists it)", 28);
    row(out, color, "Ctrl-W", "delete previous word", 28);
    row(out, color, "Ctrl-U/Ctrl-K", "delete to start/end of line", 28);
    row(out, color, "Ctrl-L", "clear screen", 28);
    row(out, color, "Ctrl-C", "cancel running query or discard current input", 28);
    row(out, color, "Ctrl-Q", "exit", 28);
    row(out, color, "Tab", "complete keywords, tables, columns and command arguments", 28);
    out << "  A statement may span several lines; it runs when terminated by ';'.\n";
}

void printDisplay(std::ostream& out, const bool color)
{
    heading(out, color, "Output and display");
    row(out, color, ".format table", "aligned text table (default)", 20);
    row(out, color, ".format json", "JSON Lines: one object per row", 20);
    row(out, color, ".format csv", "comma-separated values with header", 20);
    row(out, color, ".format arrow", "Arrow IPC stream", 20);
    row(out, color, ".pager on|off", "page results longer than the terminal", 20);
    row(out, color, ".table-fit on|off", "shrink wide tables to the terminal width", 20);
    row(out, color, ".color on|off", "ANSI colors (also disabled by NO_COLOR)", 20);
    row(out, color, "\\x  /  ... \\G", "expanded display: always / for a single query", 20);
}

void printTables(std::ostream& out, const bool color)
{
    heading(out, color, "Tables and introspection");
    row(out, color, "SHOW TABLES;", "list queryable and temporary tables", 26);
    row(out, color, "DESCRIBE <table>;", "list columns of a table", 26);
    row(out, color, "SHOW FUNCTIONS;", "list scalar and aggregate functions", 26);
    row(out, color, "SHOW OPERATORS;", "list supported operators", 26);
    row(out, color, "EXPLAIN <query>;", "show the plan without running it", 26);
    heading(out, color, "MLDP tables");
    row(out, color, "mldp.time_series", "one row per sample (pv, time, value)", 26);
    row(out, color, "mldp.time_series_table", "samples pivoted: one column per PV", 26);
    row(out, color, "mldp.pv_stats", "per-PV statistics", 26);
    row(out, color, "mldp.pv_metadata", "PV metadata", 26);
    out << "  Filters on pv and time are pushed down to MLDP; time accepts epoch seconds or NOW - <n>[s|m|h|d].\n";
    heading(out, color, "Temporary tables");
    example(out, color, "CREATE TEMP TABLE recent AS SELECT pv, value FROM mldp.time_series WHERE pv = 'A';");
    example(out, color, "DROP TABLE recent;");
}

void printMatching(std::ostream& out, const bool color)
{
    heading(out, color, "String matching");
    row(out, color, "col PREFIX 'abc'", "starts with 'abc' (case-sensitive, no wildcards)", 22);
    row(out, color, "col CONTAINS 'abc'", "contains 'abc' (case-sensitive, no wildcards)", 22);
    row(out, color, "col LIKE 'a%b_c'", "case-insensitive; % or * = any run, _ = one char, \\ escapes", 22);
    out << "  On mldp.time_series, mldp.time_series_table and mldp.pv_stats, pv PREFIX/CONTAINS/LIKE\n"
        << "  is sent to MLDP as a PV-name regex (no pv = / IN needed), then verified locally.\n";
    example(out, color, "SELECT * FROM mldp.pv_stats WHERE pv LIKE 'ltu:%:bpm%';");
}

void printGrouping(std::ostream& out, const bool color)
{
    heading(out, color, "Grouping (runs locally on the fetched rows)");
    row(out, color, "SELECT DISTINCT ON (k) k, other ...", "first row per k (use ORDER BY to pick it)", 38);
    row(out, color, "... GROUP BY k [HAVING COUNT(*) > 1]", "one row per k; HAVING filters groups", 38);
    row(out, color, "ORDER BY 2 DESC", "order by output column position", 38);
    out << "  Aggregates: COUNT(*|x|DISTINCT x), SUM, AVG, MIN, MAX, FIRST, LAST\n";
    example(out, color, "SELECT pv, COUNT(*), AVG(value) FROM mldp.time_series WHERE pv PREFIX 'ltu:' AND time >= NOW - 1h GROUP BY pv ORDER BY 2 DESC;");
}

void printExamples(std::ostream& out, const bool color)
{
    heading(out, color, "Examples");
    example(out, color, "SHOW TABLES;");
    example(out, color, "DESCRIBE mldp.time_series;");
    example(out, color, "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'A' AND time >= NOW - 10m LIMIT 20;");
    example(out, color, "SELECT * FROM mldp.time_series_table WHERE pv IN ('A', 'B') AND time >= NOW - 1h;");
    example(out, color, "SELECT * FROM mldp.pv_stats WHERE pv LIKE 'ltu:%:bpm%';");
    example(out, color, "SELECT pv, MAX(value) FROM mldp.time_series WHERE pv PREFIX 'ltu:' AND time >= NOW - 1h GROUP BY pv;");
    example(out, color, "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'A' LIMIT 1 \\G");
}

struct Topic
{
    std::string_view name;
    std::string_view summary;
    void (*print)(std::ostream&, bool);
};

constexpr std::array<Topic, 7> kTopics{{
    {"commands", "all REPL dot/backslash commands", printCommands},
    {"editing", "keyboard shortcuts and completion", printEditing},
    {"display", "output formats, pager, expanded view", printDisplay},
    {"tables", "MLDP tables, SHOW/DESCRIBE/EXPLAIN, temp tables", printTables},
    {"matching", "PREFIX, CONTAINS, LIKE and PV-name pushdown", printMatching},
    {"grouping", "DISTINCT ON, GROUP BY, HAVING, aggregates", printGrouping},
    {"examples", "copy-paste queries", printExamples},
}};

std::string topicNames()
{
    std::string names;
    for (const auto& topic : kTopics)
        names.append(names.empty() ? "" : ", ").append(topic.name);
    return names;
}

void printOverview(std::ostream& out, const bool color)
{
    out << "Enter one SQL statement terminated by ';'.\n\n";
    printCommands(out, color);
    out << "\n";
    heading(out, color, "Help topics: .help <topic>");
    for (const auto& topic : kTopics)
        row(out, color, topic.name, topic.summary, 12);
    out << "\n" << style::paint(color, style::dim, "Editing: arrows, Ctrl-A/Ctrl-E, Ctrl-W, Ctrl-U/Ctrl-K, Ctrl-L (clear screen), Ctrl-Q (exit), Tab completion.") << "\n";
}

/// Prints help for @p topic (empty = overview). Returns false for an unknown topic.
bool print(std::ostream& out, const bool color, const std::string_view topic)
{
    if (topic.empty())
    {
        printOverview(out, color);
        return true;
    }
    for (const auto& candidate : kTopics)
    {
        if (topic.size() == candidate.name.size() && startsWithIgnoreCase(candidate.name, topic))
        {
            candidate.print(out, color);
            return true;
        }
    }
    return false;
}

} // namespace help

std::optional<std::size_t> terminalWidth(const std::ostream& output)
{
    if (&output != &std::cout || ::isatty(STDOUT_FILENO) == 0)
        return std::nullopt;
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col == 0)
        return std::nullopt;
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
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == quote)
                quote = '\0';
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
    std::string              word;
    const auto               token = completionToken(input);
    const auto               preceding = input.substr(0, input.size() - token.size());
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
    if (!word.empty())
        words.push_back(word);
    return words;
}

std::vector<std::pair<std::string, std::string>> tableAliases(std::string_view input)
{
    const auto                                       words = wordsBeforeToken(input);
    std::vector<std::pair<std::string, std::string>> aliases;
    for (std::size_t index = 0; index < words.size(); ++index)
    {
        const auto keyword = uppercase(words[index]);
        if ((keyword != "FROM" && keyword != "JOIN") || index + 1 >= words.size())
            continue;
        const auto& table = words[++index];
        std::string alias = table;
        if (index + 2 < words.size() && uppercase(words[index + 1]) == "AS")
            alias = words[index += 2];
        else if (index + 1 < words.size())
        {
            const auto next = uppercase(words[index + 1]);
            if (next != "WHERE" && next != "INNER" && next != "LEFT" && next != "OUTER" && next != "JOIN" && next != "ON" && next != "LIMIT")
                alias = words[++index];
        }
        aliases.emplace_back(alias, table);
    }
    return aliases;
}

std::vector<std::string> columnsForTable(const std::string_view                                             table,
                                         const std::shared_ptr<mldp_pvxs_driver::query::QueryTableCatalog>& table_catalog)
{
    if (table_catalog)
    {
        if (const auto catalog_table = table_catalog->find(std::string(table)))
        {
            std::vector<std::string> columns;
            columns.reserve(catalog_table->schema->num_fields());
            for (const auto& field : catalog_table->schema->fields())
                columns.push_back(field->name());
            return columns;
        }
    }
    try
    {
        auto                     queryable = mldp_pvxs_driver::query::QueryableFactory::instance().createByTable(std::string(table));
        std::vector<std::string> columns;
        for (const auto& column : queryable->tableSchema(table))
            if (column.is_output)
                columns.push_back(column.name);
        return columns;
    }
    catch (const std::exception&)
    {
        return {};
    }
}

bool expectsTable(const std::vector<std::string>& words)
{
    if (words.empty())
        return false;
    const auto keyword = uppercase(words.back());
    if (keyword == "FROM" || keyword == "JOIN" || keyword == "DESCRIBE" || keyword == "DESC")
        return true;
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
        repl_->bind_key(replxx::Replxx::KEY::control('Q'), [this](char32_t)
                        {
                            quit_requested_ = true;
                            return replxx::Replxx::ACTION_RESULT::BAIL;
                        });
        repl_->set_completion_callback(
            [this](const std::string& context, int& context_length) -> replxx::Replxx::completions_t
            {
                replxx::Replxx::completions_t completions;
                context_length = mldp_pvxs_driver::cli::detail::replCompletionContextLength(context);
                for (const auto& candidate : mldp_pvxs_driver::cli::detail::replCompletions(context, table_catalog_))
                    completions.emplace_back(candidate);
                return completions;
            });
        repl_->set_highlighter_callback(
            [this](const std::string& context, replxx::Replxx::colors_t& colors)
            {
                if (!color_)
                    return;
                using Kind = mldp_pvxs_driver::cli::detail::ReplHighlight;
                using Color = replxx::Replxx::Color;
                const auto  kinds = mldp_pvxs_driver::cli::detail::replHighlight(context);
                std::size_t code_point = 0;
                for (std::size_t byte = 0; byte < kinds.size() && code_point < colors.size(); ++byte)
                {
                    // colors_t is indexed by code point; skip UTF-8 continuation bytes.
                    if ((static_cast<unsigned char>(context[byte]) & 0xC0) == 0x80)
                        continue;
                    switch (kinds[byte])
                    {
                    case Kind::Keyword: colors[code_point] = Color::BRIGHTBLUE; break;
                    case Kind::Function: colors[code_point] = Color::BRIGHTCYAN; break;
                    case Kind::String: colors[code_point] = Color::GREEN; break;
                    case Kind::Number: colors[code_point] = Color::MAGENTA; break;
                    case Kind::Comment: colors[code_point] = Color::GRAY; break;
                    case Kind::Command: colors[code_point] = Color::YELLOW; break;
                    case Kind::Default: break;
                    }
                    ++code_point;
                }
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

    void setColor(const bool color)
    {
        color_ = color;
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

    bool consumeQuitRequested() noexcept
    {
        const auto requested = quit_requested_;
        quit_requested_ = false;
        return requested;
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

    std::istream&                                               input_;
    std::unique_ptr<replxx::Replxx>                             repl_;
    std::filesystem::path                                       history_path_;
    std::vector<std::string>                                    session_history_;
    std::shared_ptr<mldp_pvxs_driver::query::QueryTableCatalog> table_catalog_;
    bool                                                        quit_requested_{false};
    bool                                                        color_{false};
};

QueryOutputFormat parseFormat(std::string_view value);

int runRepl(QueryCliOptions                           options,
            const mldp_pvxs_driver::cli::QueryRunner& runner,
            mldp_pvxs_driver::cli::QueryCommandListener& listener,
            std::istream&                             input,
            std::ostream&                             output,
            std::ostream&                             error)
{
    std::string                   buffer;
    ReplLineEditor                editor(input, runner.completionCatalog(options));
    QueryPager                    pager;
    QueryContinuationRegistry     continuations;
    auto                          output_mutex = std::make_shared<std::mutex>();
    const auto                     real_terminal = &input == &std::cin && &output == &std::cout &&
                                          ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0;
    namespace style = mldp_pvxs_driver::cli::style;
    options.color = real_terminal && style::colorEnabledByDefault(output);
    editor.setColor(options.color);
    const auto error_color = [&] { return options.color && style::colorEnabledByDefault(error); };
    const auto errorLabel = [&] { return style::paint(error_color(), style::red, "Query error:"); };
    if (real_terminal)
    {
        const auto version = std::to_string(MLDP_PVXS_DRIVER_VERSION_MAJOR) + "." +
                             std::to_string(MLDP_PVXS_DRIVER_VERSION_MINOR) + "." +
                             std::to_string(MLDP_PVXS_DRIVER_VERSION_PATCH);
        output << "\x1b[2J\x1b[H"
               << style::paint(options.color, style::cyan, "mldp query REPL")
               << style::paint(options.color, style::dim, "  v" + version + "  \xe2\x80\x94  type .help for help, .quit to exit")
               << "\n\n";
        output.flush();
    }
    InlineStatus                  inline_status(output, output_mutex);
    while (true)
    {
        bool       interrupted = false;
        const auto prompt = buffer.empty() ? style::paint(options.color, style::green, "mldp") + "> "
                                           : style::paint(options.color, style::dim, "...> ");
        const auto line = editor.read(prompt, output, interrupted);
        if (!line)
        {
            if (editor.consumeQuitRequested())
            {
                return 0;
            }
            if (interrupted)
            {
                buffer.clear();
                output << "\n";
                continue;
            }
            if (!buffer.empty())
            {
                error << errorLabel() << " incomplete SQL statement discarded at end of input\n";
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
            continue;
        }
        if (!buffer.empty() && isReplCommand(command))
        {
            error << errorLabel() << " REPL commands are only available when no SQL is buffered\n";
            continue;
        }
        if (buffer.empty() && (command == ".quit" || command == ".exit"))
        {
            editor.addHistory(command);
            return 0;
        }
        if (buffer.empty() && (command == ".help" || startsWithIgnoreCase(command, ".help ") || startsWithIgnoreCase(command, ".help\t")))
        {
            editor.addHistory(command);
            const auto topic = trim(std::string_view(command).substr(std::string_view(".help").size()));
            if (!help::print(output, options.color, topic))
            {
                error << errorLabel() << " unknown help topic '" << topic << "'; topics: " << help::topicNames() << "\n";
            }
            continue;
        }
        if (buffer.empty() && (command == ".pager" || startsWithIgnoreCase(command, ".pager ") || startsWithIgnoreCase(command, ".pager\t")))
        {
            editor.addHistory(command);
            const auto argument = trim(std::string_view(command).substr(std::string_view(".pager").size()));
            if (argument.empty())
            {
                output << "Pager: " << (options.pager ? "on" : "off") << "\n";
            }
            else if (argument == "on")
            {
                options.pager = true;
                output << "Pager: on\n";
            }
            else if (argument == "off")
            {
                options.pager = false;
                output << "Pager: off\n";
            }
            else
            {
                error << errorLabel() << " .pager accepts on or off\n";
            }
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
            if (command == "\\x")
                options.expanded = !options.expanded;
            else if (argument.empty())
            {
                output << "Expanded display: " << (options.expanded ? "on" : "off") << "\n";
                continue;
            }
            else if (argument == "on")
                options.expanded = true;
            else if (argument == "off")
                options.expanded = false;
            else
            {
                error << errorLabel() << " \\expanded accepts on or off\n";
                continue;
            }
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
            if (argument == "on")
                options.table_fit = true;
            else if (argument == "off")
                options.table_fit = false;
            else
            {
                error << errorLabel() << " .table-fit accepts on or off\n";
                continue;
            }
            output << "Table fit: " << (options.table_fit ? "on" : "off") << "\n";
            continue;
        }
        if (buffer.empty() && (command == ".color" || startsWithIgnoreCase(command, ".color ") || startsWithIgnoreCase(command, ".color\t")))
        {
            editor.addHistory(command);
            const auto argument = trim(std::string_view(command).substr(std::string_view(".color").size()));
            if (argument == "on")
                options.color = true;
            else if (argument == "off")
                options.color = false;
            else if (!argument.empty())
            {
                error << errorLabel() << " .color accepts on or off\n";
                continue;
            }
            editor.setColor(options.color);
            output << "Color: " << (options.color ? "on" : "off") << "\n";
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
                error << errorLabel() << " .format accepts exactly one style: table,json,csv,arrow\n";
                continue;
            }
            try
            {
                options.format = parseFormat(argument);
                output << "Output format: " << formatName(options.format) << "\n";
            }
            catch (const std::exception& ex)
            {
                error << errorLabel() << " " << ex.what() << "\n";
            }
            continue;
        }
        if (buffer.empty() && !command.empty() && command.front() == '.')
        {
            editor.addHistory(command);
            error << errorLabel() << " unknown REPL command '" << command << "'\n";
            continue;
        }

        auto                       line_for_sql = *line;
        bool                       expanded_once = false;
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
            std::string pending_sql = buffer;
            if (!pending_sql.empty())
                pending_sql.push_back('\n');
            const auto line_offset = pending_sql.size();
            pending_sql.append(line_for_sql);
            if (const auto pending_terminator = statementTerminator(pending_sql); pending_terminator && *pending_terminator >= line_offset)
            {
                terminator = *pending_terminator - line_offset;
            }
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
                error << errorLabel() << " only one SQL statement may be submitted at a time\n";
                buffer.clear();
                continue;
            }
            const auto sql = trim(buffer);
            buffer.clear();
            if (sql.empty())
            {
                error << errorLabel() << " empty SQL statement\n";
                continue;
            }
            editor.addHistory(sql);
            auto progress = std::make_shared<mldp_pvxs_driver::query::QueryProgressTracker>();
            auto cancellation = std::make_shared<mldp_pvxs_driver::query::QueryCancellation>();
            progress->setPhase(mldp_pvxs_driver::query::QueryProgressPhase::Parsing);
            listener.querySubmitted(sql);
            auto query_options = options;
            query_options.expanded = options.expanded || expanded_once;
            mldp_pvxs_driver::query::QueryStats completed_stats;
            ScopedQueryInterruptHandler         interrupt_handler;
            const auto                           page_result = query_options.pager && pager.canPage(input, output, query_options.format);
            std::ostringstream                   paged_output;
            const bool                            buffer_result = page_result || real_terminal;
            auto&                                 result_output = buffer_result ? static_cast<std::ostream&>(paged_output) : output;
            ConsoleStatus                         status{.query_running = true, .progress = progress->snapshot()};
            const auto tw = [&]() -> int {
                if (!real_terminal) return 0;
                winsize sz{};
                if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &sz) != 0 || sz.ws_col == 0) return 80;
                return static_cast<int>(sz.ws_col);
            }();
            if (real_terminal && !page_result)
            {
                output << "\n";
                inline_status.show(status, tw);
            }
            auto                                query = std::async(std::launch::async,
                                                                   [&]
                                                                   {
                                        return runner.run(query_options,
                                                          sql,
                                                          result_output,
                                                          progress,
                                                          std::nullopt,
                                                          !real_terminal || page_result,
                                                          &completed_stats,
                                                          cancellation,
                                                          output_mutex,
                                                          &continuations);
                                                                   });
            try
            {
                while (query.wait_for(std::chrono::milliseconds{250}) != std::future_status::ready)
                {
                    if (interrupt_handler.consumeInterrupt())
                    {
                        progress->setPhase(mldp_pvxs_driver::query::QueryProgressPhase::Cancelling);
                        cancellation->requestCancel();
                    }
                    const auto snapshot = progress->snapshot();
                    status.progress = snapshot;
                    if (real_terminal && !page_result)
                        inline_status.show(status, tw);
                    listener.progressChanged(snapshot);
                }
                (void)query.get();
                if (real_terminal && !page_result)
                {
                    inline_status.clear();
                    output << "\n";
                    output << paged_output.str();
                    if (!query_options.no_stats)
                    {
                        output << style::paint(options.color, style::dim, queryStatsLine(completed_stats)) << "\n";
                    }
                }
                if (page_result)
                {
                    std::string pager_error;
                    if (!pager.write(paged_output.str(), pager_error))
                    {
                        error << style::paint(error_color(), style::yellow, "Query warning:") << " " << pager_error << "; writing result directly\n";
                        output << paged_output.str();
                    }
                }
                listener.queryCompleted(completed_stats);
                listener.queryIdle();
            }
            catch (const mldp_pvxs_driver::query::QueryCancelled&)
            {
                inline_status.clear();
                listener.queryCancelled();
                listener.queryIdle();
                error << style::paint(error_color(), style::yellow, "Query cancelled") << "\n";
            }
            catch (const std::exception& ex)
            {
                inline_status.clear();
                printQueryError(ex, error, error_color());
                listener.queryFailed(ex.what());
                listener.queryIdle();
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

void printQueryUsage(std::ostream& output)
{
        output << R"(Usage: mldp_pvxs_driver [global options] query [query options] ["SQL"]

Run without SQL or --file to start an interactive query session.
Global -c/--config options must appear before 'query'.

Query options:
    -h, --help                    Show this help message and exit
    --file PATH                   Read SQL text from PATH
    --format FORMAT               Output format: table, json, csv, arrow
    --table-fit                   Fit table output to the terminal width
    --no-stats                    Suppress the query statistics footer
    --trace-shards                Write window-shard diagnostics to stderr
    --trace-shards-file PATH      Write window-shard diagnostics to PATH
    --memory-mb N                 Set the memory budget in MiB (default: 256)
    --spill-dir PATH              Set the spill-file directory
    --table-catalog-dir PATH      Set the persistent table catalog directory
    --spill-partitions N          Set spill partitions (default: 16)
    --join-batch-size N           Set join batch size (default: 100)
)";
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
        if (arg == "--trace-shards")
        {
            options.trace_shards = true;
            continue;
        }
        if (arg == "--trace-shards-file")
        {
            if (++index >= argc)
            {
                throw std::runtime_error("--trace-shards-file requires a path");
            }
            options.trace_shards = true;
            options.shard_trace_file = argv[index];
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

class InteractivePageStream final : public mldp_pvxs_driver::query::IRecordBatchStream
{
public:
    InteractivePageStream(mldp_pvxs_driver::query::IRecordBatchStreamUPtr input, const uint64_t limit)
        : input_(std::move(input)), remaining_(limit)
    {
    }

    void reset(const uint64_t limit)
    {
        remaining_ = limit;
    }

    [[nodiscard]] bool pageFull() const noexcept
    {
        return remaining_ == 0;
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        if (remaining_ == 0)
            return nullptr;
        if (!pending_)
            pending_ = input_->next();
        if (!pending_)
            return nullptr;
        const auto available = static_cast<uint64_t>(pending_->num_rows() - offset_);
        const auto count = std::min(available, remaining_);
        const auto result = pending_->Slice(offset_, static_cast<int64_t>(count));
        offset_ += static_cast<int64_t>(count);
        remaining_ -= count;
        if (offset_ == pending_->num_rows())
        {
            pending_.reset();
            offset_ = 0;
        }
        return result;
    }

private:
    mldp_pvxs_driver::query::IRecordBatchStreamUPtr input_;
    std::shared_ptr<arrow::RecordBatch>             pending_;
    int64_t                                         offset_{0};
    uint64_t                                        remaining_{0};
};

std::string queryFingerprint(const std::string_view sql)
{
    const auto uppercase_sql = uppercase(sql);
    const auto page = uppercase_sql.find(" PAGE TOKEN ");
    return page == std::string::npos ? std::string(sql) : std::string(sql.substr(0, page));
}

} // namespace

mldp_pvxs_driver::cli::QueryContinuationRegistry::QueryContinuationRegistry(const std::chrono::steady_clock::duration idle_timeout)
    : idle_timeout_(idle_timeout)
{
}

mldp_pvxs_driver::cli::QueryContinuationRegistry::~QueryContinuationRegistry()
{
    clear();
}

std::string mldp_pvxs_driver::cli::QueryContinuationRegistry::store(Entry entry)
{
    cleanupExpired();
    std::random_device random_device;
    std::mt19937_64    generator(random_device());
    std::string        token;
    do
    {
        std::ostringstream token_stream;
        token_stream << "p9:" << std::hex << generator() << generator();
        token = token_stream.str();
    } while (entries_.contains(token));
    entry.expires_at = std::chrono::steady_clock::now() + idle_timeout_;
    entries_.emplace(token, std::move(entry));
    return token;
}

mldp_pvxs_driver::cli::QueryContinuationRegistry::Entry
mldp_pvxs_driver::cli::QueryContinuationRegistry::take(const std::string&     token,
                                                       const std::string_view fingerprint)
{
    cleanupExpired();
    const auto found = entries_.find(token);
    if (found == entries_.end())
        throw std::runtime_error("PAGE TOKEN is invalid, expired, or has already been consumed in this REPL session");
    if (found->second.fingerprint != fingerprint)
        throw std::runtime_error("PAGE TOKEN does not match this query in the current REPL session");
    auto entry = std::move(found->second);
    entries_.erase(found);
    return entry;
}

void mldp_pvxs_driver::cli::QueryContinuationRegistry::cleanupExpired()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto entry = entries_.begin(); entry != entries_.end();)
    {
        if (entry->second.expires_at > now)
        {
            ++entry;
            continue;
        }
        if (entry->second.cancellation)
            entry->second.cancellation->requestCancel();
        entry = entries_.erase(entry);
    }
}

void mldp_pvxs_driver::cli::QueryContinuationRegistry::clear()
{
    for (auto& [token, entry] : entries_)
    {
        if (entry.cancellation)
            entry.cancellation->requestCancel();
    }
    entries_.clear();
}

std::vector<std::string> mldp_pvxs_driver::cli::detail::replCompletions(
    const std::string_view                           input,
    const std::shared_ptr<query::QueryTableCatalog>& table_catalog)
{
    if (isInsideQuotedLiteral(input))
        return {};

    const auto               token = completionToken(input);
    const auto               words = wordsBeforeToken(input);
    std::vector<std::string> candidates;
    const auto               trimmed = trim(input);
    if (startsWithIgnoreCase(trimmed, ".format"))
    {
        candidates = token.empty() || token.front() == '.'
                         ? std::vector<std::string>{".format"}
                         : std::vector<std::string>{"table", "json", "csv", "arrow"};
    }
    else if (startsWithIgnoreCase(trimmed, ".pager"))
    {
        candidates = token.empty() || token.front() == '.'
                         ? std::vector<std::string>{".pager"}
                         : std::vector<std::string>{"on", "off"};
    }
    else if (startsWithIgnoreCase(trimmed, ".help"))
    {
        if (token.empty() || token.front() == '.')
            candidates = {".help"};
        else
            for (const auto& topic : help::kTopics)
                candidates.emplace_back(topic.name);
    }
    else if (startsWithIgnoreCase(trimmed, ".color"))
    {
        candidates = token.empty() || token.front() == '.'
                         ? std::vector<std::string>{".color"}
                         : std::vector<std::string>{"on", "off"};
    }
    else if (!token.empty() && token.front() == '.')
    {
        candidates = {".help", ".clear", ".format", ".pager", ".table-fit", ".color", ".history", "\\expanded", "\\x", ".quit", ".exit"};
    }
    else if (expectsTable(words))
    {
        for (const auto& table : query::QueryableFactory::instance().registeredTables())
            candidates.push_back(table);
        if (table_catalog)
            for (const auto& table : table_catalog->tableNames())
                candidates.push_back(table);
    }
    else if (const auto dot = token.find('.'); dot != std::string::npos)
    {
        const auto alias = token.substr(0, dot);
        const auto column_prefix = token.substr(dot + 1);
        for (const auto& [candidate_alias, table] : tableAliases(input))
            if (startsWithIgnoreCase(candidate_alias, alias))
                for (const auto& column : columnsForTable(table, table_catalog))
                    candidates.push_back(candidate_alias + "." + column);
        (void)column_prefix;
    }
    else
    {
        candidates = {"SELECT", "DISTINCT", "FROM", "WHERE", "GROUP", "HAVING", "COUNT", "SUM", "AVG", "MIN", "MAX", "FIRST", "LAST",
                      "AND", "OR", "IN", "LIKE", "BETWEEN", "ORDER", "BY", "ASC", "DESC", "LIMIT", "PAGE", "TOKEN",
                      "SHOW", "TABLES", "FUNCTIONS", "OPERATORS", "DESCRIBE", "DESC", "EXPLAIN", "CREATE", "DROP", "TEMP", "TABLE", "AS", "INNER", "LEFT", "OUTER", "JOIN", "ON", "NOW", "PREFIX", "CONTAINS"};
        const auto aliases = tableAliases(input);
        if (aliases.size() == 1)
            for (const auto& column : columnsForTable(aliases.front().second, table_catalog))
                candidates.push_back(column);
    }

    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&token](const std::string& candidate)
                                    {
                                        return !startsWithIgnoreCase(candidate, token);
                                    }),
                     candidates.end());
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

std::vector<mldp_pvxs_driver::cli::detail::ReplHighlight> mldp_pvxs_driver::cli::detail::replHighlight(const std::string_view input)
{
    static const std::vector<std::string_view> keywords = {
        "SELECT", "DISTINCT", "FROM", "WHERE", "GROUP", "HAVING", "AND", "OR", "NOT", "IN", "IS", "NULL", "LIKE", "BETWEEN",
        "ORDER", "BY", "ASC", "DESC", "LIMIT", "PAGE", "TOKEN", "SHOW", "TABLES", "FUNCTIONS", "OPERATORS", "DESCRIBE",
        "EXPLAIN", "CREATE", "DROP", "TEMP", "TABLE", "AS", "INNER", "LEFT", "OUTER", "JOIN", "ON", "PREFIX", "CONTAINS",
        "TRUE", "FALSE"};
    static const std::vector<std::string_view> functions = {"COUNT", "SUM", "AVG", "MIN", "MAX", "FIRST", "LAST", "NOW"};
    const auto is_word = [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };

    std::vector<ReplHighlight> kinds(input.size(), ReplHighlight::Default);
    const auto first = input.find_first_not_of(" \t");
    if (first != std::string_view::npos && (input[first] == '.' || input[first] == '\\'))
    {
        std::fill(kinds.begin() + static_cast<std::ptrdiff_t>(first), kinds.end(), ReplHighlight::Command);
        return kinds;
    }
    const auto mark = [&](const std::size_t from, const std::size_t to, const ReplHighlight kind)
    { std::fill(kinds.begin() + static_cast<std::ptrdiff_t>(from), kinds.begin() + static_cast<std::ptrdiff_t>(to), kind); };

    for (std::size_t i = 0; i < input.size();)
    {
        const char c = input[i];
        if (c == '\'')
        {
            auto end = i + 1;
            while (end < input.size())
            {
                if (input[end] == '\'' && end + 1 < input.size() && input[end + 1] == '\'') { end += 2; continue; }
                if (input[end++] == '\'') break;
            }
            mark(i, end, ReplHighlight::String);
            i = end;
        }
        else if (c == '-' && i + 1 < input.size() && input[i + 1] == '-')
        {
            const auto end = std::min(input.find('\n', i), input.size());
            mark(i, end, ReplHighlight::Comment);
            i = end;
        }
        else if (std::isdigit(static_cast<unsigned char>(c)) != 0 && (i == 0 || !is_word(input[i - 1])))
        {
            auto end = i;
            while (end < input.size() && (is_word(input[end]) || input[end] == '.')) ++end;
            mark(i, end, ReplHighlight::Number);
            i = end;
        }
        else if (is_word(c))
        {
            auto end = i;
            while (end < input.size() && is_word(input[end])) ++end;
            const auto word = uppercase(input.substr(i, end - i));
            if (std::find(keywords.begin(), keywords.end(), word) != keywords.end())
                mark(i, end, ReplHighlight::Keyword);
            else if (std::find(functions.begin(), functions.end(), word) != functions.end())
                mark(i, end, ReplHighlight::Function);
            i = end;
        }
        else
        {
            ++i;
        }
    }
    return kinds;
}

void mldp_pvxs_driver::cli::QueryCommandPreparer::prepare(const mldp_pvxs_driver::config::Config& config) const
{
    query::QueryableFactory::instance().reset();
    if (!config.hasChild("queryable"))
    {
        throw std::runtime_error(
            "Missing 'queryable' configuration. " "Provide one or more queryable entries via -c/--config before 'query'.");
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

int mldp_pvxs_driver::cli::QueryRunner::run(const QueryCliOptions&                       options,
                                            const std::string_view                       sql,
                                            std::ostream&                                output,
                                            std::shared_ptr<query::QueryProgressTracker> progress,
                                            std::optional<std::size_t>                   viewport_width,
                                            const bool                                   print_stats,
                                            query::QueryStats*                           completed_stats,
                                            std::shared_ptr<query::QueryCancellation>    cancellation,
                                            std::shared_ptr<std::mutex>                  output_mutex,
                                            QueryContinuationRegistry*                   continuations,
                                            BatchConsumer                                batch_consumer) const
{
    const auto shard_trace = options.trace_shards ? std::make_shared<query::ShardTraceCollector>() : nullptr;
    const auto spill_dir = options.spill_dir.empty()
                               ? (std::filesystem::temp_directory_path() / "mldp-query-spill").string()
                               : options.spill_dir;
    auto       spill_file_system = std::make_shared<arrow::fs::LocalFileSystem>();
    auto       spill = std::make_shared<query::SpillManager>(spill_file_system, spill_dir);
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
        .cancellation = std::move(cancellation),
        .shard_trace = shard_trace,
    };

    bool       trace_rendered = false;
    const auto render_shard_trace = [&]()
    {
        if (!shard_trace || trace_rendered)
            return;
        trace_rendered = true;
        auto& trace_output = options.shard_trace_output ? *options.shard_trace_output : std::cerr;
        for (const auto& entry : shard_trace->entries())
        {
            const auto first_response = entry.first_response_at.time_since_epoch().count() == 0
                                            ? std::chrono::milliseconds{0}
                                            : std::chrono::duration_cast<std::chrono::milliseconds>(entry.first_response_at - entry.dispatched_at);
            const auto completed_at = entry.completed_at.time_since_epoch().count() == 0
                                          ? std::chrono::steady_clock::now()
                                          : entry.completed_at;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(completed_at - entry.dispatched_at);
            trace_output << "Shard trace #" << entry.first_response_sequence
                         << " window=" << entry.window_index
                         << " slice=" << entry.slice_index
                         << " shard=" << entry.shard_index
                         << " time=" << entry.begin_seconds << ".." << entry.end_seconds
                         << " first_batch_observed_ms=" << first_response.count()
                         << " terminal_observed_ms=" << elapsed.count()
                         << " batches=" << entry.batches
                         << " rows=" << entry.rows
                         << " pvs=";
            for (std::size_t index = 0; index < entry.pvs.size(); ++index)
            {
                if (index != 0)
                    trace_output << ',';
                trace_output << entry.pvs[index];
            }
            if (!entry.failure.empty())
                trace_output << " failure=" << entry.failure;
            trace_output << '\n';
        }
    };

    try
    {
        if (progress)
        {
            progress->setPhase(query::QueryProgressPhase::Parsing);
        }
        auto parsed = query::parseQuery(sql);
        if (progress)
        {
            progress->setPhase(query::QueryProgressPhase::Planning);
        }
        const query::QueryPlanner  planner(table_catalog_);
        const query::QueryExecutor executor;
        auto                       physical = planner.plan(parsed);
        const auto*                select = std::get_if<query::SelectStatement>(&parsed);
        if (select && select->page_token && continuations == nullptr)
            throw std::runtime_error("PAGE TOKEN p9 continuations are available only in a live REPL session");
        const bool                                      interactive_page = continuations != nullptr && select != nullptr && select->limit.has_value();
        const auto                                      fingerprint = interactive_page ? queryFingerprint(sql) : std::string{};
        std::optional<QueryContinuationRegistry::Entry> resumed;
        uint64_t                                        result_page = 1;
        if (interactive_page && select->page_token)
        {
            resumed.emplace(continuations->take(*select->page_token, fingerprint));
            context.cancellation = resumed->cancellation;
            result_page = resumed->result_page + 1;
        }
        if (progress && interactive_page)
            progress->setResultPage(result_page);
        std::optional<SpillCleanupGuard> cleanup;
        if (!interactive_page)
        {
            cleanup.emplace(SpillCleanupGuard{spill});
        }
        if (interactive_page)
        {
            // In the REPL, LIMIT is a page size rather than a row cap: the stream stays
            // live across continuation tokens. Drop the limit node and clear any row
            // limit the planner pushed into the scans, which would end the stream early.
            if (const auto* limit = std::get_if<query::plan::PhysicalLimit>(&physical->value))
                physical = limit->input;
            query::planner::propagateScanRowLimit(physical, std::nullopt);
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
        if (context.cancellation)
            context.cancellation->throwIfCancelled();
        {
            query::IRecordBatchStreamUPtr      stream;
            std::shared_ptr<query::QueryStats> stream_stats;
            InteractivePageStream*             page_stream = nullptr;
            if (resumed)
            {
                stream = std::move(resumed->stream);
                stream_stats = std::move(resumed->stats);
                page_stream = dynamic_cast<InteractivePageStream*>(stream.get());
                if (!page_stream)
                    throw std::runtime_error("PAGE TOKEN has incompatible continuation state");
                page_stream->reset(*select->limit);
            }
            else
            {
                auto streamed_result = executor.executeStream(physical, context);
                stream_stats = std::move(streamed_result.stats);
                if (interactive_page)
                {
                    stream = std::make_unique<InteractivePageStream>(std::move(streamed_result.stream), *select->limit);
                    page_stream = static_cast<InteractivePageStream*>(stream.get());
                }
                else
                {
                    stream = std::move(streamed_result.stream);
                }
            }
            if (batch_consumer)
            {
                while (auto batch = stream->next())
                {
                    if (context.cancellation) context.cancellation->throwIfCancelled();
                    batch_consumer(batch);
                    if (progress) progress->outputBatch(static_cast<uint64_t>(batch->num_rows()));
                }
            }
            else
            {
                formatQueryStream(*stream,
                                  options.format,
                                  output,
                                  options.expanded,
                                  TableRenderOptions{.viewport_width = options.table_fit ? table_width : std::nullopt, .color = options.color},
                                  context.cancellation,
                                  progress,
                                  output_mutex);
            }
            std::unique_lock lock(output_mutex ? *output_mutex : g_fallback_output_mutex);
            if (!options.no_stats && print_stats)
            {
                printQueryStats(*stream_stats, output);
            }
            if (completed_stats != nullptr)
            {
                *completed_stats = *stream_stats;
            }
            if (interactive_page && page_stream && page_stream->pageFull())
            {
                const auto token = continuations->store(QueryContinuationRegistry::Entry{
                    .fingerprint = fingerprint,
                    .stream = std::move(stream),
                    .stats = std::move(stream_stats),
                    .cancellation = context.cancellation,
                    .result_page = result_page});
                output << "-- continuation token: " << token << "\n";
            }
        }
        if (context.cancellation)
            context.cancellation->throwIfCancelled();
        if (progress)
        {
            progress->setPhase(query::QueryProgressPhase::Complete);
        }
        render_shard_trace();
        return 0;
    }
    catch (...)
    {
        render_shard_trace();
        throw;
    }
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

int mldp_pvxs_driver::cli::QueryCommand::run(int                             argc,
                                                char**                          argv,
                                                const std::vector<std::string>& global_config_sources,
                                                std::istream&                   input,
                                                std::ostream&                   output,
                                                std::ostream&                   error) const
{
    try
    {
        for (int index = 1; index < argc; ++index)
        {
            const auto arg = std::string_view{argv[index]};
            if (arg == "-h" || arg == "--help")
            {
                printQueryUsage(output);
                return 0;
            }
        }
        QueryCliOptions options;
        parseQueryArguments(argc, argv, options);
        std::ofstream shard_trace_file;
        if (!options.shard_trace_file.empty())
        {
            shard_trace_file.open(options.shard_trace_file, std::ios::out | std::ios::trunc);
            if (!shard_trace_file)
            {
                throw std::runtime_error("Unable to open shard trace file '" + options.shard_trace_file + "'");
            }
            options.shard_trace_output = &shard_trace_file;
        }
        else
        {
            options.shard_trace_output = &error;
        }
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
            QueryCommandPreparer preparer;
            preparer.prepare(config);
        }
        QueryRunner runner;
        if (interactive)
        {
            NullQueryCommandListener repl_listener;
            return runRepl(options, runner, repl_listener, input, output, error);
        }
        auto                        cancellation = std::make_shared<query::QueryCancellation>();
        ScopedQueryInterruptHandler interrupt_handler;
        auto                        query = std::async(std::launch::async,
                                                       [&]
                                                       {
                                    return runner.run(options, sql, output, nullptr, std::nullopt, true, nullptr, cancellation, nullptr);
                                                       });
        while (query.wait_for(std::chrono::milliseconds{100}) != std::future_status::ready)
        {
            if (interrupt_handler.consumeInterrupt())
            {
                cancellation->requestCancel();
            }
        }
        return query.get();
    }
    catch (const query::QueryCancelled&)
    {
        error << "Query cancelled\n";
        return 130;
    }
    catch (const std::exception& ex)
    {
        printQueryError(ex, error);
    }
    return 1;
}

mldp_pvxs_driver::cli::QueryCommand::QueryCommand(QueryCommandListener& listener, QueryablePreparer queryable_preparer)
    : listener_(listener)
    , queryable_preparer_(std::move(queryable_preparer))
{
}
