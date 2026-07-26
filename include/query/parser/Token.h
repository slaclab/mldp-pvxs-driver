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

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mldp_pvxs_driver::query {

enum class TokenType {
    END_OF_INPUT,
    IDENTIFIER,
    STRING_LITERAL,
    NUMBER_LITERAL,
    DURATION_LITERAL,
    TRUE,
    FALSE,
    TIMESTAMP_NS,
    DURATION_NS,

    SELECT,
    FROM,
    WHERE,
    IS,
    AND,
    OR,
    NOT,
    NULL_LITERAL,
    IN,
    LIKE,
    BETWEEN,
    LIMIT,
    PAGE,
    TOKEN,
    SHOW,
    TABLES,
    FUNCTIONS,
    OPERATORS,
    DESCRIBE,
    EXPLAIN,
    AS,
    INNER,
    LEFT,
    OUTER,
    JOIN,
    ON,
    NOW,
    PREFIX,
    CONTAINS,
    ORDER,
    BY,
    ASC,
    DESC,

    STAR,
    SLASH,
    COMMA,
    DOT,
    LPAREN,
    RPAREN,
    PLUS,
    MINUS,
    EQ,
    NEQ,
    LT,
    LTE,
    GT,
    GTE
};

struct TokenPosition {
    std::size_t offset{0};
    std::size_t line{1};
    std::size_t column{1};
};

struct Token {
    TokenType     type{TokenType::END_OF_INPUT};
    std::string   lexeme;
    TokenPosition position;
};

class ParseError : public std::runtime_error
{
public:
    ParseError(std::string message, TokenPosition position)
        : std::runtime_error(std::move(message))
        , position_(position)
    {
    }

    [[nodiscard]] std::size_t offset() const { return position_.offset; }
    [[nodiscard]] std::size_t line() const { return position_.line; }
    [[nodiscard]] std::size_t column() const { return position_.column; }
    [[nodiscard]] const TokenPosition& position() const { return position_; }

private:
    TokenPosition position_;
};

std::string_view tokenTypeToString(TokenType type);

} // namespace mldp_pvxs_driver::query
