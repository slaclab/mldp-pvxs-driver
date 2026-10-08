//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/WindowFunctionRegistry.h>

#include <algorithm>
#include <cctype>
#include <string>

using namespace mldp_pvxs_driver::query;

const WindowFunctionRegistry& WindowFunctionRegistry::instance()
{
    static const WindowFunctionRegistry registry;
    return registry;
}

WindowFunctionRegistry::WindowFunctionRegistry()
{
    const auto add = [this](const WindowFunctionKind kind, std::string name, std::string arguments, std::string returns, std::string description, std::string example)
    {
        functions_.push_back(WindowFunctionInfo{
            .kind = kind,
            .descriptor = ExpressionCallableDescriptor{std::move(name), kind == WindowFunctionKind::AGGREGATE ? ExpressionCallableKind::AGGREGATE : ExpressionCallableKind::WINDOW,
                                                       {}, ColumnType::INT, std::move(description), std::move(example), std::move(arguments), std::move(returns)}});
    };
    using Kind = WindowFunctionKind;
    add(Kind::RANKING, "row_number", "()", "int", "Sequential row number within the partition, starting at 1.", "ROW_NUMBER() OVER (PARTITION BY pv ORDER BY time)");
    add(Kind::RANKING, "rank", "()", "int", "Rank within the partition; ties share a rank and leave gaps.", "RANK() OVER (ORDER BY value DESC)");
    add(Kind::RANKING, "dense_rank", "()", "int", "Rank within the partition; ties share a rank, no gaps.", "DENSE_RANK() OVER (ORDER BY value DESC)");
    add(Kind::OFFSET, "lag", "(any [, int [, any]])", "input type", "Value n rows before the current row in the partition (default n = 1), else default or NULL.",
        "LAG(value) OVER (PARTITION BY pv ORDER BY time)");
    add(Kind::OFFSET, "lead", "(any [, int [, any]])", "input type", "Value n rows after the current row in the partition (default n = 1), else default or NULL.",
        "LEAD(value, 2) OVER (PARTITION BY pv ORDER BY time)");
    add(Kind::VALUE, "first_value", "(any)", "input type", "Value at the first row of the window frame.", "FIRST_VALUE(value) OVER w");
    add(Kind::VALUE, "last_value", "(any)", "input type", "Value at the last row of the window frame.",
        "LAST_VALUE(value) OVER (ORDER BY time ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)");
    for (const auto* name : {"sum", "avg", "min", "max", "count"})
        add(Kind::AGGREGATE, name, "", "", "", "");
}

const WindowFunctionInfo* WindowFunctionRegistry::find(const std::string_view name) const
{
    std::string wanted(name);
    std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const auto& function : functions_)
        if (function.descriptor.name == wanted) return &function;
    return nullptr;
}

std::vector<ExpressionCallableDescriptor> WindowFunctionRegistry::functions() const
{
    std::vector<ExpressionCallableDescriptor> result;
    for (const auto& function : functions_)
        if (function.kind != WindowFunctionKind::AGGREGATE) result.push_back(function.descriptor);
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.name < rhs.name; });
    return result;
}
