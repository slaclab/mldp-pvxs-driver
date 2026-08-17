//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryParser.h
 * @brief Declares parsing of SQL text into query statements. */
#pragma once

#include <query/parser/QueryAST.h>
#include <query/parser/Token.h>

#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Consumes lexer tokens and produces one validated query AST. */
class QueryParser
{
public:
    /** @brief Constructs a parser over an already-tokenized sequence.
     * @param[in] tokens Token sequence produced by Lexer::tokenize(). */
    explicit QueryParser(std::vector<Token> tokens);

    /** @brief Parses the token sequence into a QueryStatement.
     * @return Parsed statement.
     * @throws ParseError On syntax errors. */
    QueryStatement parse();

    /** @brief Convenience: lexes and parses a SQL string in one call.
     * @param[in] sql SQL text to parse.
     * @return Parsed statement.
     * @throws ParseError On syntax errors. */
    static QueryStatement parse(std::string_view sql);

private:
    std::vector<Token> tokens_;
};

/** @brief Lexes and parses a SQL string, throwing ParseError on syntax errors.
 * @param[in] sql SQL text.
 * @return Parsed QueryStatement.
 * @throws ParseError On syntax errors. */
QueryStatement parseQuery(std::string_view sql);

} // namespace mldp_pvxs_driver::query
