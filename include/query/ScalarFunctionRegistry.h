//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/IQueryable.h>
#include <query/parser/QueryAST.h>

#include <string>
#include <utility>
#include <vector>

namespace mldp_pvxs_driver::query {

struct ScalarFunctionSignature {
    std::vector<ColumnType> arguments;
    ColumnType              return_type{ColumnType::STRING};
};

class ScalarFunctionRegistry
{
public:
    ScalarFunctionRegistry();
    ColumnType returnType(const FunctionCall& call, const std::vector<ColumnType>& arguments) const;
    int64_t evaluateTimestamp(const FunctionCall& call) const;

private:
    std::vector<std::pair<std::string, std::vector<ScalarFunctionSignature>>> signatures_;
};

} // namespace mldp_pvxs_driver::query
