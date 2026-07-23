//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/parser/QueryParser.h>

#include <query/parser/Lexer.h>
#include <query/parser/generated/QueryBisonContext.h>
#include <query/parser/generated/QueryBisonParser.hpp>

using namespace mldp_pvxs_driver::query;

QueryParser::QueryParser(std::vector<Token> tokens)
    : tokens_(std::move(tokens))
{
}

QueryStatement QueryParser::parse()
{
    generated::ParseContext context{
        .tokens = tokens_,
        .index = 0,
        .result = QueryStatement{}};
    generated::QueryBisonParser parser(context);
    if (parser.parse() != 0)
    {
        const auto position = context.has_last_position
            ? context.last_position
            : TokenPosition{0, 1, 1};
        throw ParseError("Failed to parse query", position);
    }
    return context.result;
}

QueryStatement QueryParser::parse(const std::string_view sql)
{
    return QueryParser(Lexer(sql).tokenize()).parse();
}

mldp_pvxs_driver::query::QueryStatement mldp_pvxs_driver::query::parseQuery(const std::string_view sql)
{
    return QueryParser::parse(sql);
}
