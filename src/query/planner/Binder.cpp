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
#include <query/ExpressionRegistry.h>
#include <query/plan/PlannerError.h>

#include <arrow/type.h>

#include <sstream>
#include <cctype>
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

LiteralValue constantExpression(const ExpressionPtr& expression)
{
    if (!expression) throw plan::PlannerException(plan::BindError{.message = "Missing predicate expression"});
    if (const auto* literal = std::get_if<LiteralValue>(&expression->value)) return *literal;
    if (const auto* function = std::get_if<FunctionCall>(&expression->value)) return ScalarFunctionRegistry{}.evaluateTimestamp(*function);
    throw plan::PlannerException(plan::BindError{.message = "WHERE expression must be constant"});
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
            value.name = multi_table ? qualify(resolved.table_alias, resolved.column_name) : resolved.column_name;
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

plan::BoundTable makeBoundTable(const TableRef& table_ref, const QueryTableCatalog* catalog)
{
    if (table_ref.derived_query)
    {
        const auto child = bindSelect(*table_ref.derived_query, catalog);
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
            for (const auto& name : child.select_columns)
            {
                const auto dot = name.find('.');
                const auto local = name.substr(dot == std::string::npos ? 0 : dot + 1);
                if (const auto* column = findColumnSchema(schema, local)) selected.push_back(*column);
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

    for (const auto& where : statement.predicates)
    {
        if (const auto* in = std::get_if<InPredicate>(&where); in != nullptr && in->subquery && in->column.name == "window")
        {
            const auto resolved = resolveColumnReference(in->column, all_tables);
            auto* const table = &all_tables[table_index.at(resolved.table_alias)];
            const bool is_wide_time_series = table->table_name == "mldp.time_series_table";

            if (!is_wide_time_series || resolved.column_name != "window")
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "IN (SELECT ...) window input is supported only for mldp.time_series_table"});
            }
            if (table->window_subquery || table->window_literal)
                throw plan::PlannerException(plan::BindError{.message = "mldp.time_series_table accepts exactly one window input"});
            table->window_subquery = in->subquery;
            continue;
        }
        if (const auto* in = std::get_if<InPredicate>(&where); in != nullptr && in->column.name == "window")
        {
            if (all_tables.size() != 1 || all_tables.front().table_name != "mldp.time_series_table" ||
                (in->column.qualifier.has_value() && in->column.qualifier.value() != all_tables.front().table_alias &&
                 in->column.qualifier.value() != all_tables.front().table_name))
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "Literal window IN (...) is supported only for mldp.time_series_table"});
            }
            if (all_tables.front().window_literal || all_tables.front().window_subquery)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "mldp.time_series_table accepts exactly one window input"});
            }
            const auto& expressions = in->expressions;
            const auto value_count = expressions.empty() ? in->values.size() : expressions.size();
            if (value_count != 2)
            {
                throw plan::PlannerException(plan::BindError{
                    .message = "mldp.time_series_table literal window requires exactly two timestamp expressions"});
            }
            const auto first = expressions.empty() ? in->values[0] : constantExpression(expressions[0]);
            const auto second = expressions.empty() ? in->values[1] : constantExpression(expressions[1]);
            all_tables.front().window_literal = std::array<plan::PlannerLiteralValue, 2>{toPlannerLiteral(first), toPlannerLiteral(second)};
            continue;
        }
        const auto predicate = buildPredicate(where, all_tables);
        auto& table = all_tables[table_index.at(predicate.table_alias)];
        if (const auto* in = std::get_if<InPredicate>(&where); in != nullptr && in->subquery)
            table.in_subqueries.push_back(plan::BoundInSubquery{.predicate = predicate, .child = in->subquery});
        else
            table.predicates.push_back(predicate);
    }

    enforceTimeSeriesTableContract(statement, all_tables);

    for (const auto& table : all_tables)
    {
        if (table.table_name == "mldp.time_series_table" && table.window_subquery && table.window_literal)
        {
            throw plan::PlannerException(plan::BindError{.message = "mldp.time_series_table accepts either a literal window or window IN (SELECT ...), not both"});
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
        .select_all = statement.select_all,
        .select_columns = {},
        .select_expressions = {},
        .select_names = {},
        .order_by = {},
        .limit = statement.limit,
        .page_token = statement.page_token};

    const bool multi_table = !bound.joins.empty();
    bound.order_by.reserve(statement.order_by.size());
    for (const auto& item : statement.order_by)
    {
        const auto expression = item.expression ? item.expression : std::make_shared<Expression>(Expression{.value = item.column});
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
        const auto& items = statement.select_items;
        if (!items.empty())
        {
            for (const auto& item : items)
            {
                bindExpression(item.expression, all_tables, multi_table);
                bound.select_expressions.push_back(item.expression);
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

    return bound;
}
