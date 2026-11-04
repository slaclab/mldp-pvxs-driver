//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file WindowFunctionRegistry.h
 * @brief Declares the catalog of SQL window functions (`f(...) OVER ...`).
 *
 * Ranking, offset and value functions are window-only and listed by SHOW
 * FUNCTIONS with kind "window".  The aggregates SUM, AVG, MIN, MAX and COUNT
 * are also accepted with OVER; they keep their AggregateRegistry entry and
 * type rules and are computed over the window frame. */
#pragma once

#include <query/ExpressionRegistry.h>

#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief How a window function uses its partition. */
enum class WindowFunctionKind
{
    RANKING,   ///< ROW_NUMBER, RANK, DENSE_RANK: position in the ordered partition; frame ignored.
    OFFSET,    ///< LAG, LEAD: value at a row offset in the partition; frame ignored.
    VALUE,     ///< FIRST_VALUE, LAST_VALUE: value at a frame edge.
    AGGREGATE  ///< SUM, AVG, MIN, MAX, COUNT computed over the frame.
};

/** @brief One window-capable function. */
struct WindowFunctionInfo
{
    WindowFunctionKind           kind;       ///< Evaluation kind.
    ExpressionCallableDescriptor descriptor; ///< Name and documentation (kind WINDOW, or AGGREGATE for windowed aggregates).
};

/** @brief Immutable registry of the built-in window functions. */
class WindowFunctionRegistry
{
public:
    /** @brief Returns the process-wide registry. */
    static const WindowFunctionRegistry& instance();

    /** @brief Looks up a function usable with OVER (case-insensitive); null when unknown. */
    [[nodiscard]] const WindowFunctionInfo* find(std::string_view name) const;

    /** @brief Returns descriptors of the window-only functions, sorted by name (aggregates are listed by AggregateRegistry). */
    [[nodiscard]] std::vector<ExpressionCallableDescriptor> functions() const;

private:
    WindowFunctionRegistry();
    std::vector<WindowFunctionInfo> functions_;
};

} // namespace mldp_pvxs_driver::query
