//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/GroupedAggregator.h>

#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>
#include <query/executor/AggregateValue.h>
#include <query/executor/ExecutorUtils.h>

#include <arrow/array/builder_primitive.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

namespace {

// Group-state budget used when the context sets no memory limit; past it groups spill.
constexpr std::size_t kDefaultMemoryBudget = 1ULL << 30;

/** Dense group ids for encoded key tuples, plus the key values per group. */
class GroupKeyTable
{
public:
    explicit GroupKeyTable(const std::size_t key_count) : values_(key_count), types_(key_count) {}

    void assign(const std::vector<std::shared_ptr<arrow::Array>>& keys, const int64_t rows, std::vector<uint32_t>& group_ids)
    {
        group_ids.resize(static_cast<std::size_t>(rows));
        if (keys.empty())
        {
            std::fill(group_ids.begin(), group_ids.end(), 0U);
            return;
        }
        std::vector<ColumnReader> readers;
        readers.reserve(keys.size());
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            readers.emplace_back(*keys[index]);
            if (!types_[index]) types_[index] = preservedType(keys[index]->type());
        }
        for (int64_t row = 0; row < rows; ++row)
        {
            buffer_.clear();
            for (const auto& reader : readers) appendValueKey(buffer_, reader.at(row));
            const auto [found, inserted] = index_.try_emplace(buffer_, static_cast<uint32_t>(index_.size()));
            if (inserted)
            {
                bytes_ += buffer_.size() + 64;
                for (std::size_t index = 0; index < readers.size(); ++index)
                {
                    values_[index].push_back(OwnedValue::from(readers[index].at(row)));
                    bytes_ += sizeof(OwnedValue) + values_[index].back().heapBytes();
                }
            }
            group_ids[static_cast<std::size_t>(row)] = found->second;
        }
    }

    /** Splits rows into those whose key already has a group and the rest. */
    void classify(const std::vector<std::shared_ptr<arrow::Array>>& keys, const int64_t rows, std::vector<int64_t>& hits, std::vector<int64_t>& misses)
    {
        std::vector<ColumnReader> readers;
        readers.reserve(keys.size());
        for (const auto& key : keys) readers.emplace_back(*key);
        for (int64_t row = 0; row < rows; ++row)
        {
            buffer_.clear();
            for (const auto& reader : readers) appendValueKey(buffer_, reader.at(row));
            (index_.contains(buffer_) ? hits : misses).push_back(row);
        }
    }

    std::size_t size() const noexcept { return index_.size(); }
    std::size_t memoryBytes() const noexcept { return bytes_; }

    std::vector<std::shared_ptr<arrow::Array>> finish(arrow::MemoryPool* pool) const
    {
        std::vector<std::shared_ptr<arrow::Array>> columns;
        for (std::size_t index = 0; index < values_.size(); ++index) columns.push_back(buildValueColumn(values_[index], types_[index], pool));
        return columns;
    }

private:
    std::unordered_map<std::string, uint32_t>     index_;
    std::vector<std::vector<OwnedValue>>          values_;
    std::vector<std::shared_ptr<arrow::DataType>> types_;
    std::string                                   buffer_;
    std::size_t                                   bytes_{0};
};

/** In-memory aggregation of one stream of evaluated batches. */
class InMemoryGroups
{
public:
    InMemoryGroups(const plan::AggregateSpec& spec, std::vector<const IAggregateFunction*> functions)
        : spec_(spec), functions_(std::move(functions)), keys_(spec.keys.size()), accumulators_(spec.aggregates.size())
    {
        if (spec.keys.empty()) grow(1);
    }

    /** @p evaluated: key columns then one column per aggregate with an argument. */
    void consume(const std::shared_ptr<arrow::RecordBatch>& evaluated)
    {
        const auto rows = evaluated->num_rows();
        std::vector<std::shared_ptr<arrow::Array>> key_columns;
        for (std::size_t index = 0; index < spec_.keys.size(); ++index) key_columns.push_back(evaluated->column(static_cast<int>(index)));
        keys_.assign(key_columns, rows, group_ids_);
        grow(spec_.keys.empty() ? 1 : keys_.size());

        int column = static_cast<int>(spec_.keys.size());
        for (std::size_t index = 0; index < spec_.aggregates.size(); ++index)
        {
            const auto& call = spec_.aggregates[index];
            const arrow::Array* values = nullptr;
            std::shared_ptr<arrow::Array> owner;
            if (call.argument)
            {
                owner = evaluated->column(column++);
                values = owner.get();
                if (!accumulators_[index]) accumulators_[index] = functions_[index]->makeAccumulator(owner->type(), call.distinct);
                grow(groupCount());
            }
            accumulators_[index]->consume(values, group_ids_.data(), rows);
        }
    }

    std::size_t groupCount() const noexcept { return spec_.keys.empty() ? 1 : keys_.size(); }

    void classify(const std::shared_ptr<arrow::RecordBatch>& evaluated, std::vector<int64_t>& hits, std::vector<int64_t>& misses)
    {
        std::vector<std::shared_ptr<arrow::Array>> key_columns;
        for (std::size_t index = 0; index < spec_.keys.size(); ++index) key_columns.push_back(evaluated->column(static_cast<int>(index)));
        keys_.classify(key_columns, evaluated->num_rows(), hits, misses);
    }

    std::size_t memoryBytes() const
    {
        std::size_t bytes = keys_.memoryBytes();
        for (const auto& accumulator : accumulators_)
            if (accumulator) bytes += accumulator->memoryBytes();
        return bytes;
    }

    /** Key columns then aggregate columns, before HAVING. */
    std::shared_ptr<arrow::RecordBatch> finish(arrow::MemoryPool* pool)
    {
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> arrays = keys_.finish(pool);
        for (std::size_t index = 0; index < spec_.keys.size(); ++index) fields.push_back(arrow::field(spec_.keys[index].name, arrays[index]->type()));
        for (std::size_t index = 0; index < spec_.aggregates.size(); ++index)
        {
            auto& accumulator = accumulators_[index];
            if (!accumulator)
            {
                // No input reached this aggregate: build it with an unknown input type.
                accumulator = functions_[index]->makeAccumulator(nullptr, spec_.aggregates[index].distinct);
                accumulator->resize(groupCount());
            }
            auto array = accumulator->finish(pool);
            fields.push_back(arrow::field(spec_.aggregates[index].name, array->type()));
            arrays.push_back(std::move(array));
        }
        return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), static_cast<int64_t>(groupCount()), std::move(arrays));
    }

private:
    void grow(const std::size_t groups)
    {
        for (std::size_t index = 0; index < accumulators_.size(); ++index)
        {
            if (!accumulators_[index] && !spec_.aggregates[index].argument)
                accumulators_[index] = functions_[index]->makeAccumulator(nullptr, false);
            if (accumulators_[index]) accumulators_[index]->resize(groups);
        }
    }

    const plan::AggregateSpec&                          spec_;
    std::vector<const IAggregateFunction*>              functions_;
    GroupKeyTable                                       keys_;
    std::vector<std::unique_ptr<IAggregateAccumulator>> accumulators_;
    std::vector<uint32_t>                               group_ids_;
};

uint64_t hashKey(const std::string& key, const uint32_t depth)
{
    // Mix the depth in so a re-partitioned partition splits differently.
    return std::hash<std::string>{}(key) ^ (0x9e3779b97f4a7c15ULL * (depth + 1));
}

/** Aggregates evaluated batches (key columns, then aggregate arguments).
 *
 *  While under budget every row is folded in memory.  Past the budget the
 *  current groups are frozen: rows whose key already has a group still fold in
 *  memory, rows with new keys are hash-partitioned into spill files.  Frozen
 *  groups therefore see every row of their keys, and each partition holds keys
 *  absent from memory and from the other partitions, so partitions are
 *  aggregated independently (recursively) without merging. */
class EvaluatedAggregator
{
public:
    EvaluatedAggregator(const plan::AggregateSpec& spec, const std::vector<const IAggregateFunction*>& functions,
                        const ExecutionContext& context, const std::size_t budget, const uint32_t depth)
        : spec_(spec), functions_(functions), context_(context), budget_(budget), depth_(depth),
          groups_(std::make_unique<InMemoryGroups>(spec, functions))
    {
    }

    void consume(const std::shared_ptr<arrow::RecordBatch>& evaluated)
    {
        if (!frozen_)
        {
            groups_->consume(evaluated);
            // A global group never grows; spilling needs keys and a spill manager.
            if (!spec_.keys.empty() && context_.spill && depth_ < kMaxDepth && groups_->memoryBytes() > budget_) frozen_ = true;
            return;
        }
        std::vector<int64_t> hits;
        std::vector<int64_t> misses;
        groups_->classify(evaluated, hits, misses);
        if (!hits.empty()) groups_->consume(hits.size() == static_cast<std::size_t>(evaluated->num_rows()) ? evaluated : selectRows(evaluated, hits));
        if (!misses.empty()) spill(misses.size() == static_cast<std::size_t>(evaluated->num_rows()) ? evaluated : selectRows(evaluated, misses));
    }

    std::size_t groupCount() const { return groups_->groupCount(); }

    /** Emits frozen/in-memory groups, then each partition's groups (before HAVING). */
    void finish(const std::function<void(std::shared_ptr<arrow::RecordBatch>)>& emit)
    {
        emit(groups_->finish(context_.pool));
        groups_.reset();
        for (auto& writer : writers_)
        {
            if (!writer) continue; // no rows hashed to this partition
            auto handle = writer->finish();
            if (!handle.ok()) throw std::runtime_error("GROUP BY failed to finish spill partition: " + handle.status().ToString());
            if (handle->batch_count == 0) continue;
            auto reader = context_.spill->read(*handle);
            if (!reader.ok()) throw std::runtime_error("GROUP BY failed to read spill partition: " + reader.status().ToString());
            EvaluatedAggregator partition(spec_, functions_, context_, budget_, depth_ + 1);
            while (true)
            {
                if (context_.cancellation) context_.cancellation->throwIfCancelled();
                auto batch = reader->next();
                if (!batch.ok()) throw std::runtime_error("GROUP BY failed to read spill partition: " + batch.status().ToString());
                if (!*batch) break;
                partition.consume(*batch);
            }
            partition.finish(emit);
        }
    }

    bool spilled() const { return !writers_.empty(); }

private:
    static constexpr uint32_t kMaxDepth = 4;

    void spill(const std::shared_ptr<arrow::RecordBatch>& rows)
    {
        const auto partition_count = static_cast<std::size_t>(std::max<uint32_t>(2, context_.spill_partitions));
        if (writers_.empty()) writers_.resize(partition_count);
        std::vector<std::vector<int64_t>> selected(partition_count);
        std::vector<ColumnReader>         readers;
        for (std::size_t index = 0; index < spec_.keys.size(); ++index) readers.emplace_back(*rows->column(static_cast<int>(index)));
        std::string key;
        for (int64_t row = 0; row < rows->num_rows(); ++row)
        {
            key.clear();
            for (const auto& reader : readers) appendValueKey(key, reader.at(row));
            selected[hashKey(key, depth_) % partition_count].push_back(row);
        }
        for (std::size_t index = 0; index < partition_count; ++index)
        {
            if (selected[index].empty()) continue;
            if (!writers_[index])
            {
                auto writer = context_.spill->openWriter("group-by-d" + std::to_string(depth_) + "-p" + std::to_string(index), rows->schema());
                if (!writer.ok()) throw std::runtime_error("GROUP BY failed to open spill partition: " + writer.status().ToString());
                writers_[index].emplace(std::move(*writer));
            }
            if (const auto status = writers_[index]->append(selectRows(rows, selected[index])); !status.ok())
                throw std::runtime_error("GROUP BY failed to write spill partition: " + status.ToString());
        }
    }

    const plan::AggregateSpec&                    spec_;
    const std::vector<const IAggregateFunction*>& functions_;
    const ExecutionContext&                       context_;
    std::size_t                                   budget_;
    uint32_t                                      depth_;
    std::unique_ptr<InMemoryGroups>               groups_;
    bool                                          frozen_{false};
    std::vector<std::optional<SpillWriter>>       writers_;
};

} // namespace

struct GroupedAggregator::State
{
    plan::AggregateSpec                    spec;
    ExecutionContext                       context;
    std::vector<const IAggregateFunction*> functions;
    std::vector<ExpressionPtr>             expressions;
    std::vector<std::string>               names;
    std::unique_ptr<EvaluatedAggregator>   aggregator;

    /** Evaluates keys and aggregate arguments of an input batch. */
    std::shared_ptr<arrow::RecordBatch> evaluate(const std::shared_ptr<arrow::RecordBatch>& batch) const
    {
        if (expressions.empty())
        {
            // Only argument-less aggregates without keys: keep the row count.
            return arrow::RecordBatch::Make(arrow::schema(arrow::FieldVector{}), batch->num_rows(), std::vector<std::shared_ptr<arrow::Array>>{});
        }
        const auto projected = applyProjection(RecordBatches{batch}, expressions, names);
        return projected.empty() ? nullptr : projected.front();
    }

    std::shared_ptr<arrow::RecordBatch> applyHaving(std::shared_ptr<arrow::RecordBatch> output) const
    {
        if (!spec.having || output->num_rows() == 0) return output;
        const auto condition = applyProjection(RecordBatches{output}, {spec.having}, {"__having"});
        std::vector<int64_t> rows;
        if (!condition.empty() && condition.front())
        {
            const ColumnReader reader(*condition.front()->column(0));
            for (int64_t row = 0; row < output->num_rows(); ++row)
            {
                const auto value = reader.at(row);
                if (value.kind == ValueKind::BOOL && value.boolean) rows.push_back(row);
            }
        }
        return rows.size() == static_cast<std::size_t>(output->num_rows()) ? output : selectRows(output, rows);
    }
};

GroupedAggregator::GroupedAggregator(plan::AggregateSpec spec, ExecutionContext context) : state_(std::make_unique<State>())
{
    auto& state = *state_;
    state.spec = std::move(spec);
    state.context = std::move(context);
    for (const auto& key : state.spec.keys)
    {
        state.expressions.push_back(key.expression);
        state.names.push_back(key.name);
    }
    for (const auto& call : state.spec.aggregates)
    {
        const auto* function = AggregateRegistry::instance().find(call.function);
        if (function == nullptr) throw std::runtime_error("Unknown aggregate function: " + call.function);
        state.functions.push_back(function);
        if (!call.argument) continue;
        state.expressions.push_back(call.argument);
        state.names.push_back(call.name + "_input");
    }
    const auto budget = state.context.memory_limit_bytes != 0 ? static_cast<std::size_t>(state.context.memory_limit_bytes) : kDefaultMemoryBudget;
    state.aggregator = std::make_unique<EvaluatedAggregator>(state.spec, state.functions, state.context, budget, 0);
}

GroupedAggregator::~GroupedAggregator() = default;

std::size_t GroupedAggregator::groupCount() const noexcept { return state_->aggregator ? state_->aggregator->groupCount() : 0; }

void GroupedAggregator::consume(const std::shared_ptr<arrow::RecordBatch>& batch)
{
    auto& state = *state_;
    if (state.context.cancellation) state.context.cancellation->throwIfCancelled();
    if (!batch || batch->num_rows() == 0) return;
    const auto evaluated = state.evaluate(batch);
    if (!evaluated) return;
    state.aggregator->consume(evaluated);
    if (state.context.progress)
        state.context.progress->setActivity({}, "aggregating",
                                            std::to_string(state.aggregator->groupCount()) + " groups" + (state.aggregator->spilled() ? " (spilling)" : ""));
}

RecordBatches GroupedAggregator::finish(const int64_t batch_rows)
{
    auto&         state = *state_;
    RecordBatches result;
    state.aggregator->finish([&](std::shared_ptr<arrow::RecordBatch> output)
    {
        if (state.context.cancellation) state.context.cancellation->throwIfCancelled();
        output = state.applyHaving(std::move(output));
        for (int64_t offset = 0; offset < output->num_rows(); offset += batch_rows)
            result.push_back(output->Slice(offset, std::min(batch_rows, output->num_rows() - offset)));
    });
    state.aggregator.reset();
    return result;
}

RecordBatches mldp_pvxs_driver::query::executor::applyAggregate(const RecordBatches& input, const plan::AggregateSpec& spec, const ExecutionContext& context)
{
    GroupedAggregator aggregator(spec, context);
    for (const auto& batch : input) aggregator.consume(batch);
    return aggregator.finish();
}
