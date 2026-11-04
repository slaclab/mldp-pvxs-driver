//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/AggregateRegistry.h>

#include <query/executor/AggregateValue.h>
#include <query/plan/PlannerError.h>

#include <arrow/array/builder_primitive.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_set>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

namespace {

std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

[[noreturn]] void bindError(const std::string& message)
{
    throw plan::PlannerException(plan::BindError{.message = message});
}

template <typename Builder>
std::shared_ptr<arrow::Array> finishBuilder(Builder& builder)
{
    std::shared_ptr<arrow::Array> array;
    if (const auto status = builder.Finish(&array); !status.ok())
        throw std::runtime_error("GROUP BY failed to build aggregate column: " + status.ToString());
    return array;
}

// ---------------------------------------------------------------------------
// Accumulators
// ---------------------------------------------------------------------------

/** COUNT(*), COUNT(x), COUNT(DISTINCT x). */
class CountAccumulator final : public IAggregateAccumulator
{
public:
    explicit CountAccumulator(const bool distinct) : distinct_(distinct) {}

    void resize(const std::size_t groups) override
    {
        if (counts_.size() < groups) counts_.resize(groups, 0);
        if (distinct_ && seen_.size() < groups) seen_.resize(groups);
    }

    void consume(const arrow::Array* values, const uint32_t* group_ids, const int64_t rows) override
    {
        if (values == nullptr)
        {
            for (int64_t row = 0; row < rows; ++row) ++counts_[group_ids[row]];
            return;
        }
        const ColumnReader reader(*values);
        std::string key;
        for (int64_t row = 0; row < rows; ++row)
        {
            const auto value = reader.at(row);
            if (value.kind == ValueKind::NUL) continue;
            if (!distinct_)
            {
                ++counts_[group_ids[row]];
                continue;
            }
            key.clear();
            appendValueKey(key, value);
            if (seen_[group_ids[row]].insert(key).second)
            {
                ++counts_[group_ids[row]];
                bytes_ += key.size() + 32;
            }
        }
    }

    void merge(IAggregateAccumulator& other_base, const std::vector<uint32_t>& mapping) override
    {
        auto& other = static_cast<CountAccumulator&>(other_base);
        for (std::size_t group = 0; group < other.counts_.size(); ++group)
        {
            const auto target = mapping[group];
            if (target == kSkipGroup) continue;
            if (!distinct_)
            {
                counts_[target] += other.counts_[group];
                continue;
            }
            for (auto& key : other.seen_[group])
                if (seen_[target].insert(key).second)
                {
                    ++counts_[target];
                    bytes_ += key.size() + 32;
                }
        }
    }

    std::shared_ptr<arrow::Array> finish(arrow::MemoryPool* pool) override
    {
        arrow::Int64Builder builder(pool);
        if (!builder.AppendValues(counts_).ok()) throw std::runtime_error("GROUP BY failed to build COUNT");
        return finishBuilder(builder);
    }

    std::size_t memoryBytes() const override { return counts_.size() * sizeof(int64_t) + seen_.size() * 56 + bytes_; }

private:
    bool                                         distinct_;
    std::vector<int64_t>                         counts_;
    std::vector<std::unordered_set<std::string>> seen_;
    std::size_t                                  bytes_{0};
};

/** SUM(x) and AVG(x): exact int64 sums while every value is an integer, double otherwise. */
class SumAccumulator final : public IAggregateAccumulator
{
public:
    SumAccumulator(const bool average, std::string name) : average_(average), name_(std::move(name)) {}

    void resize(const std::size_t groups) override
    {
        if (counts_.size() >= groups) return;
        counts_.resize(groups, 0);
        integer_sums_.resize(groups, 0);
        double_sums_.resize(groups, 0);
    }

    void consume(const arrow::Array* values, const uint32_t* group_ids, const int64_t rows) override
    {
        const ColumnReader reader(*values);
        for (int64_t row = 0; row < rows; ++row)
        {
            const auto value = reader.at(row);
            if (value.kind == ValueKind::NUL) continue;
            if (!value.numeric()) throw std::runtime_error(name_ + " requires numeric values");
            const auto group = group_ids[row];
            if (value.kind == ValueKind::DOUBLE) floating_ = true;
            else integer_sums_[group] += value.integer;
            double_sums_[group] += value.asDouble();
            ++counts_[group];
        }
    }

    void merge(IAggregateAccumulator& other_base, const std::vector<uint32_t>& mapping) override
    {
        auto& other = static_cast<SumAccumulator&>(other_base);
        floating_ |= other.floating_;
        for (std::size_t group = 0; group < other.counts_.size(); ++group)
        {
            const auto target = mapping[group];
            if (target == kSkipGroup) continue;
            counts_[target] += other.counts_[group];
            integer_sums_[target] += other.integer_sums_[group];
            double_sums_[target] += other.double_sums_[group];
        }
    }

    std::shared_ptr<arrow::Array> finish(arrow::MemoryPool* pool) override
    {
        if (!average_ && !floating_)
        {
            arrow::Int64Builder builder(pool);
            for (std::size_t group = 0; group < counts_.size(); ++group)
                if (!(counts_[group] == 0 ? builder.AppendNull() : builder.Append(integer_sums_[group])).ok())
                    throw std::runtime_error("GROUP BY failed to build " + name_);
            return finishBuilder(builder);
        }
        arrow::DoubleBuilder builder(pool);
        for (std::size_t group = 0; group < counts_.size(); ++group)
        {
            const auto status = counts_[group] == 0 ? builder.AppendNull()
                                                    : builder.Append(average_ ? double_sums_[group] / static_cast<double>(counts_[group]) : double_sums_[group]);
            if (!status.ok()) throw std::runtime_error("GROUP BY failed to build " + name_);
        }
        return finishBuilder(builder);
    }

    std::size_t memoryBytes() const override { return counts_.size() * (2 * sizeof(int64_t) + sizeof(double)); }

private:
    bool                 average_;
    std::string          name_;
    bool                 floating_{false};
    std::vector<int64_t> counts_;
    std::vector<int64_t> integer_sums_;
    std::vector<double>  double_sums_;
};

/** MIN, MAX, FIRST, LAST: keep one chosen value per group. */
class ChoiceAccumulator final : public IAggregateAccumulator
{
public:
    enum class Mode { MIN, MAX, FIRST, LAST };

    ChoiceAccumulator(const Mode mode, std::shared_ptr<arrow::DataType> type) : mode_(mode), type_(preservedType(type)) {}

    void resize(const std::size_t groups) override
    {
        if (chosen_.size() < groups) chosen_.resize(groups);
    }

    void consume(const arrow::Array* values, const uint32_t* group_ids, const int64_t rows) override
    {
        const ColumnReader reader(*values);
        for (int64_t row = 0; row < rows; ++row)
        {
            const auto value = reader.at(row);
            if (value.kind == ValueKind::NUL) continue;
            offer(chosen_[group_ids[row]], value);
        }
    }

    void merge(IAggregateAccumulator& other_base, const std::vector<uint32_t>& mapping) override
    {
        auto& other = static_cast<ChoiceAccumulator&>(other_base);
        for (std::size_t group = 0; group < other.chosen_.size(); ++group)
        {
            auto& candidate = other.chosen_[group];
            if (candidate.kind == ValueKind::NUL || mapping[group] == kSkipGroup) continue;
            auto& current = chosen_[mapping[group]];
            if (current.kind == ValueKind::NUL || mode_ == Mode::LAST || (mode_ != Mode::FIRST && better(candidate, current)))
            {
                bytes_ += candidate.heapBytes();
                current = std::move(candidate);
            }
        }
    }

    std::shared_ptr<arrow::Array> finish(arrow::MemoryPool* pool) override { return buildValueColumn(chosen_, type_, pool); }

    std::size_t memoryBytes() const override { return chosen_.size() * sizeof(OwnedValue) + bytes_; }

private:
    bool better(const OwnedValue& candidate, const OwnedValue& current) const
    {
        ValueRef view;
        view.kind = candidate.kind;
        view.integer = candidate.integer;
        view.real = candidate.real;
        view.boolean = candidate.boolean;
        view.text = candidate.text;
        const auto order = compareValues(view, current);
        return mode_ == Mode::MIN ? order < 0 : order > 0;
    }

    void offer(OwnedValue& current, const ValueRef& value)
    {
        bool take = current.kind == ValueKind::NUL;
        if (!take)
        {
            if (mode_ == Mode::FIRST) return;
            if (mode_ == Mode::LAST) take = true;
            else
            {
                const auto order = compareValues(value, current);
                take = mode_ == Mode::MIN ? order < 0 : order > 0;
            }
        }
        if (!take) return;
        if (value.kind == ValueKind::STRING && current.kind == ValueKind::STRING)
        {
            current.text.assign(value.text); // reuse the existing buffer
            return;
        }
        current = OwnedValue::from(value);
        bytes_ += current.heapBytes();
    }

    Mode                             mode_;
    std::shared_ptr<arrow::DataType> type_;
    std::vector<OwnedValue>          chosen_;
    std::size_t                      bytes_{0};
};

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

class AggregateFunctionBase : public IAggregateFunction
{
public:
    explicit AggregateFunctionBase(ExpressionCallableDescriptor descriptor) : descriptor_(std::move(descriptor)) {}
    const ExpressionCallableDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    void requireArgument(const std::optional<ColumnType>& argument, const bool distinct) const
    {
        if (!argument) bindError("Only COUNT accepts '*'");
        if (distinct) bindError("DISTINCT is supported only in COUNT(DISTINCT expr)");
    }

private:
    ExpressionCallableDescriptor descriptor_;
};

class CountFunction final : public AggregateFunctionBase
{
public:
    CountFunction()
        : AggregateFunctionBase({"count", ExpressionCallableKind::AGGREGATE, {}, ColumnType::INT,
                                 "Count rows (*), non-null values, or distinct non-null values.", "COUNT(*), COUNT(DISTINCT pv)", "(* | any | DISTINCT any)", "int"})
    {
    }
    ColumnType bind(std::optional<ColumnType>, bool) const override { return ColumnType::INT; }
    std::unique_ptr<IAggregateAccumulator> makeAccumulator(const std::shared_ptr<arrow::DataType>&, const bool distinct) const override
    {
        return std::make_unique<CountAccumulator>(distinct);
    }
};

class SumFunction final : public AggregateFunctionBase
{
public:
    explicit SumFunction(const bool average)
        : AggregateFunctionBase(average ? ExpressionCallableDescriptor{"avg", ExpressionCallableKind::AGGREGATE, {}, ColumnType::INT,
                                                                       "Average of non-null numeric values.", "AVG(value)", "(numeric)", "double"}
                                        : ExpressionCallableDescriptor{"sum", ExpressionCallableKind::AGGREGATE, {}, ColumnType::INT,
                                                                       "Sum of non-null numeric values.", "SUM(value)", "(numeric)", "int | double"}),
          average_(average)
    {
    }
    ColumnType bind(const std::optional<ColumnType> argument, const bool distinct) const override
    {
        requireArgument(argument, distinct);
        if (*argument != ColumnType::INT && *argument != ColumnType::NATIVE_VALUE && *argument != ColumnType::DURATION_SECONDS)
            bindError("Aggregate function '" + descriptor().name + "' requires a numeric argument");
        return ColumnType::INT;
    }
    std::unique_ptr<IAggregateAccumulator> makeAccumulator(const std::shared_ptr<arrow::DataType>&, bool) const override
    {
        return std::make_unique<SumAccumulator>(average_, average_ ? "AVG" : "SUM");
    }

private:
    bool average_;
};

class ChoiceFunction final : public AggregateFunctionBase
{
public:
    ChoiceFunction(const ChoiceAccumulator::Mode mode, ExpressionCallableDescriptor descriptor)
        : AggregateFunctionBase(std::move(descriptor)), mode_(mode)
    {
    }
    ColumnType bind(const std::optional<ColumnType> argument, const bool distinct) const override
    {
        requireArgument(argument, distinct);
        return *argument;
    }
    std::unique_ptr<IAggregateAccumulator> makeAccumulator(const std::shared_ptr<arrow::DataType>& input, bool) const override
    {
        return std::make_unique<ChoiceAccumulator>(mode_, input);
    }

private:
    ChoiceAccumulator::Mode mode_;
};

} // namespace

const AggregateRegistry& AggregateRegistry::instance()
{
    static const AggregateRegistry registry;
    return registry;
}

AggregateRegistry::AggregateRegistry()
{
    using Mode = ChoiceAccumulator::Mode;
    const auto choice = [this](const Mode mode, std::string name, std::string description, std::string example)
    {
        functions_.push_back(std::make_unique<ChoiceFunction>(
            mode, ExpressionCallableDescriptor{std::move(name), ExpressionCallableKind::AGGREGATE, {}, ColumnType::STRING,
                                               std::move(description), std::move(example), "(any)", "input type"}));
    };
    functions_.push_back(std::make_unique<CountFunction>());
    functions_.push_back(std::make_unique<SumFunction>(false));
    functions_.push_back(std::make_unique<SumFunction>(true));
    choice(Mode::MIN, "min", "Smallest non-null value (numbers, strings, timestamps).", "MIN(time)");
    choice(Mode::MAX, "max", "Largest non-null value (numbers, strings, timestamps).", "MAX(value)");
    choice(Mode::FIRST, "first", "First non-null value in input order.", "FIRST(description)");
    choice(Mode::LAST, "last", "Last non-null value in input order.", "LAST(value)");
}

const IAggregateFunction* AggregateRegistry::find(const std::string_view name) const
{
    const auto wanted = lower(name);
    for (const auto& function : functions_)
        if (function->descriptor().name == wanted) return function.get();
    return nullptr;
}

std::vector<ExpressionCallableDescriptor> AggregateRegistry::functions() const
{
    std::vector<ExpressionCallableDescriptor> result;
    for (const auto& function : functions_) result.push_back(function->descriptor());
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.name < rhs.name; });
    return result;
}
