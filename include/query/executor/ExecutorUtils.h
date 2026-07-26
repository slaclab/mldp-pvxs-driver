//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

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

std::string joinOps(const std::set<PredicateOp>& ops);
std::string_view columnTypeName(ColumnType type);
ColumnType columnTypeFromArrow(const std::shared_ptr<arrow::DataType>& type);
std::vector<ExecutableLiteralValue> extractInSubqueryValues(const RecordBatches& batches, ColumnType target_type, std::string_view target_column);
std::vector<std::pair<int64_t, int64_t>> extractNormalizedWindows(const RecordBatches& batches);
arrow::Result<std::shared_ptr<arrow::RecordBatch>> applyFilter(const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<Predicate>& predicates);
RecordBatches applyProjection(const RecordBatches& input, const std::vector<std::string>& columns);
RecordBatches applyProjection(const RecordBatches& input, const std::vector<ExpressionPtr>& expressions, const std::vector<std::string>& names);
RecordBatches applyLimit(const RecordBatches& input, uint64_t limit);
RecordBatches applySort(const RecordBatches& input, const std::vector<plan::SortKey>& keys);
std::shared_ptr<arrow::RecordBatch> combineBatches(const RecordBatches& batches);
std::shared_ptr<arrow::RecordBatch> qualifyBatchColumns(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& alias);
std::shared_ptr<arrow::RecordBatch> joinBatches(const std::shared_ptr<arrow::RecordBatch>& left, const std::shared_ptr<arrow::RecordBatch>& right, const std::string& left_key, const std::string& right_key, plan::JoinType type, const ExecutionContext& context, QueryStats& stats);

} // namespace mldp_pvxs_driver::query::executor
