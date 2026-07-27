//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/ExpressionRegistry.h>

#include <query/plan/PlannerError.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

using namespace mldp_pvxs_driver::query;

namespace {

std::string normalized(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

class Callable : public IExpressionCallable
{
public:
    explicit Callable(ExpressionCallableDescriptor descriptor)
        : descriptor_(std::move(descriptor))
    {
    }

    const ExpressionCallableDescriptor& descriptor() const noexcept override { return descriptor_; }

private:
    ExpressionCallableDescriptor descriptor_;
};

// Concrete types deliberately make each language feature independently addressable.
class ToUtcFunction final : public Callable { public: ToUtcFunction(std::vector<ColumnType> args, std::string example) : Callable({"to_utc", ExpressionCallableKind::FUNCTION, std::move(args), ColumnType::TIMESTAMP, "Convert an ISO-8601 timestamp to UTC epoch seconds.", std::move(example)}) {} };
class FromUtcFunction final : public Callable { public: FromUtcFunction() : Callable({"from_utc", ExpressionCallableKind::FUNCTION, {ColumnType::TIMESTAMP, ColumnType::STRING}, ColumnType::STRING, "Format a UTC timestamp in an IANA timezone or fixed UTC offset.", "from_utc(time, 'America/Los_Angeles')"}) {} };
class AddOperator final : public Callable { public: AddOperator(std::vector<ColumnType> args, ColumnType result, std::string example) : Callable({"+", ExpressionCallableKind::BINARY_OPERATOR, std::move(args), result, "Add numeric values or a duration to a timestamp.", std::move(example)}) {} };
class SubtractOperator final : public Callable { public: SubtractOperator(std::vector<ColumnType> args, ColumnType result, std::string example) : Callable({"-", ExpressionCallableKind::BINARY_OPERATOR, std::move(args), result, "Subtract numeric values, timestamps, or durations.", std::move(example)}) {} };
class MultiplyOperator final : public Callable { public: MultiplyOperator() : Callable({"*", ExpressionCallableKind::BINARY_OPERATOR, {ColumnType::INT, ColumnType::INT}, ColumnType::INT, "Multiply numeric values.", "2 * 3"}) {} };
class DivideOperator final : public Callable { public: DivideOperator() : Callable({"/", ExpressionCallableKind::BINARY_OPERATOR, {ColumnType::INT, ColumnType::INT}, ColumnType::INT, "Divide numeric values.", "6 / 2"}) {} };
class EqualOperator final : public Callable { public: EqualOperator(ColumnType type) : Callable({"=", ExpressionCallableKind::BINARY_OPERATOR, {type, type}, ColumnType::BOOL, "Test values for equality.", "value = 1"}) {} };
class NotEqualOperator final : public Callable { public: NotEqualOperator(ColumnType type) : Callable({"!=", ExpressionCallableKind::BINARY_OPERATOR, {type, type}, ColumnType::BOOL, "Test values for inequality.", "value != 1"}) {} };
class LessOperator final : public Callable { public: LessOperator(ColumnType type) : Callable({"<", ExpressionCallableKind::BINARY_OPERATOR, {type, type}, ColumnType::BOOL, "Test whether the left value is smaller.", "time < timestamp_ns(1)"}) {} };
class LessEqualOperator final : public Callable { public: LessEqualOperator(ColumnType type) : Callable({"<=", ExpressionCallableKind::BINARY_OPERATOR, {type, type}, ColumnType::BOOL, "Test whether the left value is not greater.", "value <= 1"}) {} };
class GreaterOperator final : public Callable { public: GreaterOperator(ColumnType type) : Callable({">", ExpressionCallableKind::BINARY_OPERATOR, {type, type}, ColumnType::BOOL, "Test whether the left value is greater.", "value > 1"}) {} };
class GreaterEqualOperator final : public Callable { public: GreaterEqualOperator(ColumnType type) : Callable({">=", ExpressionCallableKind::BINARY_OPERATOR, {type, type}, ColumnType::BOOL, "Test whether the left value is not smaller.", "value >= 1"}) {} };
class AndOperator final : public Callable { public: AndOperator() : Callable({"AND", ExpressionCallableKind::BINARY_OPERATOR, {ColumnType::BOOL, ColumnType::BOOL}, ColumnType::BOOL, "Combine boolean values with logical AND.", "true AND false"}) {} };
class OrOperator final : public Callable { public: OrOperator() : Callable({"OR", ExpressionCallableKind::BINARY_OPERATOR, {ColumnType::BOOL, ColumnType::BOOL}, ColumnType::BOOL, "Combine boolean values with logical OR.", "true OR false"}) {} };
class NotOperator final : public Callable { public: NotOperator() : Callable({"NOT", ExpressionCallableKind::UNARY_OPERATOR, {ColumnType::BOOL}, ColumnType::BOOL, "Negate a boolean value.", "NOT true"}) {} };
class UnaryPlusOperator final : public Callable { public: UnaryPlusOperator() : Callable({"+", ExpressionCallableKind::UNARY_OPERATOR, {ColumnType::INT}, ColumnType::INT, "Return a numeric value unchanged.", "+1"}) {} };
class UnaryMinusOperator final : public Callable { public: UnaryMinusOperator() : Callable({"-", ExpressionCallableKind::UNARY_OPERATOR, {ColumnType::INT}, ColumnType::INT, "Negate a numeric value.", "-1"}) {} };

} // namespace

bool IExpressionCallable::accepts(const std::vector<ColumnType>& arguments) const noexcept { return descriptor().arguments == arguments; }
ColumnType IExpressionCallable::inferReturnType(const std::vector<ColumnType>& arguments) const
{
    if (!accepts(arguments)) throw plan::PlannerException(plan::TypeError{.message = descriptor().name + " has no matching overload"});
    return descriptor().returns;
}

ExpressionRegistry::ExpressionRegistry()
{
    registerCallable(std::make_unique<ToUtcFunction>(std::vector<ColumnType>{ColumnType::STRING}, "to_utc('2026-07-23T09:00:00Z')"));
    registerCallable(std::make_unique<ToUtcFunction>(std::vector<ColumnType>{ColumnType::STRING, ColumnType::STRING}, "to_utc('2026-07-23 09:00:00', '-07:00')"));
    registerCallable(std::make_unique<FromUtcFunction>());
    registerCallable(std::make_unique<AddOperator>(std::vector<ColumnType>{ColumnType::INT, ColumnType::INT}, ColumnType::INT, "1 + 2"));
    registerCallable(std::make_unique<AddOperator>(std::vector<ColumnType>{ColumnType::TIMESTAMP, ColumnType::DURATION_SECONDS}, ColumnType::TIMESTAMP, "time + duration_ns(2)"));
    registerCallable(std::make_unique<SubtractOperator>(std::vector<ColumnType>{ColumnType::INT, ColumnType::INT}, ColumnType::INT, "3 - 1"));
    registerCallable(std::make_unique<SubtractOperator>(std::vector<ColumnType>{ColumnType::TIMESTAMP, ColumnType::TIMESTAMP}, ColumnType::DURATION_SECONDS, "end_time - time"));
    registerCallable(std::make_unique<SubtractOperator>(std::vector<ColumnType>{ColumnType::TIMESTAMP, ColumnType::DURATION_SECONDS}, ColumnType::TIMESTAMP, "time - duration_ns(2)"));
    registerCallable(std::make_unique<MultiplyOperator>()); registerCallable(std::make_unique<DivideOperator>());
    for (const auto type : {ColumnType::STRING, ColumnType::INT, ColumnType::TIMESTAMP, ColumnType::DURATION_SECONDS, ColumnType::BOOL})
    { registerCallable(std::make_unique<EqualOperator>(type)); registerCallable(std::make_unique<NotEqualOperator>(type)); registerCallable(std::make_unique<LessOperator>(type)); registerCallable(std::make_unique<LessEqualOperator>(type)); registerCallable(std::make_unique<GreaterOperator>(type)); registerCallable(std::make_unique<GreaterEqualOperator>(type)); }
    registerCallable(std::make_unique<AndOperator>()); registerCallable(std::make_unique<OrOperator>()); registerCallable(std::make_unique<NotOperator>()); registerCallable(std::make_unique<UnaryPlusOperator>()); registerCallable(std::make_unique<UnaryMinusOperator>());
}

void ExpressionRegistry::registerCallable(std::unique_ptr<IExpressionCallable> callable)
{
    const auto& descriptor = callable->descriptor();
    if (descriptor.arguments.empty()) throw std::invalid_argument("Expression callable must have at least one argument");
    const auto duplicate = std::any_of(callables_.begin(), callables_.end(), [&descriptor](const auto& existing) { const auto& other = existing->descriptor(); return normalized(other.name) == normalized(descriptor.name) && other.kind == descriptor.kind && other.arguments == descriptor.arguments; });
    if (duplicate) throw std::invalid_argument("Duplicate expression callable: " + descriptor.name);
    callables_.push_back(std::move(callable));
}

const IExpressionCallable& ExpressionRegistry::resolveFunction(std::string_view name, const std::vector<ColumnType>& arguments) const { return resolveOperator(name, ExpressionCallableKind::FUNCTION, arguments); }
const IExpressionCallable& ExpressionRegistry::resolveOperator(std::string_view name, ExpressionCallableKind kind, const std::vector<ColumnType>& arguments) const
{
    const auto matching_name = normalized(name); const IExpressionCallable* match = nullptr;
    for (const auto& callable : callables_) if (callable->descriptor().kind == kind && normalized(callable->descriptor().name) == matching_name && callable->accepts(arguments)) { if (match != nullptr) throw std::logic_error("Ambiguous expression callable: " + std::string(name)); match = callable.get(); }
    if (match == nullptr) throw plan::PlannerException(plan::TypeError{.message = std::string(name) + " has no matching overload"});
    return *match;
}
std::vector<ExpressionCallableDescriptor> ExpressionRegistry::functions() const { std::vector<ExpressionCallableDescriptor> result; for (const auto& callable : callables_) if (callable->descriptor().kind == ExpressionCallableKind::FUNCTION) result.push_back(callable->descriptor()); std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name != b.name ? a.name < b.name : expressionArgumentsText(a.arguments) < expressionArgumentsText(b.arguments); }); return result; }
std::vector<ExpressionCallableDescriptor> ExpressionRegistry::operators() const { std::vector<ExpressionCallableDescriptor> result; for (const auto& callable : callables_) if (callable->descriptor().kind != ExpressionCallableKind::FUNCTION) result.push_back(callable->descriptor()); std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { const auto arity = a.kind == ExpressionCallableKind::UNARY_OPERATOR ? 0 : 1; const auto b_arity = b.kind == ExpressionCallableKind::UNARY_OPERATOR ? 0 : 1; if (a.name != b.name) return a.name < b.name; if (arity != b_arity) return arity < b_arity; return expressionArgumentsText(a.arguments) < expressionArgumentsText(b.arguments); }); return result; }
std::string mldp_pvxs_driver::query::columnTypeName(const ColumnType type) { switch (type) { case ColumnType::STRING: return "string"; case ColumnType::TIMESTAMP: return "timestamp"; case ColumnType::DURATION_SECONDS: return "duration"; case ColumnType::INT: return "int"; case ColumnType::NATIVE_VALUE: return "native_value"; case ColumnType::BOOL: return "bool"; } return "unknown"; }
std::string mldp_pvxs_driver::query::expressionArgumentsText(const std::vector<ColumnType>& arguments) { std::string result{"("}; for (size_t index = 0; index < arguments.size(); ++index) { if (index != 0) result += ", "; result += columnTypeName(arguments[index]); } return result + ")"; }
