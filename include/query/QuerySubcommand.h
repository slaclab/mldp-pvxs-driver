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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <memory>

namespace mldp_pvxs_driver::query {
class QueryProgressTracker;
class QueryTableCatalog;
}

namespace mldp_pvxs_driver::cli {

struct QueryCliOptions {
    std::string       sql{};
    std::string       sql_file{};
    QueryOutputFormat format{QueryOutputFormat::Table};
    bool              expanded{false};
    bool              table_fit{false};
    bool              no_stats{false};
    uint64_t    memory_mb{256};
    std::string spill_dir{};
    std::string table_catalog_dir{};
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
    int run(const QueryCliOptions& options,
            std::string_view       sql,
            std::ostream&          output,
            std::shared_ptr<query::QueryProgressTracker> progress = nullptr,
            std::optional<std::size_t> viewport_width = std::nullopt,
            bool                       print_stats = true,
            query::QueryStats*         completed_stats = nullptr) const;
    std::shared_ptr<query::QueryTableCatalog> completionCatalog(const QueryCliOptions& options) const;

private:
    mutable std::shared_ptr<query::QueryTableCatalog> table_catalog_;
    mutable std::string                               table_catalog_dir_;
};

class QuerySubcommand
{
public:
    using QueryablePreparer = std::function<void(const config::Config&)>;

    explicit QuerySubcommand(QueryablePreparer queryable_preparer = {});

    int run(int argc,
            char** argv,
            const std::vector<std::string>& global_config_sources,
            std::istream&                     input,
            std::ostream&                    output,
            std::ostream&                    error) const;

private:
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
