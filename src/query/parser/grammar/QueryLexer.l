%option reentrant
%option noyywrap
%option noinput
%option nounput
%option nodefault
%option prefix="mldp_query_"
%option outfile="src/query/parser/generated/QueryFlexLexer.cpp"
%option header-file="include/query/parser/generated/QueryFlexLexer.h"

%{
#include <query/parser/Token.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::query {

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

static std::string toUpper(std::string value)
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

static std::string unescapeQuoted(const char* text, const std::size_t length)
{
    std::string out;
    if (length < 2)
    {
        return out;
    }

    out.reserve(length - 2);
    for (std::size_t index = 1; index + 1 < length; ++index)
    {
        char current = text[index];
        if (current == '\\' && index + 2 < length)
        {
            ++index;
            current = text[index];
        }
        out.push_back(current);
    }
    return out;
}

static void advancePosition(FlexLexerExtra* extra, const char* text, const std::size_t length)
{
    for (std::size_t index = 0; index < length; ++index)
    {
        const char current = text[index];
        ++extra->offset;
        if (current == '\n')
        {
            ++extra->line;
            extra->column = 1;
        }
        else
        {
            ++extra->column;
        }
    }
}

static void emitToken(
    FlexLexerExtra* extra,
    const TokenType type,
    std::string lexeme,
    const TokenPosition& position)
{
    extra->out_tokens->push_back(Token{
        .type = type,
        .lexeme = std::move(lexeme),
        .position = position});
}

static const std::unordered_map<std::string, TokenType> kKeywords = {
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

} // namespace mldp_pvxs_driver::query
%}

WHITESPACE   [ \t\r\n]+
IDENTIFIER   [A-Za-z_][A-Za-z0-9_]*
NUMBER       [0-9]+
DURATION     [0-9]+[sSmMhH]
SINGLE_QUOTE \'([^\\\n\']|\\.)*\'
DOUBLE_QUOTE \"([^\\\n\"]|\\.)*\"

%%

{WHITESPACE} {
    auto* extra = static_cast<mldp_pvxs_driver::query::FlexLexerExtra*>(yyget_extra(yyscanner));
    mldp_pvxs_driver::query::advancePosition(extra, yytext, yyleng);
}

"!="|"<="|">="|"="|"<"|">"|"*"|","|"."|"("|")"|"+"|"-" {
    auto* extra = static_cast<mldp_pvxs_driver::query::FlexLexerExtra*>(yyget_extra(yyscanner));
    const auto start = mldp_pvxs_driver::query::TokenPosition{extra->offset, extra->line, extra->column};
    mldp_pvxs_driver::query::advancePosition(extra, yytext, yyleng);
    const std::string text(yytext, yyleng);
    mldp_pvxs_driver::query::TokenType type = mldp_pvxs_driver::query::TokenType::END_OF_INPUT;
    if (text == "!=") type = mldp_pvxs_driver::query::TokenType::NEQ;
    else if (text == "<=") type = mldp_pvxs_driver::query::TokenType::LTE;
    else if (text == ">=") type = mldp_pvxs_driver::query::TokenType::GTE;
    else if (text == "=") type = mldp_pvxs_driver::query::TokenType::EQ;
    else if (text == "<") type = mldp_pvxs_driver::query::TokenType::LT;
    else if (text == ">") type = mldp_pvxs_driver::query::TokenType::GT;
    else if (text == "*") type = mldp_pvxs_driver::query::TokenType::STAR;
    else if (text == ",") type = mldp_pvxs_driver::query::TokenType::COMMA;
    else if (text == ".") type = mldp_pvxs_driver::query::TokenType::DOT;
    else if (text == "(") type = mldp_pvxs_driver::query::TokenType::LPAREN;
    else if (text == ")") type = mldp_pvxs_driver::query::TokenType::RPAREN;
    else if (text == "+") type = mldp_pvxs_driver::query::TokenType::PLUS;
    else if (text == "-") type = mldp_pvxs_driver::query::TokenType::MINUS;
    mldp_pvxs_driver::query::emitToken(extra, type, text, start);
}

{DURATION} {
    auto* extra = static_cast<mldp_pvxs_driver::query::FlexLexerExtra*>(yyget_extra(yyscanner));
    const auto start = mldp_pvxs_driver::query::TokenPosition{extra->offset, extra->line, extra->column};
    std::string text(yytext, yyleng);
    mldp_pvxs_driver::query::advancePosition(extra, yytext, yyleng);
    mldp_pvxs_driver::query::emitToken(extra, mldp_pvxs_driver::query::TokenType::DURATION_LITERAL, std::move(text), start);
}

{NUMBER} {
    auto* extra = static_cast<mldp_pvxs_driver::query::FlexLexerExtra*>(yyget_extra(yyscanner));
    const auto start = mldp_pvxs_driver::query::TokenPosition{extra->offset, extra->line, extra->column};
    std::string text(yytext, yyleng);
    mldp_pvxs_driver::query::advancePosition(extra, yytext, yyleng);
    mldp_pvxs_driver::query::emitToken(extra, mldp_pvxs_driver::query::TokenType::NUMBER_LITERAL, std::move(text), start);
}

{IDENTIFIER} {
    auto* extra = static_cast<mldp_pvxs_driver::query::FlexLexerExtra*>(yyget_extra(yyscanner));
    const auto start = mldp_pvxs_driver::query::TokenPosition{extra->offset, extra->line, extra->column};
    std::string text(yytext, yyleng);
    mldp_pvxs_driver::query::advancePosition(extra, yytext, yyleng);

    const auto keyword = mldp_pvxs_driver::query::kKeywords.find(mldp_pvxs_driver::query::toUpper(text));
    if (keyword != mldp_pvxs_driver::query::kKeywords.end())
    {
        mldp_pvxs_driver::query::emitToken(extra, keyword->second, text, start);
    }
    else
    {
        mldp_pvxs_driver::query::emitToken(extra, mldp_pvxs_driver::query::TokenType::IDENTIFIER, std::move(text), start);
    }
}

{SINGLE_QUOTE}|{DOUBLE_QUOTE} {
    auto* extra = static_cast<mldp_pvxs_driver::query::FlexLexerExtra*>(yyget_extra(yyscanner));
    const auto start = mldp_pvxs_driver::query::TokenPosition{extra->offset, extra->line, extra->column};
    std::string value = mldp_pvxs_driver::query::unescapeQuoted(yytext, yyleng);
    mldp_pvxs_driver::query::advancePosition(extra, yytext, yyleng);
    mldp_pvxs_driver::query::emitToken(extra, mldp_pvxs_driver::query::TokenType::STRING_LITERAL, std::move(value), start);
}

. {
    auto* extra = static_cast<mldp_pvxs_driver::query::FlexLexerExtra*>(yyget_extra(yyscanner));
    const auto start = mldp_pvxs_driver::query::TokenPosition{extra->offset, extra->line, extra->column};
    extra->failed = true;
    extra->error_position = start;
    extra->error_message = std::string("Unexpected character '") + yytext[0] + "'";
    mldp_pvxs_driver::query::advancePosition(extra, yytext, yyleng);
    return 0;
}

<<EOF>> {
    return 0;
}

%%
