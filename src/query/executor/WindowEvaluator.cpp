//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/WindowEvaluator.h>

#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>
#include <query/SpillManager.h>
#include <query/executor/AggregateValue.h>
#include <query/executor/ExecutorUtils.h>

#include <arrow/array/builder_primitive.h>
#include <arrow/array/util.h>
#include <arrow/compute/api.h>
#include <arrow/util/byte_size.h>

#include <algorithm>
#include <deque>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

namespace {

constexpr std::size_t kDefaultMemoryBudget = 1ULL << 30;
constexpr int64_t     kOutputBatchRows = 65536;

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error("Window function failed: " + message);
}

void check(const arrow::Status& status, const char* what)
{
    if (!status.ok()) fail(std::string(what) + ": " + status.ToString());
}

template <typename T>
T check(arrow::Result<T> result, const char* what)
{
    if (!result.ok()) fail(std::string(what) + ": " + result.status().ToString());
    return std::move(*result);
}

int64_t saturatingAdd(const int64_t lhs, const int64_t rhs)
{
    int64_t result = 0;
    if (__builtin_add_overflow(lhs, rhs, &result)) return rhs > 0 ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
    return result;
}

int64_t saturatingSub(const int64_t lhs, const int64_t rhs)
{
    int64_t result = 0;
    if (__builtin_sub_overflow(lhs, rhs, &result)) return rhs < 0 ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
    return result;
}

/** Orders two non-null values: numbers numerically, strings lexically, booleans false < true. */
int compareRefs(const ValueRef& lhs, const ValueRef& rhs)
{
    if (lhs.kind == ValueKind::INT && rhs.kind == ValueKind::INT) return (lhs.integer > rhs.integer) - (lhs.integer < rhs.integer);
    if (lhs.numeric() && rhs.numeric())
    {
        const auto left = lhs.asDouble();
        const auto right = rhs.asDouble();
        return (left > right) - (left < right);
    }
    if (lhs.kind == ValueKind::STRING && rhs.kind == ValueKind::STRING)
    {
        const auto order = lhs.text.compare(rhs.text);
        return (order > 0) - (order < 0);
    }
    if (lhs.kind == ValueKind::BOOL && rhs.kind == ValueKind::BOOL) return static_cast<int>(lhs.boolean) - static_cast<int>(rhs.boolean);
    if (lhs.kind == ValueKind::OTHER || rhs.kind == ValueKind::OTHER) fail("PARTITION BY, ORDER BY and MIN/MAX values must be scalar");
    return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind) ? -1 : 1;
}

/** Key order: nulls are equal to each other and sort after every value, in both directions. */
int compareKey(const ValueRef& lhs, const ValueRef& rhs, const bool descending)
{
    const bool left_null = lhs.kind == ValueKind::NUL;
    const bool right_null = rhs.kind == ValueKind::NUL;
    if (left_null || right_null) return left_null == right_null ? 0 : (left_null ? 1 : -1);
    const auto order = compareRefs(lhs, rhs);
    return descending ? -order : order;
}

/** Positions of the PARTITION BY and ORDER BY key columns in an evaluated batch. */
struct KeyLayout
{
    std::vector<int>  partition;
    std::vector<int>  order;
    std::vector<bool> descending;
};

/** Readers of the key columns of one batch. */
struct KeyReaders
{
    KeyReaders(const arrow::RecordBatch& batch, const KeyLayout& layout) : layout(&layout)
    {
        for (const auto index : layout.partition) partition.emplace_back(*batch.column(index));
        for (const auto index : layout.order) order.emplace_back(*batch.column(index));
    }

    const KeyLayout*          layout;
    std::vector<ColumnReader> partition;
    std::vector<ColumnReader> order;
};

int comparePartition(const KeyReaders& lhs, const int64_t lhs_row, const KeyReaders& rhs, const int64_t rhs_row)
{
    for (std::size_t index = 0; index < lhs.partition.size(); ++index)
        if (const auto order = compareKey(lhs.partition[index].at(lhs_row), rhs.partition[index].at(rhs_row), false); order != 0) return order;
    return 0;
}

int compareOrder(const KeyReaders& lhs, const int64_t lhs_row, const KeyReaders& rhs, const int64_t rhs_row)
{
    for (std::size_t index = 0; index < lhs.order.size(); ++index)
        if (const auto order = compareKey(lhs.order[index].at(lhs_row), rhs.order[index].at(rhs_row), lhs.layout->descending[index]); order != 0)
            return order;
    return 0;
}

int compareRows(const KeyReaders& lhs, const int64_t lhs_row, const KeyReaders& rhs, const int64_t rhs_row)
{
    const auto order = comparePartition(lhs, lhs_row, rhs, rhs_row);
    return order != 0 ? order : compareOrder(lhs, lhs_row, rhs, rhs_row);
}

/** Row order of @p batch sorted by partition then order keys (stable).
 *
 *  With @p sorted_input the rows are expected ordered within each partition
 *  already, so only a stable partition sort is needed; the order is verified
 *  and a full sort is used when it does not hold. */
std::vector<int64_t> sortPermutation(const arrow::RecordBatch& batch, const KeyLayout& layout, const bool sorted_input)
{
    const KeyReaders     keys(batch, layout);
    std::vector<int64_t> rows(static_cast<std::size_t>(batch.num_rows()));
    std::iota(rows.begin(), rows.end(), 0);
    if (sorted_input)
    {
        std::stable_sort(rows.begin(), rows.end(), [&](const int64_t lhs, const int64_t rhs) { return comparePartition(keys, lhs, keys, rhs) < 0; });
        bool ordered = true;
        for (std::size_t index = 1; index < rows.size() && ordered; ++index)
            ordered = comparePartition(keys, rows[index - 1], keys, rows[index]) != 0 || compareOrder(keys, rows[index - 1], keys, rows[index]) <= 0;
        if (ordered) return rows;
    }
    std::stable_sort(rows.begin(), rows.end(), [&](const int64_t lhs, const int64_t rhs) { return compareRows(keys, lhs, keys, rhs) < 0; });
    return rows;
}

std::shared_ptr<arrow::Array> indexArray(const std::vector<int64_t>& indices)
{
    // A negative index becomes a null output row.
    arrow::Int64Builder builder;
    check(builder.Reserve(static_cast<int64_t>(indices.size())), "reserve indices");
    for (const auto index : indices)
    {
        if (index < 0) check(builder.AppendNull(), "append index");
        else builder.UnsafeAppend(index);
    }
    std::shared_ptr<arrow::Array> array;
    check(builder.Finish(&array), "build indices");
    return array;
}

std::shared_ptr<arrow::Array> take(const std::shared_ptr<arrow::Array>& source, const std::vector<int64_t>& indices)
{
    return check(arrow::compute::Take(source, indexArray(indices)), "gather rows").make_array();
}

std::shared_ptr<arrow::RecordBatch> takeRows(const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<int64_t>& rows)
{
    const auto indices = indexArray(rows);
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(static_cast<std::size_t>(batch->num_columns()));
    for (const auto& column : batch->columns()) arrays.push_back(check(arrow::compute::Take(column, indices), "sort rows").make_array());
    return arrow::RecordBatch::Make(batch->schema(), static_cast<int64_t>(rows.size()), std::move(arrays));
}

/** Gives null-typed columns (all-null computed values) the type other batches inferred, so the batches can be combined. */
void unifyNullColumns(std::vector<std::shared_ptr<arrow::RecordBatch>>& batches)
{
    if (batches.size() < 2) return;
    const auto columns = batches.front()->num_columns();
    for (int column = 0; column < columns; ++column)
    {
        std::shared_ptr<arrow::DataType> type;
        for (const auto& batch : batches)
            if (batch->column(column)->type_id() != arrow::Type::NA) type = batch->column(column)->type();
        if (!type) continue;
        for (auto& batch : batches)
        {
            if (batch->column(column)->type_id() != arrow::Type::NA) continue;
            const auto nulls = check(arrow::MakeArrayOfNull(type, batch->num_rows()), "build null column");
            batch = check(batch->SetColumn(column, batch->schema()->field(column)->WithType(type), nulls), "retype null column");
        }
    }
}

std::shared_ptr<arrow::RecordBatch> combine(std::vector<std::shared_ptr<arrow::RecordBatch>> batches)
{
    unifyNullColumns(batches);
    return combineBatches(batches);
}

/** Positions of one call's argument columns in an evaluated batch. */
struct CallLayout
{
    const plan::WindowCall* call{nullptr};
    int                     argument{-1};
    int                     default_value{-1};
};

/** Row ranges of a sorted batch: partition, ORDER BY peer group, and the end of the non-null ORDER BY keys. */
struct Segments
{
    std::vector<int64_t> partition_begin;
    std::vector<int64_t> partition_end;
    std::vector<int64_t> peer_begin;
    std::vector<int64_t> peer_end;
    std::vector<int64_t> non_null_end;
};

Segments segment(const KeyReaders& keys, const int64_t rows)
{
    Segments segments;
    const auto size = static_cast<std::size_t>(rows);
    segments.partition_begin.resize(size);
    segments.partition_end.resize(size);
    segments.peer_begin.resize(size);
    segments.peer_end.resize(size);
    segments.non_null_end.resize(size);
    int64_t begin = 0;
    while (begin < rows)
    {
        auto end = begin + 1;
        while (end < rows && comparePartition(keys, begin, keys, end) == 0) ++end;
        // Nulls sort last, so the non-null keys of a single-key ORDER BY form a prefix.
        auto non_null = end;
        if (keys.order.size() == 1)
            while (non_null > begin && keys.order.front().at(non_null - 1).kind == ValueKind::NUL) --non_null;
        for (auto peer = begin; peer < end;)
        {
            auto peer_last = peer + 1;
            while (peer_last < end && compareOrder(keys, peer, keys, peer_last) == 0) ++peer_last;
            for (auto row = peer; row < peer_last; ++row)
            {
                const auto index = static_cast<std::size_t>(row);
                segments.partition_begin[index] = begin;
                segments.partition_end[index] = end;
                segments.peer_begin[index] = peer;
                segments.peer_end[index] = peer_last;
                segments.non_null_end[index] = non_null;
            }
            peer = peer_last;
        }
        begin = end;
    }
    return segments;
}

/** Converts a RANGE offset in nanoseconds to the unit of a timestamp / duration key; other keys use it as is. */
int64_t keyOffset(const int64_t offset, const arrow::DataType& type)
{
    arrow::TimeUnit::type unit = arrow::TimeUnit::NANO;
    if (type.id() == arrow::Type::TIMESTAMP) unit = static_cast<const arrow::TimestampType&>(type).unit();
    else if (type.id() == arrow::Type::DURATION) unit = static_cast<const arrow::DurationType&>(type).unit();
    else return offset;
    switch (unit)
    {
    case arrow::TimeUnit::SECOND: return offset / 1'000'000'000;
    case arrow::TimeUnit::MILLI: return offset / 1'000'000;
    case arrow::TimeUnit::MICRO: return offset / 1'000;
    case arrow::TimeUnit::NANO: return offset;
    }
    return offset;
}

/** Frame [begin, end) of every row of a sorted batch. */
struct Frames
{
    std::vector<int64_t> begin;
    std::vector<int64_t> end;
};

Frames frames(const plan::WindowFrameSpec& frame, const Segments& segments, const KeyReaders& keys, const std::shared_ptr<arrow::DataType>& key_type,
              const bool descending, const int64_t rows)
{
    Frames result;
    result.begin.resize(static_cast<std::size_t>(rows));
    result.end.resize(static_cast<std::size_t>(rows));
    const bool ranged = !frame.rows && (frame.start == WindowBoundKind::PRECEDING || frame.start == WindowBoundKind::FOLLOWING ||
                                        frame.end == WindowBoundKind::PRECEDING || frame.end == WindowBoundKind::FOLLOWING);
    const auto start_offset = ranged ? keyOffset(frame.start_offset, *key_type) : frame.start_offset;
    const auto end_offset = ranged ? keyOffset(frame.end_offset, *key_type) : frame.end_offset;
    const auto key = [&keys](const int64_t row) { return keys.order.front().at(row).integer; };
    // First row of the non-null keys whose key is past (strict) or at the target, in sort direction.
    const auto search = [&](const int64_t row, const int64_t target, const bool strict)
    {
        const auto index = static_cast<std::size_t>(row);
        int64_t    low = segments.partition_begin[index];
        int64_t    high = segments.non_null_end[index];
        while (low < high)
        {
            const auto middle = low + (high - low) / 2;
            const auto value = key(middle);
            const bool before = descending ? (strict ? value >= target : value > target) : (strict ? value <= target : value < target);
            if (before) low = middle + 1;
            else high = middle;
        }
        return low;
    };
    // Target key `offset` rows / units before (preceding) or after the current row, in sort direction.
    const auto target = [&](const int64_t row, const int64_t offset, const bool preceding)
    {
        const auto value = key(row);
        return preceding != descending ? saturatingSub(value, offset) : saturatingAdd(value, offset);
    };
    for (int64_t row = 0; row < rows; ++row)
    {
        const auto index = static_cast<std::size_t>(row);
        const auto first = segments.partition_begin[index];
        const auto last = segments.partition_end[index];
        // RANGE offsets over a null key frame the null peers.
        const bool null_key = ranged && row >= segments.non_null_end[index];
        int64_t    begin = first;
        switch (frame.start)
        {
        case WindowBoundKind::UNBOUNDED_PRECEDING: begin = first; break;
        case WindowBoundKind::CURRENT_ROW: begin = frame.rows ? row : segments.peer_begin[index]; break;
        case WindowBoundKind::PRECEDING:
            begin = frame.rows ? std::max(first, saturatingSub(row, start_offset))
                               : (null_key ? segments.peer_begin[index] : search(row, target(row, start_offset, true), false));
            break;
        case WindowBoundKind::FOLLOWING:
            begin = frame.rows ? std::min(last, saturatingAdd(row, start_offset))
                               : (null_key ? segments.peer_begin[index] : search(row, target(row, start_offset, false), false));
            break;
        case WindowBoundKind::UNBOUNDED_FOLLOWING: begin = last; break;
        }
        int64_t end = last;
        switch (frame.end)
        {
        case WindowBoundKind::UNBOUNDED_FOLLOWING: end = last; break;
        case WindowBoundKind::CURRENT_ROW: end = frame.rows ? row + 1 : segments.peer_end[index]; break;
        case WindowBoundKind::PRECEDING:
            end = frame.rows ? saturatingAdd(saturatingSub(row, end_offset), 1)
                             : (null_key ? segments.peer_end[index] : search(row, target(row, end_offset, true), true));
            break;
        case WindowBoundKind::FOLLOWING:
            end = frame.rows ? saturatingAdd(saturatingAdd(row, end_offset), 1)
                             : (null_key ? segments.peer_end[index] : search(row, target(row, end_offset, false), true));
            break;
        case WindowBoundKind::UNBOUNDED_PRECEDING: end = first; break;
        }
        begin = std::clamp(begin, first, last);
        end = std::clamp(end, begin, last);
        result.begin[index] = begin;
        result.end[index] = end;
    }
    return result;
}

std::shared_ptr<arrow::Array> int64Column(const std::vector<int64_t>& values, arrow::MemoryPool* pool)
{
    arrow::Int64Builder builder(pool);
    check(builder.AppendValues(values), "build int64 column");
    std::shared_ptr<arrow::Array> array;
    check(builder.Finish(&array), "build int64 column");
    return array;
}

bool integerType(const arrow::DataType& type)
{
    return arrow::is_integer(type.id()) || type.id() == arrow::Type::TIMESTAMP || type.id() == arrow::Type::DURATION || type.id() == arrow::Type::NA;
}

/** SUM / AVG / COUNT over each row's frame, adding rows entering and removing rows leaving the frame. */
std::shared_ptr<arrow::Array> slidingSum(const plan::WindowCall& call, const std::shared_ptr<arrow::Array>& argument, const Segments& segments,
                                         const Frames& frame, const int64_t rows, arrow::MemoryPool* pool)
{
    const bool count = call.function == "count";
    const bool average = call.function == "avg";
    const bool integer = argument == nullptr || integerType(*argument->type());
    std::optional<ColumnReader> reader;
    if (argument) reader.emplace(*argument);
    int64_t integer_sum = 0;
    double  double_sum = 0;
    int64_t values = 0;
    const auto apply = [&](const int64_t row, const int sign)
    {
        if (!reader)
        {
            values += sign;
            return;
        }
        const auto value = reader->at(row);
        if (value.kind == ValueKind::NUL) return;
        values += sign;
        if (count) return;
        if (!value.numeric()) fail(std::string(average ? "AVG" : "SUM") + " requires numeric values");
        if (value.kind == ValueKind::INT) integer_sum += sign * value.integer;
        double_sum += sign * value.asDouble();
    };
    arrow::Int64Builder  integers(pool);
    arrow::DoubleBuilder doubles(pool);
    int64_t              low = 0;
    int64_t              high = 0;
    for (int64_t row = 0; row < rows; ++row)
    {
        const auto index = static_cast<std::size_t>(row);
        if (row == segments.partition_begin[index])
        {
            low = high = row;
            integer_sum = 0;
            double_sum = 0;
            values = 0;
        }
        for (; high < frame.end[index]; ++high) apply(high, 1);
        for (; low < frame.begin[index]; ++low) apply(low, -1);
        if (count) check(integers.Append(values), "build COUNT");
        else if (values == 0) check(average ? doubles.AppendNull() : (integer ? integers.AppendNull() : doubles.AppendNull()), "build SUM");
        else if (average) check(doubles.Append(integer ? static_cast<double>(integer_sum) / static_cast<double>(values) : double_sum / static_cast<double>(values)), "build AVG");
        else if (integer) check(integers.Append(integer_sum), "build SUM");
        else check(doubles.Append(double_sum), "build SUM");
    }
    std::shared_ptr<arrow::Array> array;
    check(count || (!average && integer) ? integers.Finish(&array) : doubles.Finish(&array), "finish window aggregate");
    return array;
}

/** MIN / MAX over each row's frame with a monotonic deque of candidate rows. */
std::shared_ptr<arrow::Array> slidingChoice(const bool minimum, const std::shared_ptr<arrow::Array>& argument, const Segments& segments,
                                            const Frames& frame, const int64_t rows)
{
    const ColumnReader   reader(*argument);
    std::deque<int64_t>  candidates;
    std::vector<int64_t> chosen(static_cast<std::size_t>(rows), -1);
    int64_t              low = 0;
    int64_t              high = 0;
    for (int64_t row = 0; row < rows; ++row)
    {
        const auto index = static_cast<std::size_t>(row);
        if (row == segments.partition_begin[index])
        {
            low = high = row;
            candidates.clear();
        }
        for (; high < frame.end[index]; ++high)
        {
            const auto value = reader.at(high);
            if (value.kind == ValueKind::NUL) continue;
            // Earlier rows that are not better than the new one can never be chosen again.
            while (!candidates.empty())
            {
                const auto order = compareRefs(reader.at(candidates.back()), value);
                if ((minimum && order < 0) || (!minimum && order > 0)) break;
                candidates.pop_back();
            }
            candidates.push_back(high);
        }
        low = frame.begin[index];
        while (!candidates.empty() && candidates.front() < low) candidates.pop_front();
        if (!candidates.empty()) chosen[index] = candidates.front();
    }
    return take(argument, chosen);
}

} // namespace

namespace mldp_pvxs_driver::query::executor {

/** Computes one window group: buffers (and spills) its input, then emits it sorted with the call columns appended. */
class WindowGroupOperator
{
public:
    WindowGroupOperator(plan::WindowGroup group, const bool sorted_input, ExecutionContext context)
        : group_(std::move(group)), sorted_input_(sorted_input), context_(std::move(context)),
          budget_(context_.memory_limit_bytes != 0 ? static_cast<std::size_t>(context_.memory_limit_bytes) : kDefaultMemoryBudget)
    {
    }

    void consume(const std::shared_ptr<arrow::RecordBatch>& batch)
    {
        throwIfCancelled();
        if (!batch || batch->num_rows() == 0) return;
        auto evaluated = evaluate(batch);
        buffered_bytes_ += static_cast<std::size_t>(arrow::util::TotalBufferSize(*evaluated));
        buffered_.push_back(std::move(evaluated));
        rows_ += batch->num_rows();
        if (context_.spill && buffered_bytes_ > budget_) spillRun();
        if (context_.progress)
            context_.progress->setActivity({}, "window", std::to_string(rows_) + " rows" + (runs_.empty() ? "" : " (spilling)"));
    }

    void finishInput()
    {
        if (!runs_.empty())
        {
            if (!buffered_.empty()) spillRun();
            for (const auto& run : runs_)
            {
                RunCursor cursor{.reader = check(context_.spill->read(run), "read window spill run")};
                if (advanceBatch(cursor)) cursors_.push_back(std::move(cursor));
            }
            merging_ = true;
            return;
        }
        if (buffered_.empty()) return;
        const auto combined = combine(std::move(buffered_));
        buffered_.clear();
        const auto sorted = takeRows(combined, sortPermutation(*combined, layout_, sorted_input_));
        const auto output = evaluateSorted(sorted);
        for (int64_t offset = 0; offset < output->num_rows(); offset += kOutputBatchRows)
            output_.push_back(output->Slice(offset, std::min(kOutputBatchRows, output->num_rows() - offset)));
    }

    std::shared_ptr<arrow::RecordBatch> next()
    {
        if (merging_) return nextMerged();
        return next_ < output_.size() ? std::move(output_[next_++]) : nullptr;
    }

private:
    struct RunCursor
    {
        SpillReader                         reader;
        std::shared_ptr<arrow::RecordBatch> batch;
        std::unique_ptr<KeyReaders>         keys;
        int64_t                             row{0};
        std::size_t                         row_bytes{0};
    };

    struct Slice
    {
        std::shared_ptr<arrow::RecordBatch> batch;
        int64_t                             offset{0};
        int64_t                             length{0};
    };

    void throwIfCancelled() const
    {
        if (context_.cancellation) context_.cancellation->throwIfCancelled();
    }

    /** Appends the evaluated key and argument columns to an input batch. */
    std::shared_ptr<arrow::RecordBatch> evaluate(const std::shared_ptr<arrow::RecordBatch>& batch)
    {
        if (input_columns_ < 0) prepare(batch->num_columns());
        if (expressions_.empty()) return batch;
        const auto evaluated = applyProjection(RecordBatches{batch}, expressions_, names_);
        auto       fields = batch->schema()->fields();
        auto       arrays = batch->columns();
        for (int index = 0; index < evaluated.front()->num_columns(); ++index)
        {
            fields.push_back(evaluated.front()->schema()->field(index));
            arrays.push_back(evaluated.front()->column(index));
        }
        return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), batch->num_rows(), std::move(arrays));
    }

    void prepare(const int input_columns)
    {
        input_columns_ = input_columns;
        const auto add = [this](const ExpressionPtr& expression)
        {
            expressions_.push_back(expression);
            names_.push_back("__window_input_" + std::to_string(names_.size()));
            return input_columns_ + static_cast<int>(expressions_.size()) - 1;
        };
        for (const auto& key : group_.partition_by) layout_.partition.push_back(add(key));
        for (const auto& key : group_.order_by)
        {
            layout_.order.push_back(add(key.expression));
            layout_.descending.push_back(key.descending);
        }
        for (const auto& call : group_.calls)
        {
            CallLayout layout{.call = &call};
            if (call.argument) layout.argument = add(call.argument);
            if (call.default_value) layout.default_value = add(call.default_value);
            calls_.push_back(layout);
        }
    }

    /** Sorts the buffered input into one spill run. */
    void spillRun()
    {
        const auto combined = combine(std::move(buffered_));
        buffered_.clear();
        buffered_bytes_ = 0;
        const auto sorted = takeRows(combined, sortPermutation(*combined, layout_, sorted_input_));
        auto       writer = check(context_.spill->openWriter("window-run-" + std::to_string(runs_.size()), sorted->schema()), "open window spill run");
        for (int64_t offset = 0; offset < sorted->num_rows(); offset += kOutputBatchRows)
            check(writer.append(sorted->Slice(offset, std::min(kOutputBatchRows, sorted->num_rows() - offset))), "write window spill run");
        runs_.push_back(check(writer.finish(), "finish window spill run"));
    }

    /** Moves @p cursor to the first row of its next non-empty batch; false at the end of the run. */
    bool advanceBatch(RunCursor& cursor)
    {
        while (true)
        {
            auto batch = check(cursor.reader.next(), "read window spill run");
            if (!batch)
            {
                cursor.batch.reset();
                cursor.keys.reset();
                return false;
            }
            if (batch->num_rows() == 0) continue;
            cursor.batch = std::move(batch);
            cursor.keys = std::make_unique<KeyReaders>(*cursor.batch, layout_);
            cursor.row = 0;
            cursor.row_bytes = static_cast<std::size_t>(arrow::util::TotalBufferSize(*cursor.batch)) / static_cast<std::size_t>(cursor.batch->num_rows()) + 1;
            return true;
        }
    }

    /** Merges the sorted runs and evaluates one partition at a time. */
    std::shared_ptr<arrow::RecordBatch> nextMerged()
    {
        while (true)
        {
            if (pending_rows_ >= kOutputBatchRows) return flushPending();
            RunCursor* best = nullptr;
            for (auto& cursor : cursors_)
            {
                if (!cursor.batch) continue;
                if (best == nullptr || compareRows(*cursor.keys, cursor.row, *best->keys, best->row) < 0) best = &cursor;
            }
            if (best == nullptr)
            {
                closePartition();
                return pending_rows_ > 0 ? flushPending() : nullptr;
            }
            if (!partition_.empty())
            {
                const auto& last = partition_.back();
                if (last_keys_batch_ != last.batch)
                {
                    last_keys_ = std::make_unique<KeyReaders>(*last.batch, layout_);
                    last_keys_batch_ = last.batch;
                }
                if (comparePartition(*best->keys, best->row, *last_keys_, last.offset + last.length - 1) != 0) closePartition();
            }
            if (!partition_.empty() && partition_.back().batch == best->batch && partition_.back().offset + partition_.back().length == best->row)
                ++partition_.back().length;
            else
                partition_.push_back(Slice{.batch = best->batch, .offset = best->row, .length = 1});
            partition_bytes_ += best->row_bytes;
            if (partition_bytes_ > budget_)
                fail("a single partition exceeds memory_limit_bytes (" + std::to_string(budget_) + " bytes); use a finer PARTITION BY or raise the limit");
            if (++best->row >= best->batch->num_rows()) advanceBatch(*best);
        }
    }

    void closePartition()
    {
        if (partition_.empty()) return;
        throwIfCancelled();
        std::vector<std::shared_ptr<arrow::RecordBatch>> slices;
        slices.reserve(partition_.size());
        for (const auto& slice : partition_) slices.push_back(slice.batch->Slice(slice.offset, slice.length));
        partition_.clear();
        partition_bytes_ = 0;
        auto output = evaluateSorted(combine(std::move(slices)));
        pending_rows_ += output->num_rows();
        pending_.push_back(std::move(output));
    }

    std::shared_ptr<arrow::RecordBatch> flushPending()
    {
        auto output = combine(std::move(pending_));
        pending_.clear();
        pending_rows_ = 0;
        return output;
    }

    /** Computes every call over a batch sorted by partition and order keys and holding whole partitions. */
    std::shared_ptr<arrow::RecordBatch> evaluateSorted(const std::shared_ptr<arrow::RecordBatch>& sorted) const
    {
        throwIfCancelled();
        const auto rows = sorted->num_rows();
        const KeyReaders keys(*sorted, layout_);
        const auto       segments = segment(keys, rows);
        auto             fields = sorted->schema()->fields();
        auto             arrays = sorted->columns();
        fields.resize(static_cast<std::size_t>(input_columns_));
        arrays.resize(static_cast<std::size_t>(input_columns_));
        for (const auto& layout : calls_)
        {
            const auto& call = *layout.call;
            const auto  argument = layout.argument >= 0 ? sorted->column(layout.argument) : nullptr;
            auto        array = evaluateCall(call, argument, layout.default_value >= 0 ? sorted->column(layout.default_value) : nullptr, segments, keys, sorted, rows);
            fields.push_back(arrow::field(call.name, array->type()));
            arrays.push_back(std::move(array));
        }
        return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), rows, std::move(arrays));
    }

    std::shared_ptr<arrow::Array> evaluateCall(const plan::WindowCall& call, const std::shared_ptr<arrow::Array>& argument,
                                               const std::shared_ptr<arrow::Array>& default_value, const Segments& segments, const KeyReaders& keys,
                                               const std::shared_ptr<arrow::RecordBatch>& sorted, const int64_t rows) const
    {
        const auto&          function = call.function;
        std::vector<int64_t> values(static_cast<std::size_t>(rows));
        if (function == "row_number" || function == "rank" || function == "dense_rank")
        {
            int64_t dense = 0;
            for (int64_t row = 0; row < rows; ++row)
            {
                const auto index = static_cast<std::size_t>(row);
                const auto first = segments.partition_begin[index];
                if (row == first) dense = 0;
                if (row == segments.peer_begin[index]) ++dense;
                values[index] = function == "row_number" ? row - first + 1 : function == "rank" ? segments.peer_begin[index] - first + 1 : dense;
            }
            return int64Column(values, context_.pool);
        }
        if (function == "lag" || function == "lead")
        {
            for (int64_t row = 0; row < rows; ++row)
            {
                const auto index = static_cast<std::size_t>(row);
                const auto target = function == "lag" ? saturatingSub(row, call.offset) : saturatingAdd(row, call.offset);
                values[index] = target >= segments.partition_begin[index] && target < segments.partition_end[index] ? target : -1;
            }
            if (!default_value) return take(argument, values);
            // Rows without a target take the default, evaluated on the current row.
            const ColumnReader      source(*argument);
            const ColumnReader      fallback(*default_value);
            std::vector<OwnedValue> chosen;
            chosen.reserve(values.size());
            for (int64_t row = 0; row < rows; ++row)
            {
                const auto target = values[static_cast<std::size_t>(row)];
                chosen.push_back(OwnedValue::from(target >= 0 ? source.at(target) : fallback.at(row)));
            }
            return buildValueColumn(chosen, preservedType(argument->type()), context_.pool);
        }

        const auto key_type = layout_.order.empty() ? arrow::int64() : sorted->column(layout_.order.front())->type();
        const auto frame = frames(call.frame, segments, keys, key_type, !layout_.descending.empty() && layout_.descending.front(), rows);
        if (function == "first_value" || function == "last_value")
        {
            for (int64_t row = 0; row < rows; ++row)
            {
                const auto index = static_cast<std::size_t>(row);
                values[index] = frame.end[index] <= frame.begin[index] ? -1 : (function == "first_value" ? frame.begin[index] : frame.end[index] - 1);
            }
            return take(argument, values);
        }
        if (function == "min" || function == "max") return slidingChoice(function == "min", argument, segments, frame, rows);
        if (function == "sum" || function == "avg" || function == "count") return slidingSum(call, argument, segments, frame, rows, context_.pool);
        fail("unsupported window function '" + function + "'");
    }

    plan::WindowGroup                                group_; ///< Owned: calls_ points into it.
    bool                                             sorted_input_;
    ExecutionContext                                 context_;
    std::size_t                                      budget_;
    int                                              input_columns_{-1};
    std::vector<ExpressionPtr>                       expressions_;
    std::vector<std::string>                         names_;
    KeyLayout                                        layout_;
    std::vector<CallLayout>                          calls_;
    std::vector<std::shared_ptr<arrow::RecordBatch>> buffered_;
    std::size_t                                      buffered_bytes_{0};
    int64_t                                          rows_{0};
    std::vector<SpillHandle>                         runs_;
    std::vector<std::shared_ptr<arrow::RecordBatch>> output_;
    std::size_t                                      next_{0};
    bool                                             merging_{false};
    std::vector<RunCursor>                           cursors_;
    std::vector<Slice>                               partition_;
    std::shared_ptr<arrow::RecordBatch>              last_keys_batch_; ///< Batch last_keys_ reads.
    std::unique_ptr<KeyReaders>                      last_keys_;       ///< Key readers of the partition's last row batch.
    std::size_t                                      partition_bytes_{0};
    std::vector<std::shared_ptr<arrow::RecordBatch>> pending_;
    int64_t                                          pending_rows_{0};
};

} // namespace mldp_pvxs_driver::query::executor

WindowOperator::WindowOperator(const plan::PhysicalWindow& node, ExecutionContext context)
{
    for (std::size_t index = 0; index < node.groups.size(); ++index)
        groups_.push_back(std::make_unique<WindowGroupOperator>(node.groups[index], index == 0 && node.sorted_input, context));
}

WindowOperator::~WindowOperator() = default;

void WindowOperator::consume(const std::shared_ptr<arrow::RecordBatch>& batch)
{
    if (groups_.empty()) fail("window node without groups");
    groups_.front()->consume(batch);
}

std::shared_ptr<arrow::RecordBatch> WindowOperator::next()
{
    if (groups_.empty()) return nullptr;
    if (!finished_)
    {
        finished_ = true;
        groups_.front()->finishInput();
        // Each later group re-sorts the previous group's output for its own keys.
        for (std::size_t index = 1; index < groups_.size(); ++index)
        {
            while (auto batch = groups_[index - 1]->next()) groups_[index]->consume(batch);
            groups_[index]->finishInput();
        }
    }
    return groups_.back()->next();
}

RecordBatches mldp_pvxs_driver::query::executor::applyWindow(const RecordBatches& input, const plan::PhysicalWindow& node, const ExecutionContext& context)
{
    WindowOperator window(node, context);
    for (const auto& batch : input) window.consume(batch);
    RecordBatches output;
    while (auto batch = window.next()) output.push_back(std::move(batch));
    return output;
}
