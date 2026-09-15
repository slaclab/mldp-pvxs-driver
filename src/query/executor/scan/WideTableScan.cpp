//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/executor/ExecutorUtils.h>
#include <query/QueryCancellation.h>
#include <query/executor/ScanExecutionHelpers.h>

#include <query/QueryProgress.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>

#include <arrow/array.h>
#include <arrow/array/data.h>
#include <arrow/builder.h>
#include <arrow/compute/api.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/scalar.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

namespace {

std::shared_ptr<arrow::Scalar> activeValue(std::shared_ptr<arrow::Scalar> value)
{
    while (value && value->is_valid &&
           (value->type->id() == arrow::Type::DENSE_UNION || value->type->id() == arrow::Type::SPARSE_UNION))
    {
        const auto union_value = std::dynamic_pointer_cast<arrow::UnionScalar>(value);
        if (!union_value) break;
        value = union_value->child_value();
    }
    return value;
}

std::shared_ptr<arrow::RecordBatch> canonicalizeLongPivotBatch(
    const std::shared_ptr<arrow::RecordBatch>& projected,
    const std::shared_ptr<arrow::Schema>& canonical_schema)
{
    if (!projected || projected->num_columns() != 3)
        throw std::runtime_error("Wide pivot canonicalization requires pv, time, and value columns");

    const auto& pv = projected->column(0);
    const auto& time = projected->column(1);
    const auto& value = projected->column(2);
    if (pv->type_id() != arrow::Type::STRING || time->type_id() != arrow::Type::TIMESTAMP)
    {
        throw std::runtime_error("Wide pivot canonicalization requires string pv and timestamp time columns");
    }
    if (!canonical_schema)
    {
        return arrow::RecordBatch::Make(
            arrow::schema({arrow::field("pv", pv->type()), arrow::field("time", time->type()), arrow::field("value", value->type())}),
            projected->num_rows(), {pv, time, value});
    }
    if (!pv->type()->Equals(*canonical_schema->field(0)->type()) ||
        !time->type()->Equals(*canonical_schema->field(1)->type()) ||
        !value->type()->Equals(*canonical_schema->field(2)->type()))
    {
        throw std::runtime_error("Wide pivot canonicalization found incompatible pv, time, or value types between backend batches");
    }
    return arrow::RecordBatch::Make(canonical_schema, projected->num_rows(), {pv, time, value});
}

RecordBatches readSortedPivotRuns(const SpillHandle& long_spill,
                                  const std::shared_ptr<SpillManager>& spill_manager,
                                  const std::string_view row_key_column,
                                  const std::string_view pivot_key_column,
                                  const std::string_view value_column,
                                  const std::vector<std::string>& output_column_labels,
                                  const std::unordered_map<std::string, std::shared_ptr<arrow::DataType>>& value_types,
                                  const ExecutionContext& context,
                                  QueryStats& stats)
{
    auto reader_result = spill_manager->read(long_spill);
    if (!reader_result.ok()) throw std::runtime_error(reader_result.status().ToString());
    auto reader = std::move(*reader_result);
    std::vector<SpillHandle> runs;
    while (true)
    {
        if (context.cancellation) context.cancellation->throwIfCancelled();
        auto next = reader.next();
        if (!next.ok()) throw std::runtime_error(next.status().ToString());
        if (!*next) break;
        const auto& batch = *next;
        const auto row_key_index = batch->schema()->GetFieldIndex(std::string(row_key_column));
        if (row_key_index < 0) throw std::runtime_error("Physical pivot input has no row-key column");
        const auto row_keys = std::dynamic_pointer_cast<arrow::TimestampArray>(batch->column(row_key_index));
        if (!row_keys) throw std::runtime_error("Physical pivot row-key column must be a timestamp");
        std::vector<int64_t> row_order(static_cast<std::size_t>(batch->num_rows()));
        std::iota(row_order.begin(), row_order.end(), 0);
        std::stable_sort(row_order.begin(), row_order.end(), [&row_keys](const int64_t left, const int64_t right) {
            return row_keys->Value(left) < row_keys->Value(right);
        });
        arrow::Int64Builder indices;
        if (!indices.AppendValues(row_order).ok()) throw std::runtime_error("Failed to build physical pivot sort indices");
        std::shared_ptr<arrow::Array> index_array;
        if (!indices.Finish(&index_array).ok()) throw std::runtime_error("Failed to finish physical pivot sort indices");
        std::vector<std::shared_ptr<arrow::Array>> sorted_columns;
        sorted_columns.reserve(batch->num_columns());
        for (const auto& column : batch->columns())
        {
            if (column->type_id() == arrow::Type::DENSE_UNION || column->type_id() == arrow::Type::SPARSE_UNION)
            {
                std::unique_ptr<arrow::ArrayBuilder> builder;
                const auto builder_status = arrow::MakeBuilder(context.pool != nullptr ? context.pool : arrow::default_memory_pool(), column->type(), &builder);
                if (!builder_status.ok()) throw std::runtime_error(builder_status.ToString());
                const arrow::ArraySpan source_span(*column->data());
                for (const auto row : row_order)
                {
                    const auto append_status = builder->AppendArraySlice(source_span, row, 1);
                    if (!append_status.ok()) throw std::runtime_error(append_status.ToString());
                }
                std::shared_ptr<arrow::Array> sorted;
                const auto finish_status = builder->Finish(&sorted);
                if (!finish_status.ok()) throw std::runtime_error(finish_status.ToString());
                sorted_columns.push_back(std::move(sorted));
                continue;
            }
            auto taken = arrow::compute::Take(column, index_array);
            if (!taken.ok()) throw std::runtime_error(taken.status().ToString());
            sorted_columns.push_back(taken->make_array());
        }
        const auto sorted = arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), std::move(sorted_columns));
        auto writer_result = spill_manager->openWriter("wide-sort", sorted->schema());
        if (!writer_result.ok()) throw std::runtime_error(writer_result.status().ToString());
        auto writer = std::move(*writer_result);
        const auto append_status = writer.append(sorted);
        if (!append_status.ok()) throw std::runtime_error(append_status.ToString());
        auto run = writer.finish();
        if (!run.ok()) throw std::runtime_error(run.status().ToString());
        stats.bytes_spilled += static_cast<uint64_t>(run->byte_count);
        ++stats.materialized_files;
        stats.materialized_bytes += static_cast<uint64_t>(run->byte_count);
        runs.push_back(*run);
    }
    struct Cursor {
        SpillReader reader;
        std::shared_ptr<arrow::RecordBatch> batch;
        int64_t row{0};
    };
    std::vector<Cursor> cursors;
    cursors.reserve(runs.size());
    for (const auto& run : runs)
    {
        auto run_reader_result = spill_manager->read(run);
        if (!run_reader_result.ok()) throw std::runtime_error(run_reader_result.status().ToString());
        Cursor cursor{.reader = std::move(*run_reader_result)};
        auto next = cursor.reader.next();
        if (!next.ok()) throw std::runtime_error(next.status().ToString());
        cursor.batch = std::move(*next);
        if (cursor.batch) cursors.push_back(std::move(cursor));
    }

    auto* pool = context.pool != nullptr ? context.pool : arrow::default_memory_pool();
    std::vector<std::shared_ptr<arrow::Field>> fields{arrow::field(std::string(row_key_column), arrow::timestamp(arrow::TimeUnit::NANO, "UTC"))};
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    builders.reserve(output_column_labels.size());
    for (const auto& label : output_column_labels)
    {
        const auto type = value_types.contains(label) ? value_types.at(label) : arrow::null();
        std::unique_ptr<arrow::ArrayBuilder> builder;
        const auto status = arrow::MakeBuilder(pool, type, &builder);
        if (!status.ok()) throw std::runtime_error(status.ToString());
        fields.push_back(arrow::field(label, type));
        builders.push_back(std::move(builder));
    }
    const auto schema = arrow::schema(fields);
    arrow::TimestampBuilder row_key_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool);
    RecordBatches output;
    const auto flush = [&]()
    {
        if (row_key_builder.length() == 0) return;
        std::shared_ptr<arrow::Array> row_keys;
        if (!row_key_builder.Finish(&row_keys).ok()) throw std::runtime_error("Failed to finish physical pivot row-key batch");
        std::vector<std::shared_ptr<arrow::Array>> arrays{row_keys};
        for (auto& builder : builders)
        {
            std::shared_ptr<arrow::Array> values;
            const auto status = builder->Finish(&values);
            if (!status.ok()) throw std::runtime_error(status.ToString());
            arrays.push_back(std::move(values));
        }
        output.push_back(arrow::RecordBatch::Make(schema, row_keys->length(), std::move(arrays)));
    };
    const auto advance = [&](Cursor& cursor)
    {
        ++cursor.row;
        if (cursor.row < cursor.batch->num_rows()) return;
        auto next = cursor.reader.next();
        if (!next.ok()) throw std::runtime_error(next.status().ToString());
        cursor.batch = std::move(*next);
        cursor.row = 0;
    };
    while (!cursors.empty())
    {
        if (context.cancellation) context.cancellation->throwIfCancelled();
        int64_t minimum_row_key = std::numeric_limits<int64_t>::max();
        for (const auto& cursor : cursors)
        {
            const auto row_keys = std::static_pointer_cast<arrow::TimestampArray>(cursor.batch->GetColumnByName(std::string(row_key_column)));
            if (!row_keys || row_keys->IsNull(cursor.row)) throw std::runtime_error("Physical pivot received a null row key");
            minimum_row_key = std::min(minimum_row_key, row_keys->Value(cursor.row));
        }
        std::unordered_map<std::string, std::shared_ptr<arrow::Scalar>> cells;
        for (std::size_t index = 0; index < cursors.size();)
        {
            auto& cursor = cursors[index];
            const auto row_keys = std::static_pointer_cast<arrow::TimestampArray>(cursor.batch->GetColumnByName(std::string(row_key_column)));
            if (!row_keys || row_keys->IsNull(cursor.row)) throw std::runtime_error("Physical pivot received a null row key");
            if (row_keys->Value(cursor.row) != minimum_row_key)
            {
                ++index;
                continue;
            }
            const auto pivot_keys = std::static_pointer_cast<arrow::StringArray>(cursor.batch->GetColumnByName(std::string(pivot_key_column)));
            if (!pivot_keys || pivot_keys->IsNull(cursor.row)) throw std::runtime_error("Physical pivot received a null pivot key");
            const auto pivot_key = pivot_keys->GetString(cursor.row);
            if (std::find(output_column_labels.begin(), output_column_labels.end(), pivot_key) == output_column_labels.end())
                throw std::runtime_error("Physical pivot received an unexpected output-column label '" + pivot_key + "'");
            auto scalar = cursor.batch->GetColumnByName(std::string(value_column))->GetScalar(cursor.row);
            if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
            if (!cells.emplace(pivot_key, activeValue(*scalar)).second)
                throw std::runtime_error("Physical pivot received a duplicate cell at row key " + std::to_string(minimum_row_key));
            advance(cursor);
            if (!cursor.batch) cursors.erase(cursors.begin() + static_cast<std::ptrdiff_t>(index));
        }
        if (!row_key_builder.Append(minimum_row_key).ok()) throw std::runtime_error("Failed to append physical pivot row key");
        for (std::size_t index = 0; index < output_column_labels.size(); ++index)
        {
            const auto found = cells.find(output_column_labels[index]);
            const auto status = found == cells.end() || !found->second || !found->second->is_valid
                ? builders[index]->AppendNull()
                : builders[index]->AppendScalar(*found->second);
            if (!status.ok()) throw std::runtime_error(status.ToString());
        }
        if (row_key_builder.length() == 4096) flush();
    }
    flush();
    return output;
}

} // namespace

RecordBatches mldp_pvxs_driver::query::executor::pivotLongStreamWithSpill(
    IRecordBatchStream& long_stream, const std::string_view row_key_column,
    const std::string_view pivot_key_column, const std::string_view value_column,
    const std::vector<std::string>& output_column_labels,
    const uint32_t output_batch_size, const ExecutionContext& context, QueryStats& stats)
{
    static_cast<void>(output_batch_size);
    const bool has_fixed_output_labels = !output_column_labels.empty();
    std::vector<std::string> output_labels = output_column_labels;

    auto spill_manager = context.spill;
    if (!spill_manager)
    {
        spill_manager = std::make_shared<SpillManager>(
            std::make_shared<arrow::fs::LocalFileSystem>(),
            (std::filesystem::temp_directory_path() / "mldp-query-spill").string());
    }

    std::optional<SpillWriter> writer;
    std::shared_ptr<arrow::Schema> canonical_schema;
    std::unordered_map<std::string, std::shared_ptr<arrow::DataType>> value_types;
    uint64_t ingestion_batch_index = 0;
    while (auto batch = long_stream.next())
    {
        ++ingestion_batch_index;
        if (context.progress) context.progress->setActivity("mldp.time_series_table", "wide pivot ingestion");
        if (context.cancellation) context.cancellation->throwIfCancelled();
        if (batch->num_rows() == 0) continue;
        const auto row_key_index = batch->schema()->GetFieldIndex(std::string(row_key_column));
        const auto pivot_key_index = batch->schema()->GetFieldIndex(std::string(pivot_key_column));
        const auto value_index = batch->schema()->GetFieldIndex(std::string(value_column));
        if (row_key_index < 0 || pivot_key_index < 0 || value_index < 0)
            throw std::runtime_error("Physical pivot input does not contain the configured columns");
        const auto row_keys = std::dynamic_pointer_cast<arrow::TimestampArray>(batch->column(row_key_index));
        if (!row_keys) throw std::runtime_error("Physical pivot row-key column must be a timestamp");
        const auto pivot_keys = std::dynamic_pointer_cast<arrow::StringArray>(batch->column(pivot_key_index));
        if (!pivot_keys) throw std::runtime_error("Physical pivot key column must be a string");
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            if (row_keys->IsNull(row)) throw std::runtime_error("Physical pivot received a null row key");
            if (pivot_keys->IsNull(row)) throw std::runtime_error("Physical pivot received a null pivot key");
            const auto pivot_key = pivot_keys->GetString(row);
            if (has_fixed_output_labels && std::find(output_labels.begin(), output_labels.end(), pivot_key) == output_labels.end())
                throw std::runtime_error("Physical pivot received an unexpected output-column label '" + pivot_key + "'");
            if (std::find(output_labels.begin(), output_labels.end(), pivot_key) == output_labels.end())
                output_labels.push_back(pivot_key);
            auto scalar = batch->column(value_index)->GetScalar(row);
            if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
            auto value = activeValue(*scalar);
            if (!value || !value->is_valid) continue;
            if (const auto found = value_types.find(pivot_key); found == value_types.end()) value_types.emplace(pivot_key, value->type);
            else if (!found->second->Equals(*value->type))
                throw std::runtime_error("Physical pivot received mixed value types for output-column label '" + pivot_key + "'");
        }
        const auto projected = applyProjection(RecordBatches{batch},
                                               {std::string(pivot_key_column), std::string(row_key_column), std::string(value_column)}).front();
        const auto canonical = canonicalizeLongPivotBatch(projected, canonical_schema);
        if (!canonical_schema) canonical_schema = canonical->schema();
        if (canonical->num_rows() == 0) continue;
        if (!writer)
        {
            auto opened = spill_manager->openWriter("wide-long", canonical_schema);
            if (!opened.ok()) throw std::runtime_error("Wide pivot spill writer open failed: " + opened.status().ToString());
            writer.emplace(std::move(*opened));
        }
        const auto status = writer->append(canonical);
        if (!status.ok())
            throw std::runtime_error("Wide pivot spill append failed for ingestion batch " + std::to_string(ingestion_batch_index) +
                                     " (" + std::to_string(canonical->num_rows()) + " rows): " + status.ToString());
    }
    if (!writer) return {};
    if (context.progress) context.progress->setActivity("mldp.time_series_table", "wide pivot", "spill finalization");
    auto spilled = writer->finish();
    if (!spilled.ok()) throw std::runtime_error("Wide pivot spill finalization failed: " + spilled.status().ToString());
    stats.bytes_spilled += static_cast<uint64_t>(spilled->byte_count);
    ++stats.materialized_files;
    stats.materialized_bytes += static_cast<uint64_t>(spilled->byte_count);
    if (context.progress) context.progress->setActivity("mldp.time_series_table", "wide pivot", "external sort and merge");
    return readSortedPivotRuns(*spilled, spill_manager, row_key_column, pivot_key_column, value_column,
                               output_labels, value_types, context, stats);
}

RecordBatches mldp_pvxs_driver::query::executor::pivotLongBatchesWithSpill(
    const RecordBatches& long_batches, const std::string_view row_key_column,
    const std::string_view pivot_key_column, const std::string_view value_column,
    const std::vector<std::string>& output_column_labels,
    const uint32_t output_batch_size, const ExecutionContext& context, QueryStats& stats)
{
    class BatchStream final : public IRecordBatchStream
    {
    public:
        explicit BatchStream(const RecordBatches& batches) : batches_(batches) {}
        std::shared_ptr<arrow::RecordBatch> next() override
        {
            return index_ < batches_.size() ? batches_[index_++] : nullptr;
        }
    private:
        const RecordBatches& batches_;
        std::size_t index_{0};
    };
    BatchStream stream(long_batches);
    return pivotLongStreamWithSpill(stream, row_key_column, pivot_key_column, value_column,
                                    output_column_labels, output_batch_size, context, stats);
}

RecordBatches mldp_pvxs_driver::query::executor::fetchTimeSeriesWindows(const plan::PhysicalTableScan& scan,
                                     const std::vector<Predicate>& pushable,
                                     const std::vector<Predicate>& local,
                                     const std::vector<std::pair<int64_t, int64_t>>& windows,
                                     const plan::WindowShardSpec& window_shards,
                                     const ExecutionContext& context,
                                     QueryStats& stats)
{
    auto queryable = QueryableFactory::instance().createByTable(scan.table_name);
    RecordBatches output;
    std::vector<std::string> requested_pvs;
    for (const auto& predicate : pushable)
    {
        if (predicate.column != "pv" || (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN)) continue;
        for (const auto& value : predicate.values)
            if (std::holds_alternative<std::string>(value)) requested_pvs.push_back(std::get<std::string>(value));
    }
    if (requested_pvs.empty()) throw std::runtime_error("MLDP time-series window requires a PV predicate");

    for (std::size_t window_offset = 0; window_offset < windows.size(); ++window_offset)
    {
        const auto& [window_begin_ns, window_end_ns] = windows[window_offset];
        const auto slice_ns = window_shards.slice_ns == 0
                                  ? mldp_pvxs_driver::query::executor::autoSliceNs(window_end_ns - window_begin_ns)
                                  : window_shards.slice_ns;
        for (int64_t slice_begin_ns = window_begin_ns; slice_begin_ns <= window_end_ns; )
        {
            if (context.cancellation) context.cancellation->throwIfCancelled();
            const auto remaining = window_end_ns - slice_begin_ns;
            const auto slice_end_ns = remaining < slice_ns ? window_end_ns : slice_begin_ns + slice_ns;
            const bool final_slice = slice_end_ns == window_end_ns;
            const auto slice_index = static_cast<uint64_t>((slice_begin_ns - window_begin_ns) / slice_ns + 1);
            for (std::size_t pv_offset = 0; pv_offset < requested_pvs.size(); pv_offset += window_shards.series_per_shard)
            {
                auto predicates = pushable;
                predicates.erase(std::remove_if(predicates.begin(), predicates.end(), [&scan](const Predicate& predicate) {
                    return (scan.window_subquery || scan.window_literal) && (predicate.column == "time" || predicate.column == "pv");
                }), predicates.end());
                std::vector<ExecutableLiteralValue> pv_values;
                const auto pv_end = std::min(requested_pvs.size(), pv_offset + static_cast<std::size_t>(window_shards.series_per_shard));
                for (std::size_t index = pv_offset; index < pv_end; ++index) pv_values.emplace_back(requested_pvs[index]);
                const auto series_shard_index = pv_offset / static_cast<std::size_t>(window_shards.series_per_shard) + 1;
                const auto series_in_shard = static_cast<uint64_t>(pv_values.size());
                predicates.push_back(Predicate{.column = "pv", .op = PredicateOp::IN, .values = std::move(pv_values)});
                predicates.push_back(Predicate{.column = "time", .op = PredicateOp::GTE, .values = {slice_begin_ns / 1'000'000'000LL}});
                predicates.push_back(Predicate{.column = "time", .op = PredicateOp::LTE, .values = {slice_end_ns / 1'000'000'000LL}});
                if (context.progress)
                {
                    context.progress->setActivity(scan.table_name, "windowed MLDP scan", "opening serial cursor shard");
                    context.progress->setWindowShard(static_cast<uint64_t>(window_offset + 1), slice_index,
                                                     series_shard_index, series_in_shard);
                    context.progress->setParallelShards(1, 1);
                }
                auto     stream = queryable->executeStream(scan.table_name, predicates, scan.projection_hint, context);
                uint64_t shard_batch_count = 0;
                while (auto batch = stream->next())
                {
                    if (context.cancellation) context.cancellation->throwIfCancelled();
                    if (context.progress) context.progress->beginBackendRpc(scan.table_name, "window server cursor");
                    ++stats.rpc_calls;
                    ++shard_batch_count;
                    const auto backend_rows = static_cast<uint64_t>(batch->num_rows());
                    stats.rows_from_backend += backend_rows;
                    if (context.progress) context.progress->finishBackendRpc(backend_rows);
                    std::vector<Predicate> shard_bounds{
                        Predicate{.column = "time", .op = PredicateOp::GTE, .values = {TimestampNsLiteral{slice_begin_ns}}}};
                    if (!final_slice)
                        shard_bounds.push_back(Predicate{.column = "time", .op = PredicateOp::LT, .values = {TimestampNsLiteral{slice_end_ns}}});
                    auto bounded = applyFilter(batch, shard_bounds);
                    if (!bounded.ok()) throw std::runtime_error(bounded.status().ToString());
                    batch = *bounded;
                    if (!local.empty())
                    {
                        auto filtered = applyFilter(batch, local);
                        if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
                        batch = *filtered;
                    }
                    if (scan.qualify_output) batch = qualifyBatchColumns(batch, scan.table_alias);
                    output.push_back(std::move(batch));
                }
                if (shard_batch_count == 0)
                {
                    if (context.progress) context.progress->beginBackendRpc(scan.table_name, "window server cursor");
                    ++stats.rpc_calls;
                    if (context.progress) context.progress->finishBackendRpc(0);
                }
                if (context.progress)
                {
                    context.progress->setParallelShards(0, 1);
                    context.progress->completeShard();
                }
            }
            if (final_slice) break;
            slice_begin_ns = slice_end_ns;
        }
    }
    return output;
}
