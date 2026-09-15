//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file ScanExecutionHelpers.h
 * @brief Declares helpers shared by backend and window scan execution. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/QueryStats.h>
#include <query/executor/ExecutionState.h>
#include <query/plan/PhysicalPlan.h>

#include <arrow/record_batch.h>

namespace mldp_pvxs_driver::query::executor {

/** @brief Reads all batches from a physical catalog table scan.
 * @param[in] scan Scan descriptor identifying the catalog table.
 * @param[in] context Execution context for catalog access.
 * @return Materialized batches. */
RecordBatches readCatalogTable(const plan::PhysicalTableScan& scan, const ExecutionContext& context);
/** @brief Fetches all pages from a backend scan, applying local predicates.
 * @param[in] scan Physical scan descriptor.
 * @param[in] pushable Predicates forwarded to the backend.
 * @param[in] local Residual predicates applied locally.
 * @param[in] context Execution context.
 * @param[in,out] stats Statistics accumulator.
 * @return Materialized batches. */
RecordBatches fetchBackendPages(const plan::PhysicalTableScan& scan,
                                const std::vector<Predicate>& pushable,
                                const std::vector<Predicate>& local,
                                const ExecutionContext& context,
                                QueryStats& stats);
/** @brief Fetches windowed time-series shards serially and applies local predicates.
 * @details Falls back to a serial scan; use WindowBackendScanRecordBatchStream for parallel execution.
 * @param[in] scan Physical scan descriptor.
 * @param[in] pushable Predicates forwarded to the backend.
 * @param[in] local Residual predicates applied locally.
 * @param[in] windows Ordered [begin_ns, end_ns] window pairs.
 * @param[in] window_shards Slice duration and series-per-shard settings.
 * @param[in] context Execution context.
 * @param[in,out] stats Statistics accumulator.
 * @return Materialized batches. */
RecordBatches fetchTimeSeriesWindows(const plan::PhysicalTableScan& scan,
                                     const std::vector<Predicate>& pushable,
                                     const std::vector<Predicate>& local,
                                     const std::vector<std::pair<int64_t, int64_t>>& windows,
                                     const plan::WindowShardSpec& window_shards,
                                     const ExecutionContext& context,
                                     QueryStats& stats);
/** @brief Pivots long-form batches into a wide layout using spill-backed merge sort.
 * @param[in] long_batches Long-form input batches.
 * @param[in] row_key_column Column used as the output row key.
 * @param[in] pivot_key_column Column whose values become output column names.
 * @param[in] value_column Column providing cell values.
 * @param[in] output_column_labels Ordered output column labels.
 * @param[in] output_batch_size Maximum output rows per batch.
 * @param[in] context Execution context for spill and memory.
 * @param[in,out] stats Statistics accumulator.
 * @return Wide output batches. */
RecordBatches pivotLongBatchesWithSpill(const RecordBatches& long_batches,
                                        std::string_view row_key_column,
                                        std::string_view pivot_key_column,
                                        std::string_view value_column,
                                        const std::vector<std::string>& output_column_labels,
                                        uint32_t output_batch_size,
                                        const ExecutionContext& context,
                                        QueryStats& stats);
/** @brief Drains a long-form pull stream and pivots it with spill-backed merge sort.
 * @param[in,out] long_stream Long-form input stream; fully drained.
 * @param[in] row_key_column Row key column name.
 * @param[in] pivot_key_column Pivot key column name.
 * @param[in] value_column Value column name.
 * @param[in] output_column_labels Ordered output labels.
 * @param[in] output_batch_size Maximum output rows per batch.
 * @param[in] context Execution context.
 * @param[in,out] stats Statistics accumulator.
 * @return Wide output batches. */
RecordBatches pivotLongStreamWithSpill(IRecordBatchStream& long_stream,
                                       std::string_view row_key_column,
                                       std::string_view pivot_key_column,
                                       std::string_view value_column,
                                       const std::vector<std::string>& output_column_labels,
                                       uint32_t output_batch_size,
                                       const ExecutionContext& context,
                                       QueryStats& stats);

} // namespace mldp_pvxs_driver::query::executor
