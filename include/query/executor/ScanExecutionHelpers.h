//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/ExecutionContext.h>
#include <query/QueryStats.h>
#include <query/executor/ExecutionState.h>
#include <query/plan/PhysicalPlan.h>

#include <arrow/record_batch.h>

namespace mldp_pvxs_driver::query::executor {

RecordBatches readCatalogTable(const plan::PhysicalTableScan& scan, const ExecutionContext& context);
RecordBatches fetchBackendPages(const plan::PhysicalTableScan& scan,
                                const std::vector<Predicate>& pushable,
                                const std::vector<Predicate>& local,
                                const ExecutionContext& context,
                                QueryStats& stats);
RecordBatches fetchTimeSeriesWindows(const plan::PhysicalTableScan& scan,
                                     const std::vector<Predicate>& pushable,
                                     const std::vector<Predicate>& local,
                                     const std::vector<std::pair<int64_t, int64_t>>& windows,
                                     const ExecutionContext& context,
                                     QueryStats& stats);

} // namespace mldp_pvxs_driver::query::executor
