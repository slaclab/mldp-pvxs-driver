//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/parser/Lexer.h>

#include <query/parser/generated/QueryFlexLexer.h>

#include <string>

using namespace mldp_pvxs_driver::query;

struct FlexLexerExtra
{
    std::vector<Token>* out_tokens{nullptr};
    std::size_t         offset{0};
    std::size_t         line{1};
    std::size_t         column{1};
    bool                failed{false};
    TokenPosition       error_position{};
    std::string         error_message;
};

Lexer::Lexer(const std::string_view input)
    : input_(input)
{
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;
    FlexLexerExtra extra{
        .out_tokens = &tokens,
        .offset = 0,
        .line = 1,
        .column = 1,
        .failed = false,
        .error_position = {},
        .error_message = {}};

    yyscan_t scanner{};
    if (mldp_query_lex_init_extra(&extra, &scanner) != 0)
    {
        throw ParseError("Failed to initialize query lexer", TokenPosition{0, 1, 1});
    }

    struct ScannerGuard
    {
        yyscan_t scanner;
        ~ScannerGuard()
        {
            if (scanner != nullptr)
            {
                mldp_query_lex_destroy(scanner);
            }
        }
    } scanner_guard{scanner};

    auto* buffer = mldp_query__scan_bytes(input_.data(), static_cast<int>(input_.size()), scanner);
    if (buffer == nullptr)
    {
        throw ParseError("Failed to create lexer input buffer", TokenPosition{0, 1, 1});
    }

    struct BufferGuard
    {
        YY_BUFFER_STATE buffer;
        yyscan_t        scanner;
        ~BufferGuard()
        {
            if (buffer != nullptr)
            {
                mldp_query__delete_buffer(buffer, scanner);
            }
        }
    } buffer_guard{buffer, scanner};

    while (mldp_query_lex(scanner) != 0)
    {
    }

    if (extra.failed)
    {
        throw ParseError(extra.error_message, extra.error_position);
    }

    tokens.push_back(Token{
        .type = TokenType::END_OF_INPUT,
        .lexeme = "",
        .position = TokenPosition{extra.offset, extra.line, extra.column}});
    return tokens;
}
