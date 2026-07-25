//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/QueryExecutor.h>

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>

namespace mldp_pvxs_driver::cli {

enum class QueryOutputFormat
{
    Table,
    Json,
    Csv,
    Arrow
};

struct TableRenderOptions
{
    std::optional<std::size_t> viewport_width{};
};

void formatQueryResult(const query::QueryExecutionResult& result,
                       QueryOutputFormat                  format,
                       std::ostream&                      output,
                       bool                               expanded = false,
                       const TableRenderOptions&          table_options = {});

void printQueryStats(const query::QueryStats& stats, std::ostream& output);

std::string queryStatsLine(const query::QueryStats& stats);

} // namespace mldp_pvxs_driver::cli
