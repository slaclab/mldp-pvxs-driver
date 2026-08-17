//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file ExpressionRegistry.h
 * @brief Declares the typed catalog of SQL scalar functions and operators. */
#pragma once

#include <query/IQueryable.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief SQL callable categories used for resolution and discovery. */
enum class ExpressionCallableKind {
    FUNCTION,         ///< Named scalar function (e.g. DATE_TRUNC).
    BINARY_OPERATOR,  ///< Infix binary operator (e.g. +, -, LIKE).
    UNARY_OPERATOR    ///< Prefix unary operator (e.g. NOT, -).
};

/** @brief Immutable metadata for an executable scalar function or operator. */
struct ExpressionCallableDescriptor {
    std::string                name;                                    ///< Callable name used in SQL.
    ExpressionCallableKind     kind{ExpressionCallableKind::FUNCTION};  ///< Function, binary operator, or unary operator.
    std::vector<ColumnType>    arguments;                               ///< Expected argument types in order.
    ColumnType                 returns{ColumnType::STRING};             ///< Return type.
    std::string                description;                             ///< Human-readable description for SHOW FUNCTIONS.
    std::string                example;                                 ///< Example usage string for SHOW FUNCTIONS.
};

/** @brief Typed SQL scalar callable; immutable once registered. */
class IExpressionCallable
{
public:
    virtual ~IExpressionCallable() = default;

    /** @brief Returns the immutable descriptor for this callable.
     * @return Const reference to the descriptor. */
    [[nodiscard]] virtual const ExpressionCallableDescriptor& descriptor() const noexcept = 0;

    /** @brief Returns true if this callable accepts the given argument types.
     * @param[in] arguments Argument types to check.
     * @return True when the call is valid. */
    [[nodiscard]] virtual bool accepts(const std::vector<ColumnType>& arguments) const noexcept;

    /** @brief Infers the return type for the given argument types.
     * @param[in] arguments Argument types.
     * @return Inferred return type.
     * @throws std::runtime_error If the arguments are invalid. */
    [[nodiscard]] virtual ColumnType inferReturnType(const std::vector<ColumnType>& arguments) const;
};

/** @brief Immutable registry of all built-in SQL scalar functions and operators. */
class ExpressionRegistry
{
public:
    /** @brief Constructs the registry and registers all built-in callables. */
    ExpressionRegistry();

    /** @brief Looks up a function by name and argument types.
     * @param[in] name Function name (case-insensitive).
     * @param[in] arguments Argument types.
     * @return Matching callable.
     * @throws std::runtime_error If no matching function is found. */
    [[nodiscard]] const IExpressionCallable& resolveFunction(std::string_view name, const std::vector<ColumnType>& arguments) const;

    /** @brief Looks up an operator by symbol, kind, and argument types.
     * @param[in] symbol Operator symbol.
     * @param[in] kind BINARY_OPERATOR or UNARY_OPERATOR.
     * @param[in] arguments Argument types.
     * @return Matching callable.
     * @throws std::runtime_error If no matching operator is found. */
    [[nodiscard]] const IExpressionCallable& resolveOperator(std::string_view symbol, ExpressionCallableKind kind, const std::vector<ColumnType>& arguments) const;

    /** @brief Returns descriptors for all registered functions.
     * @return Descriptor vector for SHOW FUNCTIONS. */
    [[nodiscard]] std::vector<ExpressionCallableDescriptor> functions() const;

    /** @brief Returns descriptors for all registered operators.
     * @return Descriptor vector for SHOW OPERATORS. */
    [[nodiscard]] std::vector<ExpressionCallableDescriptor> operators() const;

private:
    void registerCallable(std::unique_ptr<IExpressionCallable> callable);
    std::vector<std::unique_ptr<IExpressionCallable>> callables_;
};

/** @brief Returns the display name for a column type.
 * @param[in] type Column type.
 * @return Name string. */
[[nodiscard]] std::string columnTypeName(ColumnType type);

/** @brief Formats a list of argument types as a parenthesized string.
 * @param[in] arguments Argument types.
 * @return Formatted string like "(STRING, INT)". */
[[nodiscard]] std::string expressionArgumentsText(const std::vector<ColumnType>& arguments);

} // namespace mldp_pvxs_driver::query
