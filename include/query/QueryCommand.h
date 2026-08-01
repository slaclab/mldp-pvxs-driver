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
    std::string       sql{};
    std::string       sql_file{};
    QueryOutputFormat format{QueryOutputFormat::Table};
    bool              expanded{false};
    bool              table_fit{false};
    bool              pager{false};
    bool              no_stats{false};
    bool              trace_shards{false};
    std::string       shard_trace_file{};
    std::ostream*     shard_trace_output{nullptr};
    uint64_t          memory_mb{256};
    std::string       spill_dir{};
    std::string       table_catalog_dir{};
    uint32_t          spill_partitions{16};
    uint32_t          join_batch_size{100};
};

/** Live-REPL ownership for an incomplete interactive query. */
/** @brief Owns resumable query streams for interactive continuation tokens. */
class QueryContinuationRegistry
{
public:
    /** @brief Stream state retained for one continuation token. */
    struct Entry
    {
        std::string                                fingerprint;
        std::unique_ptr<query::IRecordBatchStream> stream;
        std::shared_ptr<query::QueryStats>         stats;
        std::shared_ptr<query::QueryCancellation>  cancellation;
        uint64_t                                   result_page{1};
        std::chrono::steady_clock::time_point      expires_at;
    };

    explicit QueryContinuationRegistry(std::chrono::steady_clock::duration idle_timeout = std::chrono::minutes{5});
    ~QueryContinuationRegistry();

    QueryContinuationRegistry(const QueryContinuationRegistry&) = delete;
    QueryContinuationRegistry& operator=(const QueryContinuationRegistry&) = delete;

    std::string store(Entry entry);
    Entry       take(const std::string& token, std::string_view fingerprint);
    void        cleanupExpired();
    void        clear();

private:
    std::chrono::steady_clock::duration    idle_timeout_;
    std::unordered_map<std::string, Entry> entries_;
};

/** @brief Registers queryable implementations required by the CLI. */
class QueryCommandPreparer
{
public:
    void prepare(const config::Config& config) const;
};

/** @brief Executes parsed queries and formats their output for the CLI. */
class QueryRunner
{
public:
    using BatchConsumer = std::function<void(const std::shared_ptr<arrow::RecordBatch>&)>;

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

    explicit QueryCommand(QueryCommandListener& listener, QueryablePreparer queryable_preparer = {});

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

    /** Return interactive REPL completion candidates for text before the cursor. */
    std::vector<std::string> replCompletions(std::string_view input);

    /** Return completion candidates using the current session and persistent table catalog. */
    std::vector<std::string> replCompletions(std::string_view input, const std::shared_ptr<query::QueryTableCatalog>& table_catalog);

    /** Return the ASCII token length replxx should replace for a completion. */
    int replCompletionContextLength(std::string_view input);

} // namespace detail

} // namespace mldp_pvxs_driver::cli
