//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/IQueryable.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

enum class ExpressionCallableKind { FUNCTION, BINARY_OPERATOR, UNARY_OPERATOR };

struct ExpressionCallableDescriptor {
    std::string                name;
    ExpressionCallableKind     kind{ExpressionCallableKind::FUNCTION};
    std::vector<ColumnType>    arguments;
    ColumnType                 returns{ColumnType::STRING};
    std::string                description;
    std::string                example;
};

/** A typed SQL scalar callable.  Instances are immutable after registration. */
class IExpressionCallable
{
public:
    virtual ~IExpressionCallable() = default;

    [[nodiscard]] virtual const ExpressionCallableDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual bool accepts(const std::vector<ColumnType>& arguments) const noexcept;
    [[nodiscard]] virtual ColumnType inferReturnType(const std::vector<ColumnType>& arguments) const;
};

/** Immutable catalog of executable SQL scalar functions and operators. */
class ExpressionRegistry
{
public:
    ExpressionRegistry();

    [[nodiscard]] const IExpressionCallable& resolveFunction(std::string_view name, const std::vector<ColumnType>& arguments) const;
    [[nodiscard]] const IExpressionCallable& resolveOperator(std::string_view symbol, ExpressionCallableKind kind, const std::vector<ColumnType>& arguments) const;
    [[nodiscard]] std::vector<ExpressionCallableDescriptor> functions() const;
    [[nodiscard]] std::vector<ExpressionCallableDescriptor> operators() const;

private:
    void registerCallable(std::unique_ptr<IExpressionCallable> callable);
    std::vector<std::unique_ptr<IExpressionCallable>> callables_;
};

[[nodiscard]] std::string columnTypeName(ColumnType type);
[[nodiscard]] std::string expressionArgumentsText(const std::vector<ColumnType>& arguments);

} // namespace mldp_pvxs_driver::query
