//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdexcept>
#include <string>
#include <variant>

namespace mldp_pvxs_driver::query::plan {

struct BindError {
    std::string message;

    [[nodiscard]] std::string what() const
    {
        return "BindError: " + message;
    }
};

struct TypeError {
    std::string message;

    [[nodiscard]] std::string what() const
    {
        return "TypeError: " + message;
    }
};

struct PlanError {
    std::string message;

    [[nodiscard]] std::string what() const
    {
        return "PlanError: " + message;
    }
};

using PlannerError = std::variant<BindError, TypeError, PlanError>;

inline std::string plannerErrorWhat(const PlannerError& error)
{
    return std::visit([](const auto& e) { return e.what(); }, error);
}

class PlannerException : public std::runtime_error
{
public:
    explicit PlannerException(PlannerError error)
        : std::runtime_error(plannerErrorWhat(error))
        , error_(std::move(error))
    {
    }

    [[nodiscard]] const PlannerError& error() const
    {
        return error_;
    }

private:
    PlannerError error_;
};

} // namespace mldp_pvxs_driver::query::plan
