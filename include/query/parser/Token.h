//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file Token.h
 * @brief Defines lexical token kinds and source locations. */
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mldp_pvxs_driver::query {

/** @brief Lexical token categories accepted by the SQL grammar. */
enum class TokenType {
    END_OF_INPUT,     ///< Sentinel token at end of input.
    IDENTIFIER,       ///< Unquoted name or keyword not otherwise classified.
    STRING_LITERAL,   ///< Single-quoted string literal.
    NUMBER_LITERAL,   ///< Numeric literal (integer or floating-point).
    DURATION_LITERAL, ///< Duration literal with a unit suffix (e.g. 5s, 100ms).
    TRUE,             ///< Boolean true literal.
    FALSE,            ///< Boolean false literal.
    TIMESTAMP_NS,     ///< Nanosecond-precision timestamp literal.
    DURATION_NS,      ///< Nanosecond-precision duration literal.

    SELECT,       ///< SELECT keyword.
    FROM,         ///< FROM keyword.
    WHERE,        ///< WHERE keyword.
    IS,           ///< IS keyword.
    AND,          ///< AND keyword.
    OR,           ///< OR keyword.
    NOT,          ///< NOT keyword.
    NULL_LITERAL, ///< NULL literal.
    IN,           ///< IN keyword.
    LIKE,         ///< LIKE keyword.
    BETWEEN,      ///< BETWEEN keyword.
    LIMIT,        ///< LIMIT keyword.
    PAGE,         ///< PAGE keyword.
    TOKEN,        ///< TOKEN keyword.
    SHOW,         ///< SHOW keyword.
    TABLES,       ///< TABLES keyword.
    FUNCTIONS,    ///< FUNCTIONS keyword.
    OPERATORS,    ///< OPERATORS keyword.
    DESCRIBE,     ///< DESCRIBE keyword.
    EXPLAIN,      ///< EXPLAIN keyword.
    AS,           ///< AS keyword.
    INNER,        ///< INNER keyword.
    LEFT,         ///< LEFT keyword.
    OUTER,        ///< OUTER keyword.
    JOIN,         ///< JOIN keyword.
    ON,           ///< ON keyword.
    NOW,          ///< NOW() function keyword.
    PREFIX,       ///< PREFIX keyword.
    CONTAINS,     ///< CONTAINS keyword.
    ORDER,        ///< ORDER keyword.
    BY,           ///< BY keyword.
    ASC,          ///< ASC keyword.
    DESC,         ///< DESC keyword.

    STAR,      ///< Asterisk (*).
    SLASH,     ///< Forward slash (/).
    COMMA,     ///< Comma (,).
    SEMICOLON, ///< Semicolon (;).
    DOT,       ///< Dot (.).
    LPAREN,    ///< Left parenthesis.
    RPAREN,    ///< Right parenthesis.
    PLUS,      ///< Plus sign (+).
    MINUS,     ///< Minus sign (-).
    EQ,        ///< Equality operator (=).
    NEQ,       ///< Inequality operator (<>).
    LT,        ///< Less-than operator (<).
    LTE,       ///< Less-than-or-equal operator (<=).
    GT,        ///< Greater-than operator (>).
    GTE        ///< Greater-than-or-equal operator (>=).
};

/** @brief Zero-based byte offset and one-based line and column location. */
struct TokenPosition {
    std::size_t offset{0}; ///< Zero-based byte offset in the source string.
    std::size_t line{1};   ///< One-based line number.
    std::size_t column{1}; ///< One-based column number within the line.
};

/** @brief A classified source lexeme with its original location. */
struct Token {
    TokenType     type{TokenType::END_OF_INPUT}; ///< Lexical category.
    std::string   lexeme;                        ///< Original source text for this token.
    TokenPosition position;                      ///< Source location of this token.
};

/** @brief Reports a syntax error together with its source position. */
class ParseError : public std::runtime_error
{
public:
    ParseError(std::string message, TokenPosition position)
        : std::runtime_error(std::move(message))
        , position_(position)
    {
    }

    /** @brief Returns the zero-based byte offset of the error. @return Byte offset. */
    [[nodiscard]] std::size_t offset() const { return position_.offset; }
    /** @brief Returns the one-based line number of the error. @return Line number. */
    [[nodiscard]] std::size_t line() const { return position_.line; }
    /** @brief Returns the one-based column of the error. @return Column number. */
    [[nodiscard]] std::size_t column() const { return position_.column; }
    /** @brief Returns the full source position of the error. @return Position struct. */
    [[nodiscard]] const TokenPosition& position() const { return position_; }

private:
    TokenPosition position_;
};

/** @brief Returns a short human-readable label for a token type.
 * @param[in] type Token type.
 * @return Null-terminated string label. */
std::string_view tokenTypeToString(TokenType type);

} // namespace mldp_pvxs_driver::query
