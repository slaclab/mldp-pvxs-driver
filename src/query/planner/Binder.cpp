//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/Binder.h>

#include <query/QueryableFactory.h>
#include <query/QueryTableCatalog.h>
#include <query/ScalarFunctionRegistry.h>
#include <query/AggregateRegistry.h>
#include <query/WindowFunctionRegistry.h>
#include <query/ExpressionRegistry.h>
#include <query/plan/PlannerError.h>

#include <arrow/type.h>

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <cctype>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

const ColumnSchema* findColumnSchema(const std::vector<ColumnSchema>& schema, const std::string& name)
{
    for (const auto& column : schema)
    {
        if (column.name == name)
        {
            return &column;
        }
    }
    return nullptr;
}

struct ResolvedColumn {
    std::string table_alias;
    std::string column_name;
};

std::string qualify(const std::string& alias, const std::string& name)
{
    return alias + "." + name;
}

std::string resolveColumnOnTable(const QualifiedColumn& column,
                                 const plan::BoundTable& table)
{
    if (column.qualifier.has_value() && column.qualifier.value() != table.table_alias && column.qualifier.value() != table.table_name)
    {
        return "";
    }

    if (findColumnSchema(table.schema, column.name) != nullptr)
    {
        return column.name;
    }
    if (column.name.rfind("attributes.", 0) == 0 && findColumnSchema(table.schema, "attributes") != nullptr)
    {
        return column.name;
    }
    if (column.name.rfind("provenance.", 0) == 0 && findColumnSchema(table.schema, "provenance") != nullptr)
    {
        return column.name;
    }
    return "";
}

std::string listAliases(const std::vector<plan::BoundTable>& tables)
{
    std::ostringstream out;
    for (size_t index = 0; index < tables.size(); ++index)
    {
        if (index != 0)
        {
            out << ", ";
        }
        out << "'" << tables[index].table_alias << "'";
    }
    return out.str();
}

ResolvedColumn resolveColumnReference(const QualifiedColumn& column,
                                      const std::vector<plan::BoundTable>& tables)
{
    std::vector<ResolvedColumn> matches;
    for (const auto& table : tables)
    {
        const auto resolved = resolveColumnOnTable(column, table);
        if (!resolved.empty())
        {
            matches.push_back(ResolvedColumn{.table_alias = table.table_alias, .column_name = resolved});
        }
    }

    if (matches.empty())
    {
        if (column.qualifier.has_value())
        {
            throw plan::PlannerException(plan::BindError{
                .message = "Unknown column '" + column.qualifier.value() + "." + column.name + "'"});
        }
        throw plan::PlannerException(plan::BindError{
            .message = "Unknown column '" + column.name + "'"});
    }

    if (matches.size() > 1)
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Ambiguous column '" + column.name + "'. Qualify it with one of: " + listAliases(tables)});
    }

    return matches.front();
}

const QualifiedColumn& columnExpression(const ExpressionPtr& expression, const std::string_view clause)
{
    if (expression)
    {
        if (const auto* column = std::get_if<QualifiedColumn>(&expression->value)) return *column;
    }
    throw plan::PlannerException(plan::BindError{.message = std::string(clause) + " requires a column expression"});
}

PredicateOp mapBinaryOp(const PredicateBinaryOp op)
{
    switch (op)
    {
    case PredicateBinaryOp::NEQ:
        return PredicateOp::NEQ;
    case PredicateBinaryOp::LT:
        return PredicateOp::LT;
    case PredicateBinaryOp::LTE:
        return PredicateOp::LTE;
    case PredicateBinaryOp::GT:
        return PredicateOp::GT;
    case PredicateBinaryOp::GTE:
        return PredicateOp::GTE;
    case PredicateBinaryOp::LIKE:
        return PredicateOp::LIKE;
    case PredicateBinaryOp::CONTAINS:
        return PredicateOp::CONTAINS;
    case PredicateBinaryOp::PREFIX:
        return PredicateOp::PREFIX;
    }
    return PredicateOp::EQ;
}

std::set<PredicateOp> defaultTextOps()
{
    return {PredicateOp::EQ, PredicateOp::NEQ, PredicateOp::IN, PredicateOp::PREFIX, PredicateOp::CONTAINS, PredicateOp::LIKE};
}

std::set<PredicateOp> defaultNativeValueOps()
{
    return {PredicateOp::EQ, PredicateOp::NEQ, PredicateOp::LT, PredicateOp::LTE,
            PredicateOp::GT, PredicateOp::GTE, PredicateOp::IN, PredicateOp::BETWEEN};
}

plan::PlannerLiteralValue toPlannerLiteral(const LiteralValue& value)
{
    if (std::holds_alternative<std::string>(value))
    {
        return std::get<std::string>(value);
    }
    if (std::holds_alternative<int64_t>(value))
    {
        return std::get<int64_t>(value);
    }
    if (std::holds_alternative<double>(value))
    {
        return std::get<double>(value);
    }
    if (std::holds_alternative<bool>(value))
    {
        return std::get<bool>(value);
    }
    if (std::holds_alternative<TimestampNsLiteral>(value))
    {
        return std::get<TimestampNsLiteral>(value);
    }
    if (std::holds_alternative<DurationNsLiteral>(value))
    {
        return std::get<DurationNsLiteral>(value);
    }
    return std::get<NowLiteral>(value);
}

/// Returns the call when @p expression is `f(...) OVER ...`, else null.
const FunctionCall* windowCall(const ExpressionPtr& expression)
{
    const auto* call = expression ? std::get_if<FunctionCall>(&expression->value) : nullptr;
    return call != nullptr && call->over ? call : nullptr;
}

bool containsWindow(const ExpressionPtr& expression)
{
    if (!expression) return false;
    return std::visit([](const auto& value) -> bool
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FunctionCall>)
            return value.over || std::any_of(value.arguments.begin(), value.arguments.end(), containsWindow);
        else if constexpr (std::is_same_v<T, UnaryExpression>) return containsWindow(value.operand);
        else if constexpr (std::is_same_v<T, BinaryExpression>) return containsWindow(value.left) || containsWindow(value.right);
        else return false;
    }, expression->value);
}

[[noreturn]] void windowNotAllowed()
{
    throw plan::PlannerException(plan::BindError{
        .message = "Window functions are allowed only in the select list and ORDER BY; filter on a window result through a derived table"});
}

LiteralValue constantExpression(const ExpressionPtr& expression)
{
    if (!expression) throw plan::PlannerException(plan::BindError{.message = "Missing predicate expression"});
    if (const auto* literal = std::get_if<LiteralValue>(&expression->value)) return *literal;
    if (windowCall(expression)) windowNotAllowed();
    if (const auto* function = std::get_if<FunctionCall>(&expression->value)) return ScalarFunctionRegistry{}.evaluateTimestamp(*function);
    throw plan::PlannerException(plan::BindError{.message = "WHERE expression must be constant"});
}

/// Returns `column <op> other_column` as a boolean expression, or null when
/// @p where compares a column against a constant (the regular predicate path).
ExpressionPtr columnComparisonCondition(const WherePredicate& where)
{
    const auto columnOperand = [](const ExpressionPtr& expression) -> const QualifiedColumn*
    { return expression ? std::get_if<QualifiedColumn>(&expression->value) : nullptr; };
    const auto make = [](const QualifiedColumn& column, std::string op, const ExpressionPtr& other)
    {
        return std::make_shared<Expression>(Expression{.value = BinaryExpression{
            std::move(op), std::make_shared<Expression>(Expression{.value = column}),
            std::make_shared<Expression>(*other)}});
    };
    if (const auto* eq = std::get_if<EqPredicate>(&where); eq && columnOperand(eq->expression))
        return make(eq->column, "=", eq->expression);
    if (const auto* op = std::get_if<OpPredicate>(&where); op && columnOperand(op->expression))
    {
        switch (op->op)
        {
        case PredicateBinaryOp::NEQ: return make(op->column, "!=", op->expression);
        case PredicateBinaryOp::LT: return make(op->column, "<", op->expression);
        case PredicateBinaryOp::LTE: return make(op->column, "<=", op->expression);
        case PredicateBinaryOp::GT: return make(op->column, ">", op->expression);
        case PredicateBinaryOp::GTE: return make(op->column, ">=", op->expression);
        default: break;
        }
    }
    return nullptr;
}

/// Returns `column IS [NOT] NULL` as a boolean expression when @p where tests a
/// column of a NULL-extended (LEFT JOIN right-side) table, or null otherwise.
ExpressionPtr nullTestCondition(const WherePredicate&                  where,
                                const std::vector<plan::BoundTable>&   tables,
                                const std::set<std::string>&           nullable_aliases)
{
    const QualifiedColumn* column = nullptr;
    std::string            op;
    if (const auto* is_null = std::get_if<IsNullPredicate>(&where)) { column = &is_null->column; op = "IS NULL"; }
    else if (const auto* not_null = std::get_if<IsNotNullPredicate>(&where)) { column = &not_null->column; op = "IS NOT NULL"; }
    if (!column || nullable_aliases.empty() || !nullable_aliases.contains(resolveColumnReference(*column, tables).table_alias))
        return nullptr;
    return std::make_shared<Expression>(Expression{.value = UnaryExpression{
        std::move(op), std::make_shared<Expression>(Expression{.value = *column})}});
}

std::string renderExpression(const ExpressionPtr& expression)
{
    if (!expression) return "";
    return std::visit([&](const auto& value) -> std::string
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, QualifiedColumn>) return value.name;
        else if constexpr (std::is_same_v<T, LiteralValue>)
        {
            return std::visit([](const auto& literal) -> std::string
            {
                using Literal = std::decay_t<decltype(literal)>;
                if constexpr (std::is_same_v<Literal, int64_t>) return std::to_string(literal);
                else if constexpr (std::is_same_v<Literal, std::string>) return literal;
                else if constexpr (std::is_same_v<Literal, double>) return std::to_string(literal);
                else if constexpr (std::is_same_v<Literal, bool>) return literal ? "true" : "false";
                else if constexpr (std::is_same_v<Literal, DurationNsLiteral>) return std::to_string(literal.value) + "ns";
                else if constexpr (std::is_same_v<Literal, TimestampNsLiteral>) return std::to_string(literal.value);
                else return "now";
            }, value);
        }
        else if constexpr (std::is_same_v<T, FunctionCall>) return value.name;
        else if constexpr (std::is_same_v<T, UnaryExpression>) return value.operator_name + renderExpression(value.operand);
        else if constexpr (std::is_same_v<T, BinaryExpression>) return renderExpression(value.left) + " " + value.operator_name + " " + renderExpression(value.right);
        else return "";
    }, expression->value);
}

std::string generatedExpressionName(const ExpressionPtr& expression)
{
    const auto text = renderExpression(expression);
    std::string output;
    bool separator = true;
    for (const auto ch : text)
    {
        if (std::isalnum(static_cast<unsigned char>(ch)))
        {
            output.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            separator = false;
        }
        else if (!separator)
        {
            output.push_back('_');
            separator = true;
        }
    }
    if (!output.empty() && output.back() == '_') output.pop_back();
    return output.empty() ? "expression" : output;
}

ColumnType bindExpression(const ExpressionPtr& expression, const std::vector<plan::BoundTable>& tables, const bool multi_table)
{
    if (!expression) throw plan::PlannerException(plan::BindError{.message = "Missing expression"});
    return std::visit([&](auto& value) -> ColumnType
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, QualifiedColumn>)
        {
            const auto resolved = resolveColumnReference(value, tables);
            const auto& schema = tables[std::find_if(tables.begin(), tables.end(), [&](const auto& table) { return table.table_alias == resolved.table_alias; }) - tables.begin()].schema;
            const auto* column = findColumnSchema(schema, resolved.column_name);
            if (!column)
            {
                if (resolved.column_name.rfind("attributes.", 0) != 0 && resolved.column_name.rfind("provenance.", 0) != 0)
                    throw plan::PlannerException(plan::BindError{.message = "Unknown column '" + resolved.column_name + "'"});
                value.name = multi_table ? qualify(resolved.table_alias, resolved.column_name) : resolved.column_name;
                value.qualifier.reset();
                return ColumnType::STRING;
            }
            // Columns of an alias-less scope (window outputs) keep their bare name.
            value.name = multi_table && !resolved.table_alias.empty() ? qualify(resolved.table_alias, resolved.column_name) : resolved.column_name;
            value.qualifier.reset();
            return column->type;
        }
        else if constexpr (std::is_same_v<T, LiteralValue>)
        {
            if (std::holds_alternative<std::string>(value)) return ColumnType::STRING;
            if (std::holds_alternative<int64_t>(value)) return ColumnType::INT;
            if (std::holds_alternative<bool>(value)) return ColumnType::BOOL;
            if (std::holds_alternative<TimestampNsLiteral>(value) || std::holds_alternative<NowLiteral>(value)) return ColumnType::TIMESTAMP;
            if (std::holds_alternative<DurationNsLiteral>(value)) return ColumnType::DURATION_SECONDS;
            return ColumnType::INT;
        }
        else if constexpr (std::is_same_v<T, FunctionCall>)
        {
            if (value.over) windowNotAllowed();
            std::vector<ColumnType> args; for (const auto& argument : value.arguments) args.push_back(bindExpression(argument, tables, multi_table));
            return ExpressionRegistry{}.resolveFunction(value.name, args).inferReturnType(args);
        }
        else if constexpr (std::is_same_v<T, UnaryExpression>)
        {
            const auto type = bindExpression(value.operand, tables, multi_table);
            return ExpressionRegistry{}.resolveOperator(value.operator_name, ExpressionCallableKind::UNARY_OPERATOR, {type}).inferReturnType({type});
        }
        else
        {
            const auto left = bindExpression(value.left, tables, multi_table);
            const auto right = bindExpression(value.right, tables, multi_table);
            return ExpressionRegistry{}.resolveOperator(value.operator_name, ExpressionCallableKind::BINARY_OPERATOR, {left, right}).inferReturnType({left, right});
        }
    }, expression->value);
}

// ---------------------------------------------------------------------------
// GROUP BY / aggregate binding
// ---------------------------------------------------------------------------

const IAggregateFunction* aggregateFunction(std::string_view name)
{
    return AggregateRegistry::instance().find(name);
}

bool isAggregateCall(const ExpressionPtr& expression)
{
    const auto* call = expression ? std::get_if<FunctionCall>(&expression->value) : nullptr;
    return call != nullptr && !call->over && aggregateFunction(call->name) != nullptr;
}

bool containsAggregate(const ExpressionPtr& expression)
{
    if (!expression) return false;
    return std::visit([](const auto& value) -> bool
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FunctionCall>)
        {
            // `SUM(x) OVER ...` is a window call, but aggregates in its arguments or
            // window keys (`SUM(COUNT(*)) OVER ...`) still make the query grouped.
            if (value.over)
            {
                return std::any_of(value.arguments.begin(), value.arguments.end(), containsAggregate) ||
                       std::any_of(value.over->partition_by.begin(), value.over->partition_by.end(), containsAggregate) ||
                       std::any_of(value.over->order_by.begin(), value.over->order_by.end(), [](const auto& item) { return containsAggregate(item.expression); });
            }
            if (aggregateFunction(value.name)) return true;
            return std::any_of(value.arguments.begin(), value.arguments.end(), containsAggregate);
        }
        else if constexpr (std::is_same_v<T, UnaryExpression>) return containsAggregate(value.operand);
        else if constexpr (std::is_same_v<T, BinaryExpression>) return containsAggregate(value.left) || containsAggregate(value.right);
        else return false;
    }, expression->value);
}

ExpressionPtr cloneExpression(const ExpressionPtr& expression);

std::shared_ptr<WindowSpec> cloneWindowSpec(const WindowSpec& spec)
{
    auto copy = std::make_shared<WindowSpec>(spec);
    for (auto& expression : copy->partition_by) expression = cloneExpression(expression);
    for (auto& item : copy->order_by)
        item.expression = cloneExpression(item.expression ? item.expression : std::make_shared<Expression>(Expression{.value = item.column}));
    if (copy->frame)
    {
        copy->frame->start.offset = cloneExpression(copy->frame->start.offset);
        copy->frame->end.offset = cloneExpression(copy->frame->end.offset);
    }
    return copy;
}

ExpressionPtr cloneExpression(const ExpressionPtr& expression)
{
    if (!expression) return nullptr;
    return std::visit([](const auto& value) -> ExpressionPtr
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FunctionCall>)
        {
            FunctionCall copy = value;
            for (auto& argument : copy.arguments) argument = cloneExpression(argument);
            if (copy.over) copy.over = cloneWindowSpec(*copy.over);
            return std::make_shared<Expression>(Expression{.value = std::move(copy)});
        }
        else if constexpr (std::is_same_v<T, UnaryExpression>)
            return std::make_shared<Expression>(Expression{.value = UnaryExpression{value.operator_name, cloneExpression(value.operand)}});
        else if constexpr (std::is_same_v<T, BinaryExpression>)
            return std::make_shared<Expression>(Expression{.value = BinaryExpression{value.operator_name, cloneExpression(value.left), cloneExpression(value.right)}});
        else
            return std::make_shared<Expression>(Expression{.value = value});
    }, expression->value);
}

// Unambiguous rendering used to match a select expression to a GROUP BY key.
std::string canonicalExpression(const ExpressionPtr& expression)
{
    if (!expression) return "";
    return std::visit([](const auto& value) -> std::string
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FunctionCall>)
        {
            std::string lower(value.name);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            std::string text = lower + "(" + (value.distinct ? "distinct " : "") + (value.star ? "*" : "");
            for (std::size_t index = 0; index < value.arguments.size(); ++index)
                text += (index == 0 ? "" : ",") + canonicalExpression(value.arguments[index]);
            text += ")";
            if (value.over)
            {
                text += " over(";
                for (const auto& expression : value.over->partition_by) text += canonicalExpression(expression) + ",";
                text += "|";
                for (const auto& item : value.over->order_by)
                    text += canonicalExpression(item.expression) + (item.direction == SortDirection::DESCENDING ? " desc," : ",");
                text += ")";
            }
            return text;
        }
        else if constexpr (std::is_same_v<T, LiteralValue>)
        {
            if (const auto* text = std::get_if<std::string>(&value)) return "'" + *text + "'";
            return renderExpression(std::make_shared<Expression>(Expression{.value = value}));
        }
        else if constexpr (std::is_same_v<T, UnaryExpression>) return "(" + value.operator_name + " " + canonicalExpression(value.operand) + ")";
        else if constexpr (std::is_same_v<T, BinaryExpression>)
            return "(" + canonicalExpression(value.left) + " " + value.operator_name + " " + canonicalExpression(value.right) + ")";
        else return "col:" + value.name;
    }, expression->value);
}

// Display name for an aggregate output, e.g. count, max_time, count_distinct_pv.
std::string aggregateDisplayName(const FunctionCall& call)
{
    std::string lower(call.name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (call.star || call.arguments.empty()) return lower;
    return lower + (call.distinct ? "_distinct_" : "_") + generatedExpressionName(call.arguments.front());
}

std::string selectItemName(const SelectItem& item, const bool multi_table, const std::vector<plan::BoundTable>& tables)
{
    if (item.alias) return *item.alias;
    if (const auto* column = std::get_if<QualifiedColumn>(&item.expression->value))
    {
        const auto resolved = resolveColumnReference(*column, tables);
        return multi_table ? qualify(resolved.table_alias, resolved.column_name) : resolved.column_name;
    }
    if (const auto* call = std::get_if<FunctionCall>(&item.expression->value); call && aggregateFunction(call->name))
        return aggregateDisplayName(*call);
    return generatedExpressionName(item.expression);
}

/** Rewrites select / HAVING / ORDER BY expressions of an aggregate query so
 *  they reference only the aggregate output (group keys and aggregate results). */
class AggregateRewriter
{
public:
    AggregateRewriter(const std::vector<plan::BoundTable>& tables, const bool multi_table) : tables_(tables), multi_table_(multi_table) {}

    void addKey(const ExpressionPtr& expression)
    {
        if (containsAggregate(expression))
            throw plan::PlannerException(plan::BindError{.message = "GROUP BY cannot contain an aggregate function"});
        auto bound = cloneExpression(expression);
        const auto type = bindExpression(bound, tables_, multi_table_);
        if (type == ColumnType::NATIVE_VALUE)
            throw plan::PlannerException(plan::BindError{.message = "GROUP BY requires a scalar expression; native sample values cannot be grouped"});
        const auto canonical = canonicalExpression(bound);
        if (key_index_.contains(canonical)) return;
        const auto name = "__key_" + std::to_string(spec_.keys.size());
        key_index_.emplace(canonical, spec_.keys.size());
        spec_.keys.push_back(plan::GroupKey{.expression = bound, .name = name});
        output_schema_.push_back(ColumnSchema{.name = name, .type = type, .required = false, .is_output = true, .pushable_ops = {}, .filterable_ops = {}, .notes = {}});
    }

    /** Returns an equivalent expression over the aggregate output. */
    ExpressionPtr rewrite(const ExpressionPtr& expression)
    {
        if (!expression) return nullptr;
        if (const auto* call = windowCall(expression))
        {
            // Window arguments and keys run over the aggregate output; the call
            // itself is evaluated later by the window step.
            FunctionCall copy = *call;
            for (auto& argument : copy.arguments) argument = rewrite(argument);
            copy.over = std::make_shared<WindowSpec>(*call->over);
            for (auto& key : copy.over->partition_by) key = rewrite(key);
            for (auto& item : copy.over->order_by) item.expression = rewrite(item.expression);
            return std::make_shared<Expression>(Expression{.value = std::move(copy)});
        }
        if (isAggregateCall(expression)) return column(aggregate(std::get<FunctionCall>(expression->value)));
        if (std::holds_alternative<LiteralValue>(expression->value)) return expression;
        if (!containsAggregate(expression) && !containsWindow(expression))
        {
            auto bound = cloneExpression(expression);
            (void)bindExpression(bound, tables_, multi_table_);
            if (const auto found = key_index_.find(canonicalExpression(bound)); found != key_index_.end())
                return column(spec_.keys[found->second].name);
            if (const auto* reference = std::get_if<QualifiedColumn>(&bound->value))
                throw plan::PlannerException(plan::BindError{
                    .message = "Column '" + reference->name + "' must appear in GROUP BY or be used in an aggregate function"});
        }
        return std::visit([this, &expression](const auto& value) -> ExpressionPtr
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, FunctionCall>)
            {
                FunctionCall copy = value;
                for (auto& argument : copy.arguments) argument = rewrite(argument);
                return std::make_shared<Expression>(Expression{.value = std::move(copy)});
            }
            else if constexpr (std::is_same_v<T, UnaryExpression>)
                return std::make_shared<Expression>(Expression{.value = UnaryExpression{value.operator_name, rewrite(value.operand)}});
            else if constexpr (std::is_same_v<T, BinaryExpression>)
                return std::make_shared<Expression>(Expression{.value = BinaryExpression{value.operator_name, rewrite(value.left), rewrite(value.right)}});
            else
                return expression;
        }, expression->value);
    }

    /** Type-checks a rewritten expression against the aggregate output plus @p extra columns. */
    ColumnType check(const ExpressionPtr& rewritten, const std::vector<ColumnSchema>& extra = {}) const
    {
        auto schema = output_schema_;
        schema.insert(schema.end(), extra.begin(), extra.end());
        const std::vector<plan::BoundTable> output{plan::BoundTable{.table_name = "aggregate", .table_alias = "", .schema = std::move(schema)}};
        return bindExpression(rewritten, output, false);
    }

    plan::AggregateSpec& spec() { return spec_; }

private:
    static ExpressionPtr column(const std::string& name)
    {
        return std::make_shared<Expression>(Expression{.value = QualifiedColumn{.qualifier = std::nullopt, .name = name, .path = {}}});
    }

    std::string aggregate(const FunctionCall& call)
    {
        const auto* function = aggregateFunction(call.name);
        if (!call.star && call.arguments.size() != 1)
            throw plan::PlannerException(plan::BindError{.message = "Aggregate function '" + call.name + "' takes exactly one argument"});
        ExpressionPtr             argument;
        std::optional<ColumnType> argument_type;
        if (!call.star)
        {
            if (containsAggregate(call.arguments.front()))
                throw plan::PlannerException(plan::BindError{.message = "Aggregate functions cannot be nested"});
            argument = cloneExpression(call.arguments.front());
            argument_type = bindExpression(argument, tables_, multi_table_);
        }
        const auto type = function->bind(argument_type, call.distinct);
        FunctionCall normalized = call;
        normalized.arguments = argument ? std::vector<ExpressionPtr>{argument} : std::vector<ExpressionPtr>{};
        const auto canonical = canonicalExpression(std::make_shared<Expression>(Expression{.value = normalized}));
        if (const auto found = aggregate_index_.find(canonical); found != aggregate_index_.end())
            return spec_.aggregates[found->second].name;
        const auto name = "__agg_" + std::to_string(spec_.aggregates.size());
        aggregate_index_.emplace(canonical, spec_.aggregates.size());
        spec_.aggregates.push_back(plan::AggregateCall{.function = function->descriptor().name, .argument = argument, .distinct = call.distinct, .name = name});
        output_schema_.push_back(ColumnSchema{.name = name, .type = type, .required = false, .is_output = true, .pushable_ops = {}, .filterable_ops = {}, .notes = {}});
        return name;
    }

    const std::vector<plan::BoundTable>&     tables_;
    bool                                     multi_table_;
    plan::AggregateSpec                      spec_;
    std::vector<ColumnSchema>                output_schema_;
    std::map<std::string, std::size_t>       key_index_;
    std::map<std::string, std::size_t>       aggregate_index_;
};

// ---------------------------------------------------------------------------
// Window function binding
// ---------------------------------------------------------------------------

[[noreturn]] void windowError(const std::string& message)
{
    throw plan::PlannerException(plan::BindError{.message = message});
}

/** Merges a window spec with the named window it references (`OVER (w ORDER BY ...)`). */
WindowSpec resolveWindowSpec(const WindowSpec& spec, const std::vector<NamedWindow>& named, const std::size_t depth = 0)
{
    if (!spec.base_name) return spec;
    const auto found = std::find_if(named.begin(), named.end(), [&](const auto& window) { return window.name == *spec.base_name; });
    if (found == named.end()) windowError("Unknown window '" + *spec.base_name + "'");
    if (depth > named.size()) windowError("Window '" + *spec.base_name + "' is defined in terms of itself");
    auto base = resolveWindowSpec(found->spec, named, depth + 1);
    if (!spec.partition_by.empty()) windowError("Window '" + *spec.base_name + "' cannot be given a new PARTITION BY");
    if (!spec.order_by.empty() && !base.order_by.empty()) windowError("Window '" + *spec.base_name + "' already has an ORDER BY");
    if (base.frame && (!spec.order_by.empty() || spec.frame)) windowError("Window '" + *spec.base_name + "' has a frame and cannot be extended");
    if (!spec.order_by.empty()) base.order_by = spec.order_by;
    if (spec.frame) base.frame = spec.frame;
    base.base_name.reset();
    return base;
}

/** Returns a copy of @p expression whose window calls carry fully resolved, private window specs. */
ExpressionPtr resolveNamedWindows(const ExpressionPtr& expression, const std::vector<NamedWindow>& named)
{
    if (!expression) return nullptr;
    return std::visit([&](const auto& value) -> ExpressionPtr
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FunctionCall>)
        {
            FunctionCall copy = value;
            for (auto& argument : copy.arguments) argument = resolveNamedWindows(argument, named);
            if (copy.over) copy.over = cloneWindowSpec(resolveWindowSpec(*copy.over, named));
            return std::make_shared<Expression>(Expression{.value = std::move(copy)});
        }
        else if constexpr (std::is_same_v<T, UnaryExpression>)
            return std::make_shared<Expression>(Expression{.value = UnaryExpression{value.operator_name, resolveNamedWindows(value.operand, named)}});
        else if constexpr (std::is_same_v<T, BinaryExpression>)
            return std::make_shared<Expression>(Expression{.value = BinaryExpression{value.operator_name, resolveNamedWindows(value.left, named),
                                                                                       resolveNamedWindows(value.right, named)}});
        else
            return std::make_shared<Expression>(Expression{.value = value});
    }, expression->value);
}

/** Checks the WINDOW clause and resolves the named windows of every select and ORDER BY expression. */
SelectStatement resolveStatementWindows(const SelectStatement& statement)
{
    std::set<std::string> names;
    for (const auto& window : statement.named_windows)
    {
        if (!names.insert(window.name).second) windowError("Window '" + window.name + "' is defined more than once");
        (void)resolveWindowSpec(window.spec, statement.named_windows);
    }
    auto resolved = statement;
    for (auto& item : resolved.select_items) item.expression = resolveNamedWindows(item.expression, statement.named_windows);
    for (auto& item : resolved.order_by)
        if (item.expression) item.expression = resolveNamedWindows(item.expression, statement.named_windows);
    return resolved;
}

/** Non-negative integer value of a constant frame / LAG offset. */
std::optional<int64_t> constantInteger(const ExpressionPtr& expression)
{
    const auto* literal = expression ? std::get_if<LiteralValue>(&expression->value) : nullptr;
    const auto* value = literal ? std::get_if<int64_t>(literal) : nullptr;
    return value ? std::optional<int64_t>{*value} : std::nullopt;
}

std::optional<int64_t> constantDurationNs(const ExpressionPtr& expression)
{
    const auto* literal = expression ? std::get_if<LiteralValue>(&expression->value) : nullptr;
    const auto* value = literal ? std::get_if<DurationNsLiteral>(literal) : nullptr;
    return value ? std::optional<int64_t>{value->value} : std::nullopt;
}

/** Replaces window calls by references to `__win_N` columns and collects them into window groups.
 *
 *  Argument and key expressions are bound by @p bind, which type-checks them in
 *  place against the window input (base tables or the aggregate output). */
class WindowBinder
{
public:
    using Bind = std::function<ColumnType(const ExpressionPtr&)>;

    explicit WindowBinder(Bind bind) : bind_(std::move(bind)) {}

    /** Returns @p expression with every window call replaced by its output column. */
    ExpressionPtr rewrite(const ExpressionPtr& expression)
    {
        if (!expression) return nullptr;
        if (const auto* call = windowCall(expression)) return column(add(*call));
        return std::visit([&](const auto& value) -> ExpressionPtr
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, FunctionCall>)
            {
                FunctionCall copy = value;
                for (auto& argument : copy.arguments) argument = rewrite(argument);
                return std::make_shared<Expression>(Expression{.value = std::move(copy)});
            }
            else if constexpr (std::is_same_v<T, UnaryExpression>)
                return std::make_shared<Expression>(Expression{.value = UnaryExpression{value.operator_name, rewrite(value.operand)}});
            else if constexpr (std::is_same_v<T, BinaryExpression>)
                return std::make_shared<Expression>(Expression{.value = BinaryExpression{value.operator_name, rewrite(value.left), rewrite(value.right)}});
            else
                return expression;
        }, expression->value);
    }

    /** Window output columns, for binding expressions that reference them. */
    const std::vector<ColumnSchema>& outputs() const { return outputs_; }

    std::vector<plan::WindowGroup>& groups() { return groups_; }

private:
    static ExpressionPtr column(const std::string& name)
    {
        return std::make_shared<Expression>(Expression{.value = QualifiedColumn{.qualifier = std::nullopt, .name = name, .path = {}}});
    }

    ColumnType bindScalar(const ExpressionPtr& expression, const std::string_view clause) const
    {
        if (containsWindow(expression)) windowError("Window functions cannot be nested");
        const auto type = bind_(expression);
        if (type == ColumnType::NATIVE_VALUE) windowError(std::string(clause) + " requires a scalar expression; native sample values cannot be used");
        return type;
    }

    ColumnType bindArgument(const ExpressionPtr& expression) const
    {
        if (containsWindow(expression)) windowError("Window functions cannot be nested");
        return bind_(expression);
    }

    plan::WindowGroup& group(const WindowSpec& spec, std::vector<ColumnType>& order_types)
    {
        plan::WindowGroup candidate;
        std::string       key;
        for (const auto& expression : spec.partition_by)
        {
            (void)bindScalar(expression, "Window PARTITION BY");
            candidate.partition_by.push_back(expression);
            key += canonicalExpression(expression) + ",";
        }
        key += "|";
        for (const auto& item : spec.order_by)
        {
            order_types.push_back(bindScalar(item.expression, "Window ORDER BY"));
            const auto* reference = std::get_if<QualifiedColumn>(&item.expression->value);
            candidate.order_by.push_back(plan::SortKey{.column = reference ? reference->name : "",
                                                       .expression = item.expression,
                                                       .descending = item.direction == SortDirection::DESCENDING});
            key += canonicalExpression(item.expression) + (item.direction == SortDirection::DESCENDING ? " desc," : ",");
        }
        if (const auto found = group_index_.find(key); found != group_index_.end()) return groups_[found->second];
        group_index_.emplace(key, groups_.size());
        groups_.push_back(std::move(candidate));
        return groups_.back();
    }

    static int boundRank(const WindowBoundKind kind)
    {
        switch (kind)
        {
        case WindowBoundKind::UNBOUNDED_PRECEDING: return 0;
        case WindowBoundKind::PRECEDING: return 1;
        case WindowBoundKind::CURRENT_ROW: return 2;
        case WindowBoundKind::FOLLOWING: return 3;
        case WindowBoundKind::UNBOUNDED_FOLLOWING: return 4;
        }
        return 2;
    }

    static plan::WindowFrameSpec frame(const WindowSpec& spec, const std::vector<ColumnType>& order_types)
    {
        if (!spec.frame)
        {
            // Standard default: up to the current row's last peer with ORDER BY,
            // otherwise the whole partition.
            return plan::WindowFrameSpec{.rows = false,
                                         .start = WindowBoundKind::UNBOUNDED_PRECEDING,
                                         .start_offset = 0,
                                         .end = spec.order_by.empty() ? WindowBoundKind::UNBOUNDED_FOLLOWING : WindowBoundKind::CURRENT_ROW,
                                         .end_offset = 0};
        }
        const auto& input = *spec.frame;
        const bool  rows = input.unit == WindowFrameUnit::ROWS;
        if (input.start.kind == WindowBoundKind::UNBOUNDED_FOLLOWING) windowError("Window frame cannot start at UNBOUNDED FOLLOWING");
        if (input.end.kind == WindowBoundKind::UNBOUNDED_PRECEDING) windowError("Window frame cannot end at UNBOUNDED PRECEDING");
        if (boundRank(input.start.kind) > boundRank(input.end.kind)) windowError("Window frame starts after it ends");
        const auto offset = [&](const WindowFrameBound& bound) -> int64_t
        {
            if (bound.kind != WindowBoundKind::PRECEDING && bound.kind != WindowBoundKind::FOLLOWING) return 0;
            std::optional<int64_t> value;
            if (rows)
            {
                value = constantInteger(bound.offset);
                if (!value) windowError("ROWS frame offsets must be non-negative integer constants");
            }
            else
            {
                if (order_types.size() != 1) windowError("RANGE frame with an offset requires exactly one ORDER BY key");
                const auto key = order_types.front();
                if (key == ColumnType::INT)
                {
                    value = constantInteger(bound.offset);
                    if (!value) windowError("RANGE offset for an integer ORDER BY key must be an integer constant");
                }
                else if (key == ColumnType::TIMESTAMP || key == ColumnType::DURATION_SECONDS)
                {
                    value = constantDurationNs(bound.offset);
                    if (!value) windowError("RANGE offset for a timestamp ORDER BY key must be a duration (e.g. 5m)");
                }
                else
                {
                    windowError("RANGE frame with an offset requires an integer, timestamp or duration ORDER BY key");
                }
            }
            if (*value < 0) windowError("Window frame offsets must not be negative");
            return *value;
        };
        return plan::WindowFrameSpec{.rows = rows,
                                     .start = input.start.kind,
                                     .start_offset = offset(input.start),
                                     .end = input.end.kind,
                                     .end_offset = offset(input.end)};
    }

    std::string add(const FunctionCall& call)
    {
        const auto* info = WindowFunctionRegistry::instance().find(call.name);
        if (info == nullptr) windowError("'" + call.name + "' is not a window function");
        if (call.distinct) windowError("DISTINCT is not supported in window functions");
        const auto& function = info->descriptor.name;
        if (call.star && function != "count") windowError("Only COUNT accepts '*'");

        std::vector<ColumnType> order_types;
        auto&                   target = group(*call.over, order_types);
        plan::WindowCall        bound{.function = function, .argument = nullptr, .offset = 1, .default_value = nullptr,
                                      .frame = frame(*call.over, order_types), .name = "__win_" + std::to_string(next_++)};
        ColumnType              type = ColumnType::INT;
        switch (info->kind)
        {
        case WindowFunctionKind::RANKING:
            if (!call.arguments.empty()) windowError("Window function '" + function + "' takes no arguments");
            break;
        case WindowFunctionKind::OFFSET:
            if (call.arguments.empty() || call.arguments.size() > 3) windowError("Window function '" + function + "' takes 1 to 3 arguments");
            bound.argument = call.arguments[0];
            type = bindArgument(bound.argument);
            if (call.arguments.size() > 1)
            {
                const auto offset = constantInteger(call.arguments[1]);
                if (!offset || *offset < 0) windowError("The offset of '" + function + "' must be a non-negative integer constant");
                bound.offset = *offset;
            }
            if (call.arguments.size() > 2)
            {
                bound.default_value = call.arguments[2];
                (void)bindArgument(bound.default_value);
            }
            break;
        case WindowFunctionKind::VALUE:
            if (call.arguments.size() != 1) windowError("Window function '" + function + "' takes exactly one argument");
            bound.argument = call.arguments[0];
            type = bindArgument(bound.argument);
            break;
        case WindowFunctionKind::AGGREGATE:
        {
            if (!call.star && call.arguments.size() != 1) windowError("Aggregate function '" + function + "' takes exactly one argument");
            std::optional<ColumnType> argument_type;
            if (!call.star)
            {
                bound.argument = call.arguments[0];
                argument_type = bindArgument(bound.argument);
            }
            type = aggregateFunction(function)->bind(argument_type, false);
            break;
        }
        }
        outputs_.push_back(ColumnSchema{.name = bound.name, .type = type, .required = false, .is_output = true,
                                        .pushable_ops = {}, .filterable_ops = {}, .notes = {}});
        target.calls.push_back(std::move(bound));
        return outputs_.back().name;
    }

    Bind                               bind_;
    std::vector<plan::WindowGroup>     groups_;
    std::map<std::string, std::size_t> group_index_;
    std::vector<ColumnSchema>          outputs_;
    std::size_t                        next_{0};
};

bool isAggregateQuery(const SelectStatement& statement)
{
    if (!statement.group_by.empty() || statement.having) return true;
    if (std::any_of(statement.select_items.begin(), statement.select_items.end(), [](const auto& item) { return containsAggregate(item.expression); }))
        return true;
    return std::any_of(statement.order_by.begin(), statement.order_by.end(), [](const auto& item) { return containsAggregate(item.expression); });
}

void bindAggregateQuery(const SelectStatement& statement, const std::vector<plan::BoundTable>& tables, const bool multi_table, plan::BoundSelect& bound)
{
    if (statement.select_all)
        throw plan::PlannerException(plan::BindError{.message = "SELECT * cannot be used with GROUP BY or aggregate functions; list the grouped columns and aggregates"});
    if (!statement.distinct_on.empty())
        throw plan::PlannerException(plan::BindError{.message = "DISTINCT ON cannot be combined with GROUP BY or aggregate functions"});

    AggregateRewriter rewriter(tables, multi_table);
    for (const auto& key : statement.group_by) rewriter.addKey(key);
    // Window functions run after grouping, over the aggregate output.
    WindowBinder windows([&rewriter](const ExpressionPtr& expression) { return rewriter.check(expression); });

    std::map<std::string, ExpressionPtr> aliases;
    for (const auto& item : statement.select_items)
    {
        auto rewritten = windows.rewrite(rewriter.rewrite(item.expression));
        const auto name = selectItemName(item, multi_table, tables);
        bound.select_columns.push_back(name);
        bound.select_expressions.push_back(rewritten);
        bound.select_names.push_back(name);
        aliases.emplace(name, rewritten);
    }

    if (statement.having)
    {
        auto having = rewriter.rewrite(statement.having);
        if (rewriter.check(having) != ColumnType::BOOL)
            throw plan::PlannerException(plan::BindError{.message = "HAVING requires a boolean condition"});
        rewriter.spec().having = having;
    }

    for (const auto& item : statement.order_by)
    {
        const auto expression = item.expression ? item.expression : std::make_shared<Expression>(Expression{.value = item.column});
        ExpressionPtr rewritten;
        // ORDER BY may name a select output by position (1-based)...
        if (const auto* literal = std::get_if<LiteralValue>(&expression->value))
        {
            const auto* position = std::get_if<int64_t>(literal);
            if (position == nullptr || *position < 1 || *position > static_cast<int64_t>(bound.select_expressions.size()))
                throw plan::PlannerException(plan::BindError{.message = "ORDER BY position is out of range"});
            rewritten = bound.select_expressions[static_cast<std::size_t>(*position - 1)];
        }
        // ...or by alias / display name.
        else if (const auto* column = std::get_if<QualifiedColumn>(&expression->value); column && !column->qualifier)
            if (const auto alias = aliases.find(column->name); alias != aliases.end()) rewritten = alias->second;
        if (!rewritten) rewritten = windows.rewrite(rewriter.rewrite(expression));
        const auto* reference = std::get_if<QualifiedColumn>(&rewritten->value);
        if (reference == nullptr)
            throw plan::PlannerException(plan::BindError{
                .message = "ORDER BY in an aggregate query must name a grouped column, an aggregate, or a select alias"});
        bound.order_by.push_back(plan::SortKey{
            .column = reference->name,
            .expression = rewritten,
            .descending = item.direction == SortDirection::DESCENDING});
    }

    for (const auto& expression : bound.select_expressions) bound.select_types.push_back(rewriter.check(expression, windows.outputs()));
    bound.aggregate = std::make_shared<const plan::AggregateSpec>(std::move(rewriter.spec()));
    bound.windows = std::move(windows.groups());
}

plan::PlannerPredicate buildPredicate(const WherePredicate& where,
                                      const std::vector<plan::BoundTable>& tables)
{
    return std::visit(
        [&](const auto& predicate) -> plan::PlannerPredicate
        {
            const auto resolved = resolveColumnReference(predicate.column, tables);
            const auto* bound_table = [&]() -> const plan::BoundTable* {
                for (const auto& table : tables)
                {
                    if (table.table_alias == resolved.table_alias)
                    {
                        return &table;
                    }
                }
                return nullptr;
            }();
            if (bound_table == nullptr)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Unknown table alias '" + resolved.table_alias + "' for predicate binding"});
            }

            const auto* schema_column = findColumnSchema(bound_table->schema, resolved.column_name);
            const bool is_dynamic_metadata_predicate = schema_column == nullptr &&
                                                       ((resolved.column_name.rfind("attributes.", 0) == 0 &&
                                                         findColumnSchema(bound_table->schema, "attributes") != nullptr) ||
                                                        (resolved.column_name.rfind("provenance.", 0) == 0 &&
                                                         findColumnSchema(bound_table->schema, "provenance") != nullptr));
            if (schema_column == nullptr && !is_dynamic_metadata_predicate)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Unknown column '" + qualify(resolved.table_alias, resolved.column_name) + "'"});
            }

            plan::PlannerPredicate bound{};
            bound.column = resolved.column_name;
            bound.table_alias = resolved.table_alias;
            if (schema_column != nullptr)
            {
                bound.column_type = schema_column->type;
                bound.required_column = schema_column->required;
                bound.pushable_ops = schema_column->pushable_ops;
                bound.filterable_ops = schema_column->filterable_ops;
            }
            else
            {
                bound.column_type = ColumnType::STRING;
                const bool time_series_metadata = bound_table->table_name == "mldp.time_series" ||
                                                  bound_table->table_name == "mldp.time_series_table";
                // The query client selects time-series candidate PVs with
                // their returned metadata. Annotation services can push only
                // exact attribute criteria; its text patterns run on Arrow.
                bound.pushable_ops = time_series_metadata
                                         ? std::set<PredicateOp>{PredicateOp::EQ, PredicateOp::IN, PredicateOp::PREFIX, PredicateOp::CONTAINS, PredicateOp::LIKE}
                                         : std::set<PredicateOp>{PredicateOp::EQ, PredicateOp::IN};
                bound.filterable_ops = defaultTextOps();
            }

            if constexpr (std::is_same_v<std::decay_t<decltype(predicate)>, EqPredicate>)
            {
                bound.op = PredicateOp::EQ;
                bound.values.push_back(toPlannerLiteral(predicate.expression ? constantExpression(predicate.expression) : predicate.value));
            }
            else if constexpr (std::is_same_v<std::decay_t<decltype(predicate)>, InPredicate>)
            {
                bound.op = PredicateOp::IN;
                if (!predicate.expressions.empty())
                {
                    for (const auto& value : predicate.expressions) bound.values.push_back(toPlannerLiteral(constantExpression(value)));
                }
                else for (const auto& value : predicate.values)
                {
                    bound.values.push_back(toPlannerLiteral(value));
                }
            }
            else if constexpr (std::is_same_v<std::decay_t<decltype(predicate)>, RangePredicate>)
            {
                bound.op = PredicateOp::BETWEEN;
                bound.values.push_back(toPlannerLiteral(predicate.lower_expression ? constantExpression(predicate.lower_expression) : predicate.lower));
                bound.values.push_back(toPlannerLiteral(predicate.upper_expression ? constantExpression(predicate.upper_expression) : predicate.upper));
            }
            else if constexpr (std::is_same_v<std::decay_t<decltype(predicate)>, IsNotNullPredicate>)
            {
                bound.op = PredicateOp::IS_NOT_NULL;
            }
            else if constexpr (std::is_same_v<std::decay_t<decltype(predicate)>, IsNullPredicate>)
            {
                bound.op = PredicateOp::IS_NULL;
            }
            else
            {
                bound.op = mapBinaryOp(predicate.op);
                bound.values.push_back(toPlannerLiteral(predicate.expression ? constantExpression(predicate.expression) : predicate.value));
            }

            // NULL tests run in the local Arrow filter for every column type
            // (e.g. the anti-join `LEFT JOIN ... WHERE right.key IS NULL`).
            if (bound.op == PredicateOp::IS_NULL || bound.op == PredicateOp::IS_NOT_NULL)
                bound.filterable_ops.insert(bound.op);
            const bool pushable = bound.pushable_ops.contains(bound.op);
            const bool filterable = bound.filterable_ops.contains(bound.op);
            if (!pushable && !filterable)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Operator not supported for column '" + qualify(bound.table_alias, bound.column) + "'"});
            }
            return bound;
        },
        where);
}

/// Binds a WHERE OR/NOT tree. Leaves use the regular predicate binding; subquery,
/// window and column-to-column leaves are rejected because they cannot run as a
/// per-row local filter.
plan::PlannerPredicateGroup bindPredicateGroup(const WherePredicateTree& node, const std::vector<plan::BoundTable>& tables)
{
    const auto kind = [&node]
    {
        switch (node.kind)
        {
            case WherePredicateTree::Kind::AND: return plan::PlannerPredicateGroup::Kind::AND;
            case WherePredicateTree::Kind::OR: return plan::PlannerPredicateGroup::Kind::OR;
            case WherePredicateTree::Kind::NOT: return plan::PlannerPredicateGroup::Kind::NOT;
            case WherePredicateTree::Kind::LEAF: break;
        }
        return plan::PlannerPredicateGroup::Kind::LEAF;
    }();
    plan::PlannerPredicateGroup group{.kind = kind, .leaf = {}, .children = {}};
    if (node.kind != WherePredicateTree::Kind::LEAF)
    {
        for (const auto& child : node.children) group.children.push_back(bindPredicateGroup(child, tables));
        return group;
    }
    if (const auto* in = std::get_if<InPredicate>(&node.leaf); in != nullptr && (in->subquery || in->column.name == "window" || !in->window_options.empty()))
        throw plan::PlannerException(plan::BindError{.message = "IN (SELECT ...) and window IN (...) are not supported inside OR/NOT; use them as top-level AND conditions"});
    if (columnComparisonCondition(node.leaf))
        throw plan::PlannerException(plan::BindError{.message = "Column-to-column comparisons are not supported inside OR/NOT"});
    group.leaf = buildPredicate(node.leaf, tables);
    return group;
}

/// Calls @p fn on every leaf predicate of @p group.
template <typename Fn>
void forEachGroupLeaf(const plan::PlannerPredicateGroup& group, const Fn& fn)
{
    if (group.kind == plan::PlannerPredicateGroup::Kind::LEAF) fn(group.leaf);
    for (const auto& child : group.children) forEachGroupLeaf(child, fn);
}

/// Rewrites `c = a OR c = b OR c IN (...)` on one column into a single IN predicate,
/// which can then be pushed down like any other top-level conjunct.
std::optional<plan::PlannerPredicate> sameColumnMembership(const plan::PlannerPredicateGroup& group)
{
    if (group.kind != plan::PlannerPredicateGroup::Kind::OR) return std::nullopt;
    std::optional<plan::PlannerPredicate> merged;
    for (const auto& child : group.children)
    {
        if (child.kind != plan::PlannerPredicateGroup::Kind::LEAF) return std::nullopt;
        const auto& leaf = child.leaf;
        if (leaf.op != PredicateOp::EQ && leaf.op != PredicateOp::IN) return std::nullopt;
        if (!merged)
        {
            merged = leaf;
            merged->op = PredicateOp::IN;
            continue;
        }
        if (leaf.column != merged->column || leaf.table_alias != merged->table_alias) return std::nullopt;
        merged->values.insert(merged->values.end(), leaf.values.begin(), leaf.values.end());
    }
    if (!merged || (!merged->pushable_ops.contains(PredicateOp::IN) && !merged->filterable_ops.contains(PredicateOp::IN))) return std::nullopt;
    return merged;
}

plan::BoundTable makeBoundTable(const TableRef& table_ref, const QueryTableCatalog* catalog)
{
    if (table_ref.derived_query)
    {
        // A UNION exposes the output schema of its first branch.
        const auto& derived = *table_ref.derived_query;
        const auto  child = derived.set_operations.empty() ? bindSelect(derived, catalog) : bindSelect(setOperationHead(derived), catalog);
        std::vector<ColumnSchema> schema;
        const auto appendOutput = [&schema](const plan::BoundTable& table)
        {
            for (const auto& column : table.schema)
                if (column.is_output) schema.push_back(ColumnSchema{.name = column.name, .type = column.type, .required = false,
                                                                    .is_output = true, .pushable_ops = {}, .filterable_ops = defaultTextOps(), .notes = "Derived query output"});
        };
        appendOutput(child.from);
        for (const auto& join : child.joins) appendOutput(join.table);
        if (!child.select_all)
        {
            std::vector<ColumnSchema> selected;
            const bool typed = child.select_types.size() == child.select_columns.size() && child.select_names.size() == child.select_columns.size();
            for (std::size_t index = 0; index < child.select_columns.size(); ++index)
            {
                const auto& name = child.select_columns[index];
                const auto dot = name.find('.');
                const auto local = name.substr(dot == std::string::npos ? 0 : dot + 1);
                if (const auto* column = findColumnSchema(schema, local)) selected.push_back(*column);
                else if (typed)
                {
                    // Computed outputs (expressions, aggregates, window results) are exposed under their output name.
                    const auto type = child.select_types[index];
                    selected.push_back(ColumnSchema{.name = child.select_names[index], .type = type, .required = false, .is_output = true, .pushable_ops = {},
                                                    .filterable_ops = type == ColumnType::STRING ? defaultTextOps() : defaultNativeValueOps(),
                                                    .notes = "Derived query output"});
                }
            }
            schema = std::move(selected);
        }
        // An alias-less derived source has an internal identity for planning;
        // SQL may still reference its preserved output schema unqualified.
        return plan::BoundTable{.table_name = "<derived>", .table_alias = table_ref.alias.value_or("derived"), .schema = std::move(schema),
                                .predicates = {}, .derived_query = table_ref.derived_query};
    }
    if (catalog != nullptr)
    {
        if (const auto stored = catalog->find(table_ref.table_name))
        {
            std::vector<ColumnSchema> schema;
            schema.reserve(stored->schema->num_fields());
            for (const auto& field : stored->schema->fields())
            {
                ColumnType type = ColumnType::STRING;
                if (field->type()->id() == arrow::Type::INT64) type = ColumnType::INT;
                else if (field->type()->id() == arrow::Type::BOOL) type = ColumnType::BOOL;
                else if (field->type()->id() == arrow::Type::TIMESTAMP) type = ColumnType::TIMESTAMP;
                else if (field->type()->id() == arrow::Type::DURATION) type = ColumnType::DURATION_SECONDS;
                else if (field->type()->id() == arrow::Type::DENSE_UNION || field->type()->id() == arrow::Type::SPARSE_UNION) type = ColumnType::NATIVE_VALUE;
                schema.push_back(ColumnSchema{.name = field->name(), .type = type, .required = false, .is_output = true,
                                              .pushable_ops = {}, .filterable_ops = type == ColumnType::NATIVE_VALUE ? defaultNativeValueOps() : defaultTextOps(), .notes = "Arrow IPC snapshot"});
            }
            return plan::BoundTable{.table_name = table_ref.table_name, .table_alias = table_ref.alias.value_or(table_ref.table_name),
                                    .schema = std::move(schema), .predicates = {}, .ipc_path = stored->path, .arrow_ipc = true};
        }
    }
    const auto registered_tables = QueryableFactory::instance().registeredTables();
    if (!registered_tables.contains(table_ref.table_name))
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Unknown table '" + table_ref.table_name + "'"});
    }

    IQueryableUPtr queryable;
    try
    {
        queryable = QueryableFactory::instance().createByTable(table_ref.table_name);
    }
    catch (const std::exception& ex)
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Failed to initialize query client for table '" + table_ref.table_name + "': " + ex.what()});
    }
    const auto alias = table_ref.alias.value_or(table_ref.table_name);
    return plan::BoundTable{
        .table_name = table_ref.table_name,
        .table_alias = alias,
        .schema = queryable->tableSchema(table_ref.table_name),
        .predicates = {}};
}

void requireUniqueAlias(const plan::BoundTable& table,
                        const std::unordered_set<std::string>& aliases)
{
    if (aliases.contains(table.table_alias))
    {
        throw plan::PlannerException(plan::BindError{
            .message = "Duplicate table alias '" + table.table_alias + "'"});
    }
}

void ensureColumnCoveredForRequiredCheck(const plan::BoundTable& table,
                                         const std::unordered_set<std::string>& covered_columns)
{
    for (const auto& column : table.schema)
    {
        if (!column.required)
        {
            continue;
        }
        if (!covered_columns.contains(column.name))
        {
            throw plan::PlannerException(plan::BindError{
                .message = "Missing required predicate for column '" + qualify(table.table_alias, column.name) + "'"});
        }
    }
}

void enforceTimeSeriesTableContract(const SelectStatement&              statement,
                                    const std::vector<plan::BoundTable>& tables)
{
    for (const auto& table : tables)
    {
        if (table.table_name != "mldp.time_series_table")
            continue;

        if (!statement.select_all)
        {
            throw plan::PlannerException(plan::BindError{
                .message = "mldp.time_series_table supports SELECT * only; its PV columns are defined by the required pv predicate"});
        }
        if (!statement.order_by.empty())
        {
            throw plan::PlannerException(plan::BindError{
                .message = "mldp.time_series_table does not support ORDER BY because its PV columns are runtime-defined"});
        }
        if (tables.size() != 1)
        {
            throw plan::PlannerException(plan::BindError{
                .message = "mldp.time_series_table does not support joins"});
        }
        const bool has_pv_input = std::any_of(table.in_subqueries.begin(), table.in_subqueries.end(), [](const auto& subquery)
                                               { return subquery.predicate.column == "pv"; }) ||
                                  std::any_of(table.predicates.begin(), table.predicates.end(), [](const auto& predicate)
                                              { return predicate.column == "pv"; });
        if (!has_pv_input)
        {
            throw plan::PlannerException(plan::BindError{
                .message = "mldp.time_series_table requires a pv predicate"});
        }
    }
}

} // namespace

plan::BoundSelect mldp_pvxs_driver::query::planner::bindSelect(const SelectStatement& statement, const QueryTableCatalog* catalog)
{
    auto from = makeBoundTable(statement.from, catalog);
    std::vector<plan::BoundJoinClause> joins;
    std::vector<std::pair<ResolvedColumn, ResolvedColumn>> join_required_columns;
    joins.reserve(statement.joins.size());
    join_required_columns.reserve(statement.joins.size());

    std::unordered_set<std::string> aliases;
    requireUniqueAlias(from, aliases);
    aliases.insert(from.table_alias);

    std::vector<plan::BoundTable> all_tables;
    all_tables.push_back(from);
    for (const auto& join_clause : statement.joins)
    {
        auto right_table = makeBoundTable(join_clause.table, catalog);
        requireUniqueAlias(right_table, aliases);

        std::vector<plan::BoundTable> join_scope = all_tables;
        join_scope.push_back(right_table);
        QualifiedColumn left_column = join_clause.condition.left;
        QualifiedColumn right_column = join_clause.condition.right;
        if (join_clause.condition.expression)
        {
            const auto* equality = std::get_if<BinaryExpression>(&join_clause.condition.expression->value);
            if (equality == nullptr || equality->operator_name != "=" || !equality->left || !equality->right ||
                !std::holds_alternative<QualifiedColumn>(equality->left->value) || !std::holds_alternative<QualifiedColumn>(equality->right->value))
            {
                throw plan::PlannerException(plan::BindError{.message = "JOIN ON currently requires a column equality expression"});
            }
            left_column = std::get<QualifiedColumn>(equality->left->value);
            right_column = std::get<QualifiedColumn>(equality->right->value);
        }
        const auto left_ref = resolveColumnReference(left_column, join_scope);
        const auto right_ref = resolveColumnReference(right_column, join_scope);

        const bool left_is_existing = aliases.contains(left_ref.table_alias);
        const bool right_is_existing = aliases.contains(right_ref.table_alias);
        const bool left_is_right = left_ref.table_alias == right_table.table_alias;
        const bool right_is_right = right_ref.table_alias == right_table.table_alias;

        plan::LogicalJoinCondition condition;
        if (left_is_existing && right_is_right)
        {
            condition.left_column = qualify(left_ref.table_alias, left_ref.column_name);
            condition.right_column = qualify(right_ref.table_alias, right_ref.column_name);
            join_required_columns.emplace_back(left_ref, right_ref);
        }
        else if (right_is_existing && left_is_right)
        {
            condition.left_column = qualify(right_ref.table_alias, right_ref.column_name);
            condition.right_column = qualify(left_ref.table_alias, left_ref.column_name);
            join_required_columns.emplace_back(right_ref, left_ref);
        }
        else
        {
            throw plan::PlannerException(plan::BindError{
                .message = "JOIN ON must compare one column from already-joined tables to one column from table alias '" +
                    right_table.table_alias + "'"});
        }

        aliases.insert(right_table.table_alias);
        all_tables.push_back(right_table);
        joins.push_back(plan::BoundJoinClause{
            .type = join_clause.type == query::JoinType::LEFT_OUTER
                ? plan::LogicalJoinType::LEFT_OUTER
                : plan::LogicalJoinType::INNER,
            .table = std::move(right_table),
            .condition = std::move(condition)});
    }

    std::unordered_map<std::string, size_t> table_index;
    table_index.reserve(all_tables.size());
    for (size_t index = 0; index < all_tables.size(); ++index)
    {
        table_index[all_tables[index].table_alias] = index;
    }

    const auto bindWindowShardOptions = [](plan::BoundTable& table, const InPredicate& input)
    {
        if (!input.window_options.empty() && table.table_name != "mldp.time_series" && table.table_name != "mldp.time_series_table")
            throw plan::PlannerException(plan::BindError{.message = "Window shard options are supported only for MLDP time-series tables"});
        std::set<std::string> seen;
        for (const auto& option : input.window_options)
        {
            std::string name = option.name;
            std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (!seen.insert(name).second)
                throw plan::PlannerException(plan::BindError{.message = "MLDP time-series window shard option is duplicated: " + name});
            if (name == "slice")
            {
                if (!std::holds_alternative<DurationNsLiteral>(option.value) || std::get<DurationNsLiteral>(option.value).value <= 0)
                    throw plan::PlannerException(plan::BindError{.message = "MLDP time-series window slice must be a positive duration"});
                table.window_shards.slice_ns = std::get<DurationNsLiteral>(option.value).value;
            }
            else if (name == "series_per_shard")
            {
                if (!std::holds_alternative<int64_t>(option.value) || std::get<int64_t>(option.value) <= 0)
                    throw plan::PlannerException(plan::BindError{.message = "MLDP time-series window series_per_shard must be a positive integer"});
                table.window_shards.series_per_shard = static_cast<uint64_t>(std::get<int64_t>(option.value));
            }
            else
            {
                throw plan::PlannerException(plan::BindError{.message = "Unknown MLDP time-series window shard option: " + name});
            }
        }
    };

    // Tables on the right of a LEFT JOIN are NULL-extended; a NULL test on them
    // (the `LEFT JOIN ... WHERE right.key IS NULL` anti-join) must see the join output.
    std::set<std::string> nullable_aliases;
    for (std::size_t index = 0; index < joins.size(); ++index)
        if (joins[index].type == plan::LogicalJoinType::LEFT_OUTER) nullable_aliases.insert(all_tables[index + 1].table_alias);

    std::vector<ExpressionPtr> conditions;
    for (const auto& where : statement.predicates)
    {
        if (auto condition = nullTestCondition(where, all_tables, nullable_aliases))
        {
            (void)bindExpression(condition, all_tables, !joins.empty());
            conditions.push_back(std::move(condition));
            continue;
        }
        // `col <op> other_col`: not a literal predicate, so evaluate it as a boolean
        // expression after joins instead of pushing it to a scan.
        if (auto condition = columnComparisonCondition(where))
        {
            if (bindExpression(condition, all_tables, !joins.empty()) != ColumnType::BOOL)
                throw plan::PlannerException(plan::BindError{.message = "WHERE comparison must be boolean"});
            conditions.push_back(std::move(condition));
            continue;
        }
        if (const auto* in = std::get_if<InPredicate>(&where); in != nullptr && !in->window_options.empty() && in->column.name != "window")
        {
            throw plan::PlannerException(plan::BindError{.message = "Window shard options are supported only for MLDP time-series window input"});
        }
        if (const auto* in = std::get_if<InPredicate>(&where); in != nullptr && in->subquery && in->column.name == "window")
        {
            if (all_tables.size() != 1 ||
                (all_tables.front().table_name != "mldp.time_series" && all_tables.front().table_name != "mldp.time_series_table") ||
                (in->column.qualifier.has_value() && in->column.qualifier.value() != all_tables.front().table_alias &&
                 in->column.qualifier.value() != all_tables.front().table_name))
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "IN (SELECT ...) window input is supported only for mldp.time_series or mldp.time_series_table"});
            }
            auto* const table = &all_tables.front();
            if (table->window_subquery || table->window_literal)
                throw plan::PlannerException(plan::BindError{.message = "MLDP time-series tables accept exactly one window input"});
            table->window_subquery = in->subquery;
            bindWindowShardOptions(*table, *in);
            continue;
        }
        if (const auto* in = std::get_if<InPredicate>(&where); in != nullptr && in->column.name == "window")
        {
            if (all_tables.size() != 1 ||
                (all_tables.front().table_name != "mldp.time_series" && all_tables.front().table_name != "mldp.time_series_table") ||
                (in->column.qualifier.has_value() && in->column.qualifier.value() != all_tables.front().table_alias &&
                 in->column.qualifier.value() != all_tables.front().table_name))
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Literal window IN (...) is supported only for mldp.time_series or mldp.time_series_table"});
            }
            if (all_tables.front().window_literal || all_tables.front().window_subquery)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "MLDP time-series tables accept exactly one window input"});
            }
            const auto& expressions = in->expressions;
            const auto value_count = expressions.empty() ? in->values.size() : expressions.size();
            if (value_count != 2)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "MLDP time-series literal window requires exactly two timestamp expressions"});
            }
            const auto first = expressions.empty() ? in->values[0] : constantExpression(expressions[0]);
            const auto second = expressions.empty() ? in->values[1] : constantExpression(expressions[1]);
            all_tables.front().window_literal = std::array<plan::PlannerLiteralValue, 2>{toPlannerLiteral(first), toPlannerLiteral(second)};
            bindWindowShardOptions(all_tables.front(), *in);
            continue;
        }
        const auto predicate = buildPredicate(where, all_tables);
        auto& table = all_tables[table_index.at(predicate.table_alias)];
        if (const auto* in = std::get_if<InPredicate>(&where); in != nullptr && in->subquery)
            table.in_subqueries.push_back(plan::BoundInSubquery{.predicate = predicate, .child = in->subquery});
        else
            table.predicates.push_back(predicate);
    }

    // OR/NOT trees never reach the backend (except a same-column equality OR, which
    // becomes a pushable IN). Each tree runs as a local filter on its one table.
    for (const auto& tree : statement.predicate_groups)
    {
        auto group = bindPredicateGroup(tree, all_tables);
        if (auto membership = sameColumnMembership(group))
        {
            all_tables[table_index.at(membership->table_alias)].predicates.push_back(std::move(*membership));
            continue;
        }
        std::set<std::string> aliases;
        forEachGroupLeaf(group, [&](const plan::PlannerPredicate& leaf)
        {
            aliases.insert(leaf.table_alias);
            if (!leaf.filterable_ops.contains(leaf.op))
                throw plan::PlannerException(plan::BindError{
                    .message = "Operator on column '" + qualify(leaf.table_alias, leaf.column) +
                               "' is evaluated only by the backend and cannot be used inside OR/NOT"});
        });
        if (aliases.size() != 1)
            throw plan::PlannerException(plan::BindError{.message = "OR/NOT conditions must reference columns of a single table"});
        if (nullable_aliases.contains(*aliases.begin()))
            throw plan::PlannerException(plan::BindError{.message = "OR/NOT conditions on the right side of a LEFT JOIN are not supported"});
        all_tables[table_index.at(*aliases.begin())].predicate_groups.push_back(std::move(group));
    }

    enforceTimeSeriesTableContract(statement, all_tables);

    for (const auto& table : all_tables)
    {
        if ((table.table_name == "mldp.time_series" || table.table_name == "mldp.time_series_table") &&
            table.window_subquery && table.window_literal)
        {
            throw plan::PlannerException(plan::BindError{.message = "MLDP time-series tables accept either a literal window or window IN (SELECT ...), not both"});
        }
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> covered_columns;
    for (const auto& table : all_tables)
    {
        covered_columns[table.table_alias] = {};
        for (const auto& predicate : table.predicates)
        {
            covered_columns[table.table_alias].insert(predicate.column);
        }
        for (const auto& subquery : table.in_subqueries)
            if (subquery.predicate.pushable_ops.contains(PredicateOp::IN)) covered_columns[table.table_alias].insert(subquery.predicate.column);
    }
    for (const auto& [left, right] : join_required_columns)
    {
        covered_columns[left.table_alias].insert(left.column_name);
        covered_columns[right.table_alias].insert(right.column_name);
    }
    for (const auto& table : all_tables)
    {
        ensureColumnCoveredForRequiredCheck(table, covered_columns[table.table_alias]);
    }

    from = all_tables.front();
    for (size_t index = 0; index < joins.size(); ++index)
    {
        joins[index].table = all_tables[index + 1];
    }

    plan::BoundSelect bound{
        .from = std::move(from),
        .joins = std::move(joins),
        .distinct = statement.distinct,
        .distinct_on = {},
        .select_all = statement.select_all,
        .select_columns = {},
        .select_expressions = {},
        .select_names = {},
        .order_by = {},
        .limit = statement.limit,
        .page_token = statement.page_token,
        .conditions = std::move(conditions)};

    const bool multi_table = !bound.joins.empty();
    const auto windowed = resolveStatementWindows(statement);
    if (isAggregateQuery(statement))
    {
        bindAggregateQuery(windowed, all_tables, multi_table, bound);
        return bound;
    }
    for (const auto& key : statement.distinct_on)
    {
        if (bindExpression(key, all_tables, multi_table) == ColumnType::NATIVE_VALUE)
        {
            throw plan::PlannerException(plan::BindError{.message = "DISTINCT ON requires a scalar expression"});
        }
        bound.distinct_on.push_back(key);
    }

    // Window calls become __win_N columns computed by the window step; the select
    // list and ORDER BY bind against the input tables plus those columns.
    WindowBinder windows([&all_tables, multi_table](const ExpressionPtr& expression) { return bindExpression(expression, all_tables, multi_table); });
    const auto windowScope = [&all_tables, &windows]()
    {
        auto scope = all_tables;
        if (!windows.outputs().empty())
            scope.push_back(plan::BoundTable{.table_name = "window", .table_alias = "", .schema = windows.outputs(), .predicates = {}});
        return scope;
    };
    std::vector<ExpressionPtr>           select_expressions;
    std::map<std::string, ExpressionPtr> window_aliases;
    for (const auto& item : windowed.select_items)
    {
        const bool has_window = containsWindow(item.expression);
        select_expressions.push_back(has_window ? windows.rewrite(item.expression) : item.expression);
        if (has_window && item.alias) window_aliases.emplace(*item.alias, select_expressions.back());
    }

    bound.order_by.reserve(windowed.order_by.size());
    for (const auto& item : windowed.order_by)
    {
        const auto expression = item.expression ? item.expression : std::make_shared<Expression>(Expression{.value = item.column});
        ExpressionPtr window_key;
        if (containsWindow(expression)) window_key = windows.rewrite(expression);
        else if (const auto* column = std::get_if<QualifiedColumn>(&expression->value); column && !column->qualifier)
            if (const auto alias = window_aliases.find(column->name); alias != window_aliases.end()) window_key = alias->second;
        if (window_key)
        {
            const auto* reference = std::get_if<QualifiedColumn>(&window_key->value);
            if (reference == nullptr)
                throw plan::PlannerException(plan::BindError{.message = "ORDER BY on a window function must name the call itself or its select alias"});
            if (statement.select_all)
                throw plan::PlannerException(plan::BindError{.message = "ORDER BY a window function requires an explicit select list"});
            if (bindExpression(window_key, windowScope(), multi_table) == ColumnType::NATIVE_VALUE)
                throw plan::PlannerException(plan::BindError{.message = "ORDER BY requires a scalar expression"});
            bound.order_by.push_back(plan::SortKey{.column = reference->name, .expression = window_key,
                                                   .descending = item.direction == SortDirection::DESCENDING});
            continue;
        }
        const auto original_column = std::holds_alternative<QualifiedColumn>(expression->value) ? std::get<QualifiedColumn>(expression->value) : QualifiedColumn{};
        const auto expression_type = bindExpression(expression, all_tables, multi_table);
        if (expression_type == ColumnType::NATIVE_VALUE)
        {
            throw plan::PlannerException(plan::BindError{.message = "ORDER BY requires a scalar expression"});
        }
        const auto resolved = original_column.name.empty() ? ResolvedColumn{} : resolveColumnReference(original_column, all_tables);
        if (!resolved.table_alias.empty())
        {
        const auto& schema = all_tables[table_index.at(resolved.table_alias)].schema;
        const bool dynamic_attribute = resolved.column_name.rfind("attributes.", 0) == 0 ||
                                       resolved.column_name.rfind("provenance.", 0) == 0;
        const auto* column_schema = findColumnSchema(schema, resolved.column_name);
        if ((!dynamic_attribute && (column_schema == nullptr || !column_schema->is_output)) ||
            resolved.column_name == "tags" || resolved.column_name == "attributes" || resolved.column_name == "provenance")
        {
            throw plan::PlannerException(plan::BindError{
                .message = "ORDER BY requires a scalar output column; collection columns such as tags and attributes are not sortable"});
        }
        }
        bound.order_by.push_back(plan::SortKey{
            .column = resolved.column_name,
            .expression = expression,
            .descending = item.direction == SortDirection::DESCENDING});
    }

    if (!statement.select_all)
    {
        bound.select_columns.reserve(statement.select_items.empty() ? statement.columns.size() : statement.select_items.size());
        const auto& items = windowed.select_items;
        if (!items.empty())
        {
            const auto scope = windowScope();
            for (std::size_t index = 0; index < items.size(); ++index)
            {
                const auto& item = items[index];
                bound.select_types.push_back(bindExpression(select_expressions[index], scope, multi_table));
                bound.select_expressions.push_back(select_expressions[index]);
                const auto generated = generatedExpressionName(item.expression);
                if (std::holds_alternative<QualifiedColumn>(item.expression->value))
                {
                    bound.select_columns.push_back(std::get<QualifiedColumn>(item.expression->value).name);
                    bound.select_names.push_back(item.alias.value_or(bound.select_columns.back()));
                }
                else
                {
                    bound.select_columns.push_back(generated);
                    bound.select_names.push_back(item.alias.value_or(generated));
                }
            }
        }
        else for (const auto& column : statement.columns)
        {
            const auto resolved = resolveColumnReference(column, all_tables);
            bound.select_columns.push_back(multi_table ? qualify(resolved.table_alias, resolved.column_name) : resolved.column_name);
            bound.select_expressions.push_back(std::make_shared<Expression>(Expression{.value = QualifiedColumn{.name = bound.select_columns.back()}}));
            bound.select_names.push_back(resolved.column_name);
        }
    }

    bound.windows = std::move(windows.groups());
    return bound;
}
