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

#include <query/parser/QueryAST.h>
#include <query/parser/Token.h>

#include <cstddef>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query::generated {

struct ParseContext {
    const std::vector<Token>& tokens;
    std::size_t               index{0};
    QueryStatement            result;
    TokenPosition             last_position{};
    bool                      has_last_position{false};
};

} // namespace mldp_pvxs_driver::query::generated
