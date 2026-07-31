//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file ScalarFunctionRegistry.h
 * @brief Declares SQL scalar-function discovery and evaluation metadata. */
#pragma once

#include <query/ExpressionRegistry.h>
#include <query/parser/QueryAST.h>

#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Resolves scalar function signatures and evaluates planner-supported timestamp calls. */
class ScalarFunctionRegistry
{
public:
    ScalarFunctionRegistry();
    ColumnType returnType(const FunctionCall& call, const std::vector<ColumnType>& arguments) const;
    int64_t evaluateTimestamp(const FunctionCall& call) const;

private:
    ExpressionRegistry registry_;
};

} // namespace mldp_pvxs_driver::query
