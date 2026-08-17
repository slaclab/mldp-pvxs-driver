//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file ExecutorUtils.h
 * @brief Declares shared Arrow and predicate helpers used by query execution. */
#pragma once

#include <query/ExecutionContext.h>
#include <query/executor/ExecutionState.h>
#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/plan/PhysicalPlan.h>

#include <arrow/record_batch.h>

#include <set>
#include <string_view>

namespace mldp_pvxs_driver::query::executor {

/** @brief Formats a set of predicate operators as a comma-separated string.
 * @param[in] ops Set of operators.
 * @return Formatted string. */
std::string joinOps(const std::set<PredicateOp>& ops);
/** @brief Returns the display name for a logical column type.
 * @param[in] type Column type.
 * @return Short string label. */
std::string_view columnTypeName(ColumnType type);
/** @brief Infers the logical column type from an Arrow data type.
 * @param[in] type Arrow data type.
 * @return Matching ColumnType.
 * @throws std::runtime_error If no mapping exists. */
ColumnType columnTypeFromArrow(const std::shared_ptr<arrow::DataType>& type);
/** @brief Returns true if value matches the SQL LIKE pattern.
 * @param[in] value String to test.
 * @param[in] pattern LIKE pattern (% and _ wildcards).
 * @return True on match. */
bool matchesLikePattern(std::string_view value, std::string_view pattern);
/** @brief Extracts membership values from subquery result batches.
 * @param[in] batches Subquery output batches.
 * @param[in] target_type Expected logical type for the extracted values.
 * @param[in] target_column Column name to extract values from.
 * @return Vector of ExecutableLiteralValue. */
std::vector<ExecutableLiteralValue> extractInSubqueryValues(const RecordBatches& batches, ColumnType target_type, std::string_view target_column);
/** @brief Extracts and normalizes [begin, end] time window pairs from result batches.
 * @param[in] batches Result batches with begin/end timestamp columns.
 * @return Ordered vector of [begin_ns, end_ns] pairs. */
std::vector<std::pair<int64_t, int64_t>> extractNormalizedWindows(const RecordBatches& batches);
/** @brief Applies a list of predicates to a single batch.
 * @param[in] batch Input batch.
 * @param[in] predicates Predicates to evaluate.
 * @return Arrow Result with the filtered batch. */
arrow::Result<std::shared_ptr<arrow::RecordBatch>> applyFilter(const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<Predicate>& predicates);
/** @brief Projects named columns from a batch vector.
 * @param[in] input Input batches.
 * @param[in] columns Column names to retain in order.
 * @return Projected batches. */
RecordBatches applyProjection(const RecordBatches& input, const std::vector<std::string>& columns);
/** @brief Evaluates computed expressions over a batch vector and assigns output names.
 * @param[in] input Input batches.
 * @param[in] expressions Expressions to evaluate.
 * @param[in] names Output column names.
 * @return Batches with computed columns. */
RecordBatches applyProjection(const RecordBatches& input, const std::vector<ExpressionPtr>& expressions, const std::vector<std::string>& names);
/** @brief Truncates a batch vector to at most limit rows.
 * @param[in] input Input batches.
 * @param[in] limit Maximum rows.
 * @return Truncated batches. */
RecordBatches applyLimit(const RecordBatches& input, uint64_t limit);
/** @brief Sorts all rows across a batch vector by the given sort keys.
 * @param[in] input Input batches.
 * @param[in] keys Sort keys and directions.
 * @return Sorted batches. */
RecordBatches applySort(const RecordBatches& input, const std::vector<plan::SortKey>& keys);
/** @brief Concatenates a batch vector into a single record batch.
 * @param[in] batches Input batches.
 * @return Single combined batch, or nullptr if input is empty. */
std::shared_ptr<arrow::RecordBatch> combineBatches(const RecordBatches& batches);
/** @brief Prefixes all column names in a batch with the given table alias.
 * @param[in] batch Input batch.
 * @param[in] alias Alias to prepend (e.g. "t1.column").
 * @return Batch with renamed columns. */
std::shared_ptr<arrow::RecordBatch> qualifyBatchColumns(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& alias);
/** @brief Joins two batches on an equi-join key.
 * @param[in] left Left-side input.
 * @param[in] right Right-side input.
 * @param[in] left_key Left join column name.
 * @param[in] right_key Right join column name.
 * @param[in] type INNER or LEFT_OUTER.
 * @param[in] context Execution context.
 * @param[in,out] stats Statistics accumulator.
 * @return Joined record batch. */
std::shared_ptr<arrow::RecordBatch> joinBatches(const std::shared_ptr<arrow::RecordBatch>& left, const std::shared_ptr<arrow::RecordBatch>& right, const std::string& left_key, const std::string& right_key, plan::JoinType type, const ExecutionContext& context, QueryStats& stats);

} // namespace mldp_pvxs_driver::query::executor
