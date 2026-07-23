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

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace mldp_pvxs_driver::query {

namespace {

std::string toUpper(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::toupper(c));
        });
    return value;
}

bool isIdentifierStart(char c)
{
    const auto ch = static_cast<unsigned char>(c);
    return std::isalpha(ch) != 0 || c == '_';
}

bool isIdentifierPart(char c)
{
    const auto ch = static_cast<unsigned char>(c);
    return std::isalnum(ch) != 0 || c == '_';
}

const std::unordered_map<std::string, TokenType> kKeywords = {
    {"SELECT", TokenType::SELECT},
    {"FROM", TokenType::FROM},
    {"WHERE", TokenType::WHERE},
    {"AND", TokenType::AND},
    {"IN", TokenType::IN},
    {"LIKE", TokenType::LIKE},
    {"BETWEEN", TokenType::BETWEEN},
    {"LIMIT", TokenType::LIMIT},
    {"PAGE", TokenType::PAGE},
    {"TOKEN", TokenType::TOKEN},
    {"SHOW", TokenType::SHOW},
    {"TABLES", TokenType::TABLES},
    {"DESCRIBE", TokenType::DESCRIBE},
    {"EXPLAIN", TokenType::EXPLAIN},
    {"AS", TokenType::AS},
    {"INNER", TokenType::INNER},
    {"LEFT", TokenType::LEFT},
    {"OUTER", TokenType::OUTER},
    {"JOIN", TokenType::JOIN},
    {"ON", TokenType::ON},
    {"NOW", TokenType::NOW},
    {"PREFIX", TokenType::PREFIX},
    {"CONTAINS", TokenType::CONTAINS},
};

} // namespace

std::string_view tokenTypeToString(const TokenType type)
{
    switch (type)
    {
    case TokenType::END_OF_INPUT:
        return "end-of-input";
    case TokenType::IDENTIFIER:
        return "identifier";
    case TokenType::STRING_LITERAL:
        return "string literal";
    case TokenType::NUMBER_LITERAL:
        return "number literal";
    case TokenType::DURATION_LITERAL:
        return "duration literal";
    case TokenType::SELECT:
        return "SELECT";
    case TokenType::FROM:
        return "FROM";
    case TokenType::WHERE:
        return "WHERE";
    case TokenType::AND:
        return "AND";
    case TokenType::IN:
        return "IN";
    case TokenType::LIKE:
        return "LIKE";
    case TokenType::BETWEEN:
        return "BETWEEN";
    case TokenType::LIMIT:
        return "LIMIT";
    case TokenType::PAGE:
        return "PAGE";
    case TokenType::TOKEN:
        return "TOKEN";
    case TokenType::SHOW:
        return "SHOW";
    case TokenType::TABLES:
        return "TABLES";
    case TokenType::DESCRIBE:
        return "DESCRIBE";
    case TokenType::EXPLAIN:
        return "EXPLAIN";
    case TokenType::AS:
        return "AS";
    case TokenType::INNER:
        return "INNER";
    case TokenType::LEFT:
        return "LEFT";
    case TokenType::OUTER:
        return "OUTER";
    case TokenType::JOIN:
        return "JOIN";
    case TokenType::ON:
        return "ON";
    case TokenType::NOW:
        return "NOW";
    case TokenType::PREFIX:
        return "PREFIX";
    case TokenType::CONTAINS:
        return "CONTAINS";
    case TokenType::STAR:
        return "*";
    case TokenType::COMMA:
        return ",";
    case TokenType::DOT:
        return ".";
    case TokenType::LPAREN:
        return "(";
    case TokenType::RPAREN:
        return ")";
    case TokenType::PLUS:
        return "+";
    case TokenType::MINUS:
        return "-";
    case TokenType::EQ:
        return "=";
    case TokenType::NEQ:
        return "!=";
    case TokenType::LT:
        return "<";
    case TokenType::LTE:
        return "<=";
    case TokenType::GT:
        return ">";
    case TokenType::GTE:
        return ">=";
    }
    return "unknown";
}

Lexer::Lexer(const std::string_view input)
    : input_(input)
{
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;
    while (!atEnd())
    {
        skipWhitespace();
        if (atEnd())
        {
            break;
        }

        const auto start = position();
        const char c = current();
        switch (c)
        {
        case '*':
            advance();
            tokens.push_back(Token{TokenType::STAR, "*", start});
            break;
        case ',':
            advance();
            tokens.push_back(Token{TokenType::COMMA, ",", start});
            break;
        case '.':
            advance();
            tokens.push_back(Token{TokenType::DOT, ".", start});
            break;
        case '(':
            advance();
            tokens.push_back(Token{TokenType::LPAREN, "(", start});
            break;
        case ')':
            advance();
            tokens.push_back(Token{TokenType::RPAREN, ")", start});
            break;
        case '+':
            advance();
            tokens.push_back(Token{TokenType::PLUS, "+", start});
            break;
        case '-':
            advance();
            tokens.push_back(Token{TokenType::MINUS, "-", start});
            break;
        case '=':
            advance();
            tokens.push_back(Token{TokenType::EQ, "=", start});
            break;
        case '!':
            if (peek() == '=')
            {
                advance();
                advance();
                tokens.push_back(Token{TokenType::NEQ, "!=", start});
                break;
            }
            throw ParseError("Unexpected character '!'", start);
        case '<':
            if (peek() == '=')
            {
                advance();
                advance();
                tokens.push_back(Token{TokenType::LTE, "<=", start});
                break;
            }
            advance();
            tokens.push_back(Token{TokenType::LT, "<", start});
            break;
        case '>':
            if (peek() == '=')
            {
                advance();
                advance();
                tokens.push_back(Token{TokenType::GTE, ">=", start});
                break;
            }
            advance();
            tokens.push_back(Token{TokenType::GT, ">", start});
            break;
        case '\'':
        case '"':
            tokens.push_back(readStringLiteral());
            break;
        default:
            if (isIdentifierStart(c))
            {
                tokens.push_back(readIdentifierOrKeyword());
            }
            else if (std::isdigit(static_cast<unsigned char>(c)) != 0)
            {
                tokens.push_back(readNumberOrDuration());
            }
            else
            {
                throw ParseError("Unexpected character '" + std::string(1, c) + "'", start);
            }
            break;
        }
    }

    tokens.push_back(Token{TokenType::END_OF_INPUT, "", position()});
    return tokens;
}

bool Lexer::atEnd() const
{
    return index_ >= input_.size();
}

char Lexer::current() const
{
    return atEnd() ? '\0' : input_[index_];
}

char Lexer::peek(const std::size_t lookahead) const
{
    const auto idx = index_ + lookahead;
    return idx >= input_.size() ? '\0' : input_[idx];
}

char Lexer::advance()
{
    if (atEnd())
    {
        return '\0';
    }
    const char c = input_[index_++];
    if (c == '\n')
    {
        ++line_;
        column_ = 1;
    }
    else
    {
        ++column_;
    }
    return c;
}

void Lexer::skipWhitespace()
{
    while (!atEnd())
    {
        const auto ch = static_cast<unsigned char>(current());
        if (std::isspace(ch) == 0)
        {
            return;
        }
        advance();
    }
}

TokenPosition Lexer::position() const
{
    return TokenPosition{index_, line_, column_};
}

Token Lexer::makeToken(const TokenType type, const std::size_t start_index, const std::size_t start_line, const std::size_t start_column) const
{
    const auto length = index_ - start_index;
    return Token{
        type,
        std::string{input_.substr(start_index, length)},
        TokenPosition{start_index, start_line, start_column}};
}

Token Lexer::readIdentifierOrKeyword()
{
    const auto start_index = index_;
    const auto start_line = line_;
    const auto start_column = column_;
    advance();
    while (!atEnd() && isIdentifierPart(current()))
    {
        advance();
    }

    auto token = makeToken(TokenType::IDENTIFIER, start_index, start_line, start_column);
    const auto keyword = kKeywords.find(toUpper(token.lexeme));
    if (keyword != kKeywords.end())
    {
        token.type = keyword->second;
    }
    return token;
}

Token Lexer::readNumberOrDuration()
{
    const auto start_index = index_;
    const auto start_line = line_;
    const auto start_column = column_;
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(current())) != 0)
    {
        advance();
    }

    TokenType type = TokenType::NUMBER_LITERAL;
    if (!atEnd())
    {
        const char unit = current();
        if (unit == 's' || unit == 'm' || unit == 'h' || unit == 'S' || unit == 'M' || unit == 'H')
        {
            advance();
            type = TokenType::DURATION_LITERAL;
        }
    }

    return makeToken(type, start_index, start_line, start_column);
}

Token Lexer::readStringLiteral()
{
    const auto start_index = index_;
    const auto start_line = line_;
    const auto start_column = column_;
    const char quote = advance();

    std::string value;
    while (!atEnd())
    {
        const char c = advance();
        if (c == quote)
        {
            return Token{
                TokenType::STRING_LITERAL,
                value,
                TokenPosition{start_index, start_line, start_column}};
        }
        if (c == '\\' && !atEnd())
        {
            const char escaped = advance();
            value.push_back(escaped);
            continue;
        }
        value.push_back(c);
    }

    throw ParseError("Unterminated string literal", TokenPosition{start_index, start_line, start_column});
}

} // namespace mldp_pvxs_driver::query
