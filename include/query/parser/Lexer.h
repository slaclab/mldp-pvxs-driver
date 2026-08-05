//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file Lexer.h
 * @brief Declares the SQL lexer wrapper used by the query parser. */
#pragma once

#include <query/parser/Token.h>

#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Tokenizes SQL source text while retaining token positions. */
class Lexer
{
public:
    /** @brief Constructs a lexer over the given SQL input.
     * @param[in] input SQL source text to tokenize; must outlive the Lexer. */
    explicit Lexer(std::string_view input);

    /** @brief Tokenizes the entire input and returns the token sequence.
     * @return Token vector ending with an END_OF_INPUT sentinel.
     * @throws ParseError On unrecognized characters or malformed literals. */
    std::vector<Token> tokenize();

private:
    std::string_view input_;    ///< Source text being tokenized.
    std::size_t      index_{0}; ///< Current read position (byte index).
    std::size_t      line_{1};  ///< Current one-based line number.
    std::size_t      column_{1}; ///< Current one-based column number.

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
