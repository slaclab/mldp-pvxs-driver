//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryCommand.h
 * @brief Declares the query CLI subcommand and interactive REPL support. */
#pragma once

#include <config/Config.h>
#include <query/QueryCommandListener.h>
#include <query/QueryFormatter.h>
#include <query/QueryStats.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <chrono>
#include <memory>

namespace mldp_pvxs_driver::query {
class QueryProgressTracker;
class QueryCancellation;
class QueryTableCatalog;
class IRecordBatchStream;
} // namespace mldp_pvxs_driver::query

namespace mldp_pvxs_driver::cli {

/** @brief Command-line configuration for one query invocation. */
struct QueryCliOptions
{
    std::string       sql{};                          ///< Inline SQL text from a positional CLI argument.
    std::string       sql_file{};                     ///< Path to a SQL file; empty = not used.
    QueryOutputFormat format{QueryOutputFormat::Table}; ///< Output encoding.
    bool              expanded{false};                ///< True to use expanded (vertical) table layout.
    bool              table_fit{false};               ///< True to constrain table width to the terminal width.
    bool              pager{false};                   ///< True to pipe table output through the system pager.
    bool              no_stats{false};                ///< True to suppress the statistics footer.
    bool              trace_shards{false};            ///< True to collect per-shard timing diagnostics.
    std::string       shard_trace_file{};             ///< Path for shard trace output; empty = write to stderr.
    std::ostream*     shard_trace_output{nullptr};    ///< Override output stream for shard trace; null uses shard_trace_file or stderr.
    uint64_t          memory_mb{256};                 ///< Arrow memory pool limit in mebibytes.
    std::string       spill_dir{};                    ///< Spill directory path; empty = system temp directory.
    std::string       table_catalog_dir{};            ///< Catalog persistence directory; empty = system temp directory.
    uint32_t          spill_partitions{16};           ///< Number of partitions for spill-based operations.
    uint32_t          join_batch_size{100};           ///< Maximum rows per join probe batch.
};

/** @brief Owns resumable query streams for interactive continuation tokens. */
class QueryContinuationRegistry
{
public:
    /** @brief Stream state retained for one continuation token. */
    struct Entry
    {
        std::string                                fingerprint;  ///< SQL fingerprint used to validate PAGE TOKEN continuations.
        std::unique_ptr<query::IRecordBatchStream> stream;       ///< Incomplete result stream held for resumption.
        std::shared_ptr<query::QueryStats>         stats;        ///< Shared statistics accumulator for the continued query.
        std::shared_ptr<query::QueryCancellation>  cancellation; ///< Cancellation token shared with the ongoing stream.
        uint64_t                                   result_page{1}; ///< 1-based page number already delivered to the user.
        std::chrono::steady_clock::time_point      expires_at;   ///< Time point after which this entry is eligible for cleanup.
    };

    /** @brief Constructs a registry with the given idle timeout for stored entries.
     * @param[in] idle_timeout  Duration after which unused entries are expired. */
    explicit QueryContinuationRegistry(std::chrono::steady_clock::duration idle_timeout = std::chrono::minutes{5});
    ~QueryContinuationRegistry();

    QueryContinuationRegistry(const QueryContinuationRegistry&) = delete;
    QueryContinuationRegistry& operator=(const QueryContinuationRegistry&) = delete;

    /** @brief Stores an entry and returns its continuation token string.
     * @param[in] entry  Entry to store; the token is generated internally.
     * @return PAGE TOKEN string the caller should display to the user. */
    std::string store(Entry entry);

    /** @brief Removes and returns a stored entry by token, validating its fingerprint.
     * @param[in] token        Token string previously returned by store().
     * @param[in] fingerprint  SQL fingerprint of the resuming query.
     * @return The stored entry.
     * @throws std::runtime_error  If the token is unknown, expired, or the fingerprint does not match. */
    Entry       take(const std::string& token, std::string_view fingerprint);

    /** @brief Removes all entries whose expiry time has passed. */
    void        cleanupExpired();

    /** @brief Removes all stored entries and cancels their streams. */
    void        clear();

private:
    std::chrono::steady_clock::duration    idle_timeout_;
    std::unordered_map<std::string, Entry> entries_;
};

/** @brief Registers queryable implementations required by the CLI. */
class QueryCommandPreparer
{
public:
    /** @brief Registers all queryable implementations required by the query command.
     * @param[in] config  Driver configuration. */
    void prepare(const config::Config& config) const;
};

/** @brief Executes parsed queries and formats their output for the CLI. */
class QueryRunner
{
public:
    using BatchConsumer = std::function<void(const std::shared_ptr<arrow::RecordBatch>&)>;

    /** @brief Parses, plans, executes, and formats one SQL statement.
     * @param[in]  options          CLI formatting and resource options.
     * @param[in]  sql              SQL text to execute.
     * @param[out] output           Destination for formatted output.
     * @param[in]  progress         Optional progress tracker.
     * @param[in]  viewport_width   Optional terminal width for table formatting.
     * @param[in]  print_stats      True to write statistics to output after the result.
     * @param[out] completed_stats  Optional pointer to receive the final statistics.
     * @param[in]  cancellation     Optional cancellation token.
     * @param[in]  output_mutex     Optional mutex for concurrent writes to output.
     * @param[in]  continuations    Optional registry for REPL paging continuations.
     * @param[in]  batch_consumer   Optional raw batch callback; when set, skips formatting.
     * @return 0 on success, 1 on execution error. */
    int                                       run(const QueryCliOptions&                       options,
                                                  std::string_view                             sql,
                                                  std::ostream&                                output,
                                                  std::shared_ptr<query::QueryProgressTracker> progress = nullptr,
                                                  std::optional<std::size_t>                   viewport_width = std::nullopt,
                                                  bool                                         print_stats = true,
                                                  query::QueryStats*                           completed_stats = nullptr,
                                                  std::shared_ptr<query::QueryCancellation>    cancellation = nullptr,
                                                  std::shared_ptr<std::mutex>                  output_mutex = nullptr,
                                                  QueryContinuationRegistry*                   continuations = nullptr,
                                                  BatchConsumer                                batch_consumer = {}) const;

    /** @brief Returns the catalog used to provide tab-completion candidates.
     * @param[in] options  Current CLI options.
     * @return Shared catalog pointer; may be null. */
    std::shared_ptr<query::QueryTableCatalog> completionCatalog(const QueryCliOptions& options) const;

private:
    mutable std::shared_ptr<query::QueryTableCatalog> table_catalog_;
    mutable std::string                               table_catalog_dir_;
};

/** @brief Implements the query command and its interactive REPL mode. */
class QueryCommand
{
public:
    using QueryablePreparer = std::function<void(const config::Config&)>;

    /** @brief Constructs the command with the given lifecycle listener and optional preparer.
     * @param[in,out] listener            Receives lifecycle events.
     * @param[in]     queryable_preparer  Optional override for registering queryable types. */
    explicit QueryCommand(QueryCommandListener& listener, QueryablePreparer queryable_preparer = {});

    /** @brief Parses arguments and either runs a one-shot query or starts the interactive REPL.
     * @param[in]     argc                 Argument count (argv[0] is the "query" subcommand name).
     * @param[in]     argv                 Argument vector.
     * @param[in]     global_config_sources Paths to global config files.
     * @param[in,out] input                Input stream for REPL; typically std::cin.
     * @param[out]    output               Output stream for results.
     * @param[out]    error                Output stream for error messages.
     * @return 0 on success, non-zero on error. */
    int run(int                             argc,
            char**                          argv,
            const std::vector<std::string>& global_config_sources,
            std::istream&                   input,
            std::ostream&                   output,
            std::ostream&                   error) const;

private:
    QueryCommandListener& listener_;
    QueryablePreparer queryable_preparer_;
};

namespace detail {

    /** @brief Returns REPL tab-completion candidates for text preceding the cursor.
     * @param[in] input  Text entered so far.
     * @return Candidate strings. */
    std::vector<std::string> replCompletions(std::string_view input);

    /** @brief Returns REPL tab-completion candidates using the active session catalog.
     * @param[in] input          Text entered so far.
     * @param[in] table_catalog  Catalog for user-created table names.
     * @return Candidate strings. */
    std::vector<std::string> replCompletions(std::string_view input, const std::shared_ptr<query::QueryTableCatalog>& table_catalog);

    /** @brief Returns the token length replxx should replace for a given completion.
     * @param[in] input  Text entered so far.
     * @return Character count of the token to replace. */
    int replCompletionContextLength(std::string_view input);

} // namespace detail

} // namespace mldp_pvxs_driver::cli
