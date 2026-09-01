//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file PlannerError.h
 * @brief Defines structured bind, type, and planning errors. */
#pragma once

#include <stdexcept>
#include <string>
#include <variant>

namespace mldp_pvxs_driver::query::plan {

/** @brief Error produced while resolving SQL names and schema constraints. */
struct BindError {
    std::string message; ///< Human-readable error description.

    /** @brief Returns the error message with a BindError prefix. @return Formatted error string. */
    [[nodiscard]] std::string what() const
    {
        return "BindError: " + message;
    }
};

/** @brief Error produced while checking expression types. */
struct TypeError {
    std::string message; ///< Human-readable error description.

    /** @brief Returns the error message with a TypeError prefix. @return Formatted error string. */
    [[nodiscard]] std::string what() const
    {
        return "TypeError: " + message;
    }
};

/** @brief Error produced while constructing or validating a query plan. */
struct PlanError {
    std::string message; ///< Human-readable error description.

    /** @brief Returns the error message with a PlanError prefix. @return Formatted error string. */
    [[nodiscard]] std::string what() const
    {
        return "PlanError: " + message;
    }
};

/** @brief Discriminated union of all structured planner errors. */
using PlannerError = std::variant<BindError, TypeError, PlanError>;

/** @brief Returns the what() string for any PlannerError variant.
 * @param[in] error The error to describe.
 * @return Error description string. */
inline std::string plannerErrorWhat(const PlannerError& error)
{
    return std::visit([](const auto& e) { return e.what(); }, error);
}

/** @brief Exception wrapper that retains the structured planner error. */
class PlannerException : public std::runtime_error
{
public:
    explicit PlannerException(PlannerError error)
        : std::runtime_error(plannerErrorWhat(error))
        , error_(std::move(error))
    {
    }

    /** @brief Returns the structured planner error retained in this exception.
     * @return Const reference to the underlying PlannerError. */
    [[nodiscard]] const PlannerError& error() const
    {
        return error_;
    }

private:
    PlannerError error_;
};

} // namespace mldp_pvxs_driver::query::plan
