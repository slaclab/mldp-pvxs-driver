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

#include <query/parser/Token.h>

#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

class Lexer
{
public:
    explicit Lexer(std::string_view input);

    std::vector<Token> tokenize();

private:
    std::string_view input_;
    std::size_t      index_{0};
    std::size_t      line_{1};
    std::size_t      column_{1};

    [[nodiscard]] bool atEnd() const;
    [[nodiscard]] char current() const;
    [[nodiscard]] char peek(std::size_t lookahead = 1) const;
    char               advance();
    void               skipWhitespace();
    [[nodiscard]] TokenPosition position() const;
    Token              makeToken(TokenType type, std::size_t start_index, std::size_t start_line, std::size_t start_column) const;
    Token              readIdentifierOrKeyword();
    Token              readNumberOrDuration();
    Token              readStringLiteral();
};

} // namespace mldp_pvxs_driver::query
