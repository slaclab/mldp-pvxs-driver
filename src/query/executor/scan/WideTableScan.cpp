//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ExecutorUtils.h>
#include <query/QueryCancellation.h>
#include <query/executor/ScanExecutionHelpers.h>

#include <query/QueryResult.h>
#include <query/QueryProgress.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>

#include <arrow/array.h>
#include <arrow/builder.h>
#include <arrow/compute/api.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/scalar.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
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

RecordBatches pivotLongBatches(const RecordBatches& long_batches,
                               const std::vector<std::string>& requested_pvs,
                               arrow::MemoryPool* pool)
{
    struct Cell {
        std::shared_ptr<arrow::Scalar> value;
    };
    std::map<int64_t, std::unordered_map<std::string, Cell>> rows;
    std::unordered_map<std::string, std::shared_ptr<arrow::DataType>> value_types;

    for (const auto& batch : long_batches)
    {
        if (!batch) continue;
        const auto pv_index = batch->schema()->GetFieldIndex("pv");
        const auto time_index = batch->schema()->GetFieldIndex("time");
        const auto value_index = batch->schema()->GetFieldIndex("value");
        if (pv_index < 0 || time_index < 0 || value_index < 0)
            throw std::runtime_error("MLDP streamed wide pivot requires pv, time, and value columns");
        const auto pvs = std::dynamic_pointer_cast<arrow::StringArray>(batch->column(pv_index));
        const auto times = std::dynamic_pointer_cast<arrow::TimestampArray>(batch->column(time_index));
        if (!pvs || !times) throw std::runtime_error("MLDP streamed wide pivot received invalid pv or time columns");
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            if (pvs->IsNull(row) || times->IsNull(row))
                throw std::runtime_error("MLDP streamed wide pivot received a null pv or time");
            const auto pv = pvs->GetString(row);
            if (std::find(requested_pvs.begin(), requested_pvs.end(), pv) == requested_pvs.end())
                throw std::runtime_error("MLDP streamed wide pivot received an unexpected PV '" + pv + "'");
            auto scalar_result = batch->column(value_index)->GetScalar(row);
            if (!scalar_result.ok()) throw std::runtime_error(scalar_result.status().ToString());
            auto value = activeValue(*scalar_result);
            auto& cells = rows[times->Value(row)];
            if (cells.contains(pv))
                throw std::runtime_error("MLDP streamed wide pivot received duplicate (time, pv) data for '" + pv +
                                         "' at " + std::to_string(times->Value(row)) + " ns");
            if (value && value->is_valid)
            {
                const auto type = value_types.find(pv);
                if (type == value_types.end()) value_types.emplace(pv, value->type);
                else if (!type->second->Equals(*value->type))
                    throw std::runtime_error("MLDP streamed wide pivot received mixed value types for '" + pv + "'");
            }
            cells.emplace(pv, Cell{.value = std::move(value)});
        }
    }

    arrow::TimestampBuilder time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool);
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> value_builders;
    std::vector<std::shared_ptr<arrow::Field>> fields{arrow::field("time", arrow::timestamp(arrow::TimeUnit::NANO, "UTC"))};
    value_builders.reserve(requested_pvs.size());
    for (const auto& pv : requested_pvs)
    {
        const auto type = value_types.contains(pv) ? value_types.at(pv) : arrow::null();
        std::unique_ptr<arrow::ArrayBuilder> builder;
        const auto status = arrow::MakeBuilder(pool, type, &builder);
        if (!status.ok()) throw std::runtime_error(status.ToString());
        fields.push_back(arrow::field(pv, type));
        value_builders.push_back(std::move(builder));
    }
    for (const auto& [time, cells] : rows)
    {
        if (!time_builder.Append(time).ok()) throw std::runtime_error("Failed to append streamed wide pivot time");
        for (std::size_t index = 0; index < requested_pvs.size(); ++index)
        {
            const auto found = cells.find(requested_pvs[index]);
            const auto value = found == cells.end() ? nullptr : found->second.value;
            const auto status = !value || !value->is_valid
                ? value_builders[index]->AppendNull()
                : value_builders[index]->AppendScalar(*value);
            if (!status.ok()) throw std::runtime_error(status.ToString());
        }
    }
    std::shared_ptr<arrow::Array> time;
    if (!time_builder.Finish(&time).ok()) throw std::runtime_error("Failed to finish streamed wide pivot time");
    std::vector<std::shared_ptr<arrow::Array>> arrays{time};
    arrays.reserve(requested_pvs.size() + 1);
    for (auto& builder : value_builders)
    {
        std::shared_ptr<arrow::Array> values;
        const auto status = builder->Finish(&values);
        if (!status.ok()) throw std::runtime_error(status.ToString());
        arrays.push_back(std::move(values));
    }
    return {arrow::RecordBatch::Make(arrow::schema(std::move(fields)), time->length(), std::move(arrays))};
}

RecordBatches readSortedPivotRuns(const SpillHandle& long_spill,
                                  const std::shared_ptr<SpillManager>& spill_manager,
                                  const std::vector<std::string>& requested_pvs,
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
        const auto time_index = batch->schema()->GetFieldIndex("time");
        if (time_index < 0) throw std::runtime_error("MLDP streamed wide pivot requires a time column");
        const auto times = std::dynamic_pointer_cast<arrow::TimestampArray>(batch->column(time_index));
        if (!times) throw std::runtime_error("MLDP streamed wide pivot received an invalid time column");
        std::vector<int64_t> rows(static_cast<std::size_t>(batch->num_rows()));
        std::iota(rows.begin(), rows.end(), 0);
        std::stable_sort(rows.begin(), rows.end(), [&times](const int64_t left, const int64_t right) {
            return times->Value(left) < times->Value(right);
        });
        arrow::Int64Builder indices;
        if (!indices.AppendValues(rows).ok()) throw std::runtime_error("Failed to build wide pivot sort indices");
        std::shared_ptr<arrow::Array> index_array;
        if (!indices.Finish(&index_array).ok()) throw std::runtime_error("Failed to finish wide pivot sort indices");
        std::vector<std::shared_ptr<arrow::Array>> sorted_columns;
        sorted_columns.reserve(batch->num_columns());
        for (const auto& column : batch->columns())
        {
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
    std::vector<std::shared_ptr<arrow::Field>> fields{arrow::field("time", arrow::timestamp(arrow::TimeUnit::NANO, "UTC"))};
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    builders.reserve(requested_pvs.size());
    for (const auto& pv : requested_pvs)
    {
        const auto type = value_types.contains(pv) ? value_types.at(pv) : arrow::null();
        std::unique_ptr<arrow::ArrayBuilder> builder;
        const auto status = arrow::MakeBuilder(pool, type, &builder);
        if (!status.ok()) throw std::runtime_error(status.ToString());
        fields.push_back(arrow::field(pv, type));
        builders.push_back(std::move(builder));
    }
    const auto schema = arrow::schema(fields);
    arrow::TimestampBuilder time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool);
    RecordBatches output;
    const auto flush = [&]()
    {
        if (time_builder.length() == 0) return;
        std::shared_ptr<arrow::Array> time;
        if (!time_builder.Finish(&time).ok()) throw std::runtime_error("Failed to finish wide pivot time batch");
        std::vector<std::shared_ptr<arrow::Array>> arrays{time};
        for (auto& builder : builders)
        {
            std::shared_ptr<arrow::Array> values;
            const auto status = builder->Finish(&values);
            if (!status.ok()) throw std::runtime_error(status.ToString());
            arrays.push_back(std::move(values));
        }
        output.push_back(arrow::RecordBatch::Make(schema, time->length(), std::move(arrays)));
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
        int64_t minimum_time = std::numeric_limits<int64_t>::max();
        for (const auto& cursor : cursors)
        {
            const auto times = std::static_pointer_cast<arrow::TimestampArray>(cursor.batch->GetColumnByName("time"));
            minimum_time = std::min(minimum_time, times->Value(cursor.row));
        }
        std::unordered_map<std::string, std::shared_ptr<arrow::Scalar>> cells;
        for (std::size_t index = 0; index < cursors.size();)
        {
            auto& cursor = cursors[index];
            const auto times = std::static_pointer_cast<arrow::TimestampArray>(cursor.batch->GetColumnByName("time"));
            if (times->Value(cursor.row) != minimum_time)
            {
                ++index;
                continue;
            }
            const auto pvs = std::static_pointer_cast<arrow::StringArray>(cursor.batch->GetColumnByName("pv"));
            const auto pv = pvs->GetString(cursor.row);
            if (std::find(requested_pvs.begin(), requested_pvs.end(), pv) == requested_pvs.end())
                throw std::runtime_error("MLDP streamed wide pivot received an unexpected PV '" + pv + "'");
            auto scalar = cursor.batch->GetColumnByName("value")->GetScalar(cursor.row);
            if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
            if (!cells.emplace(pv, activeValue(*scalar)).second)
                throw std::runtime_error("MLDP streamed wide pivot received duplicate (time, pv) data for '" + pv + "' at " + std::to_string(minimum_time) + " ns");
            advance(cursor);
            if (!cursor.batch) cursors.erase(cursors.begin() + static_cast<std::ptrdiff_t>(index));
        }
        if (!time_builder.Append(minimum_time).ok()) throw std::runtime_error("Failed to append wide pivot time");
        for (std::size_t index = 0; index < requested_pvs.size(); ++index)
        {
            const auto found = cells.find(requested_pvs[index]);
            const auto status = found == cells.end() || !found->second || !found->second->is_valid
                ? builders[index]->AppendNull()
                : builders[index]->AppendScalar(*found->second);
            if (!status.ok()) throw std::runtime_error(status.ToString());
        }
        if (time_builder.length() == 4096) flush();
    }
    flush();
    return output;
}

} // namespace

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

    if (scan.table_name == "mldp.time_series_table")
    {
        auto temporary_spill = context.spill;
        if (!temporary_spill)
        {
            temporary_spill = std::make_shared<SpillManager>(
                std::make_shared<arrow::fs::LocalFileSystem>(),
                (std::filesystem::temp_directory_path() / "mldp-query-spill").string());
        }
        std::vector<Predicate> long_filters;
        for (const auto& predicate : pushable)
            if (predicate.column != "pv" && predicate.column != "time") long_filters.push_back(predicate);
        std::optional<SpillWriter> long_writer;
        std::unordered_map<std::string, std::shared_ptr<arrow::DataType>> value_types;
        for (const auto& [begin_ns, end_ns] : windows)
        {
            for (int64_t slice_begin_ns = begin_ns; slice_begin_ns <= end_ns; )
            {
                const auto remaining = end_ns - slice_begin_ns;
                const auto slice_end_ns = remaining < window_shards.slice_ns ? end_ns : slice_begin_ns + window_shards.slice_ns;
                const bool final_slice = slice_end_ns == end_ns;
                for (std::size_t pv_offset = 0; pv_offset < requested_pvs.size(); pv_offset += window_shards.pv_group)
                {
                    auto predicates = pushable;
                    predicates.erase(std::remove_if(predicates.begin(), predicates.end(), [](const Predicate& predicate) {
                        return predicate.column == "time" || predicate.column == "pv";
                    }), predicates.end());
                    std::vector<ExecutableLiteralValue> pv_values;
                    const auto pv_end = std::min(requested_pvs.size(), pv_offset + static_cast<std::size_t>(window_shards.pv_group));
                    for (std::size_t index = pv_offset; index < pv_end; ++index) pv_values.emplace_back(requested_pvs[index]);
                    predicates.push_back(Predicate{.column = "pv", .op = PredicateOp::IN, .values = std::move(pv_values)});
                    predicates.push_back(Predicate{.column = "time", .op = PredicateOp::GTE, .values = {slice_begin_ns / 1'000'000'000LL}});
                    predicates.push_back(Predicate{.column = "time", .op = PredicateOp::LTE, .values = {slice_end_ns / 1'000'000'000LL}});
                    if (context.progress) context.progress->beginBackendRpc("mldp.time_series", "wide shard open");
                    auto stream = queryable->executeStream("mldp.time_series", predicates, {}, context);
                    while (auto batch = stream->next())
                    {
                        if (context.cancellation) context.cancellation->throwIfCancelled();
                        ++stats.rpc_calls;
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
                        if (!long_filters.empty())
                        {
                            auto filtered = applyFilter(batch, long_filters);
                            if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
                            batch = *filtered;
                        }
                        const auto pv_column = std::static_pointer_cast<arrow::StringArray>(batch->GetColumnByName("pv"));
                        const auto value_column = batch->GetColumnByName("value");
                        if (!pv_column || !value_column) throw std::runtime_error("MLDP streamed wide pivot requires pv and value columns");
                        for (int64_t row = 0; row < batch->num_rows(); ++row)
                        {
                            auto scalar = value_column->GetScalar(row);
                            if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
                            auto value = activeValue(*scalar);
                            if (!value || !value->is_valid) continue;
                            const auto pv = pv_column->GetString(row);
                            if (const auto found = value_types.find(pv); found == value_types.end()) value_types.emplace(pv, value->type);
                            else if (!found->second->Equals(*value->type))
                                throw std::runtime_error("MLDP streamed wide pivot received mixed value types for '" + pv + "'");
                        }
                        auto projected = applyProjection(RecordBatches{std::move(batch)}, {"pv", "time", "value"}).front();
                        if (!long_writer)
                        {
                            auto writer = temporary_spill->openWriter("wide-long", projected->schema());
                            if (!writer.ok()) throw std::runtime_error(writer.status().ToString());
                            long_writer.emplace(std::move(*writer));
                        }
                        const auto append_status = long_writer->append(projected);
                        if (!append_status.ok()) throw std::runtime_error(append_status.ToString());
                    }
                }
                if (final_slice) break;
                slice_begin_ns = slice_end_ns;
            }
        }
        if (!long_writer) return {};
        if (context.progress) context.progress->setPhase(QueryProgressPhase::Executing, "wide spill finalization");
        auto spilled = long_writer->finish();
        if (!spilled.ok()) throw std::runtime_error(spilled.status().ToString());
        stats.bytes_spilled += static_cast<uint64_t>(spilled->byte_count);
        ++stats.materialized_files;
        stats.materialized_bytes += static_cast<uint64_t>(spilled->byte_count);
        if (context.progress) context.progress->setPhase(QueryProgressPhase::Executing, "wide external sort and pivot");
        output = readSortedPivotRuns(*spilled, temporary_spill, requested_pvs, value_types, context, stats);
        if (!local.empty())
        {
            for (auto& batch : output)
            {
                auto filtered = applyFilter(batch, local);
                if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
                batch = *filtered;
            }
        }
        if (scan.qualify_output)
            for (auto& batch : output) batch = qualifyBatchColumns(batch, scan.table_alias);
        return output;
    }

    const auto slice_ns = window_shards.slice_ns;
    for (const auto& [window_begin_ns, window_end_ns] : windows)
    {
        for (int64_t slice_begin_ns = window_begin_ns; slice_begin_ns <= window_end_ns; )
        {
            if (context.cancellation) context.cancellation->throwIfCancelled();
            const auto remaining = window_end_ns - slice_begin_ns;
            const auto slice_end_ns = remaining < slice_ns ? window_end_ns : slice_begin_ns + slice_ns;
            const bool final_slice = slice_end_ns == window_end_ns;
            for (std::size_t pv_offset = 0; pv_offset < requested_pvs.size(); pv_offset += window_shards.pv_group)
            {
                auto predicates = pushable;
                predicates.erase(std::remove_if(predicates.begin(), predicates.end(), [&scan](const Predicate& predicate) {
                    return (scan.window_subquery || scan.window_literal) && (predicate.column == "time" || predicate.column == "pv");
                }), predicates.end());
                std::vector<ExecutableLiteralValue> pv_values;
                const auto pv_end = std::min(requested_pvs.size(), pv_offset + static_cast<std::size_t>(window_shards.pv_group));
                for (std::size_t index = pv_offset; index < pv_end; ++index) pv_values.emplace_back(requested_pvs[index]);
                predicates.push_back(Predicate{.column = "pv", .op = PredicateOp::IN, .values = std::move(pv_values)});
                predicates.push_back(Predicate{.column = "time", .op = PredicateOp::GTE, .values = {slice_begin_ns / 1'000'000'000LL}});
                predicates.push_back(Predicate{.column = "time", .op = PredicateOp::LTE, .values = {slice_end_ns / 1'000'000'000LL}});
                if (scan.table_name == "mldp.time_series")
                {
                    if (context.progress) context.progress->beginBackendRpc(scan.table_name, "window server cursor");
                    auto stream = queryable->executeStream(scan.table_name, predicates, scan.projection_hint, context);
                    while (auto batch = stream->next())
                    {
                        if (context.cancellation) context.cancellation->throwIfCancelled();
                        ++stats.rpc_calls;
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
                    continue;
                }
                std::string page_token;
                do
                {
                    if (context.progress) context.progress->beginBackendRpc(scan.table_name, page_token.empty() ? "window" : "window continuation page");
                    const auto result = queryable->execute(scan.table_name, predicates, scan.projection_hint, context, page_token);
                    ++stats.rpc_calls;
                    page_token = result.next_page_token;
                    if (!result.batch) continue;
                    stats.rows_from_backend += static_cast<uint64_t>(result.batch->num_rows());
                    auto batch = result.batch;
                    if (!local.empty()) { auto filtered = applyFilter(batch, local); if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString()); batch = *filtered; }
                    if (scan.qualify_output) batch = qualifyBatchColumns(batch, scan.table_alias);
                    output.push_back(std::move(batch));
                } while (!page_token.empty());
            }
            if (final_slice) break;
            slice_begin_ns = slice_end_ns;
        }
    }
    return output;
}
