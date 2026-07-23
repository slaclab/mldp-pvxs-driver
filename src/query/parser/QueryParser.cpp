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

#include <charconv>
#include <limits>
#include <sstream>

namespace mldp_pvxs_driver::query {

namespace {

int64_t parseInteger(const Token& token)
{
    int64_t value = 0;
    const auto* begin = token.lexeme.data();
    const auto* end = token.lexeme.data() + token.lexeme.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end)
    {
        throw ParseError("Invalid integer literal: " + token.lexeme, token.position);
    }
    return value;
}

uint64_t parseUnsignedInteger(const Token& token)
{
    uint64_t value = 0;
    const auto* begin = token.lexeme.data();
    const auto* end = token.lexeme.data() + token.lexeme.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end)
    {
        throw ParseError("Invalid unsigned integer literal: " + token.lexeme, token.position);
    }
    return value;
}

int64_t durationToSeconds(const Token& token)
{
    if (token.lexeme.size() < 2)
    {
        throw ParseError("Invalid duration literal: " + token.lexeme, token.position);
    }

    const char unit = token.lexeme.back();
    const auto number_token = Token{
        TokenType::NUMBER_LITERAL,
        token.lexeme.substr(0, token.lexeme.size() - 1),
        token.position};
    const int64_t base = parseInteger(number_token);
    if (base < 0)
    {
        throw ParseError("Duration literal must be non-negative: " + token.lexeme, token.position);
    }

    switch (unit)
    {
    case 's':
    case 'S':
        return base;
    case 'm':
    case 'M':
        return base * 60;
    case 'h':
    case 'H':
        return base * 60 * 60;
    default:
        throw ParseError("Unsupported duration unit in: " + token.lexeme, token.position);
    }
}

std::string joinPath(const std::vector<std::string>& path, const std::size_t from)
{
    std::ostringstream out;
    for (std::size_t i = from; i < path.size(); ++i)
    {
        if (i > from)
        {
            out << '.';
        }
        out << path[i];
    }
    return out.str();
}

ParseError expectedError(const Token& token, std::string_view expected)
{
    return ParseError(
        "Expected " + std::string(expected) + ", got '" + token.lexeme + "' (" + std::string(tokenTypeToString(token.type)) + ")",
        token.position);
}

} // namespace

QueryParser::QueryParser(std::vector<Token> tokens)
    : tokens_(std::move(tokens))
{
}

QueryStatement QueryParser::parse()
{
    auto statement = parseStatement();
    consume(TokenType::END_OF_INPUT, "end of input");
    return statement;
}

QueryStatement QueryParser::parse(const std::string_view sql)
{
    return QueryParser(Lexer(sql).tokenize()).parse();
}

const Token& QueryParser::current() const
{
    return tokens_.at(index_);
}

const Token& QueryParser::previous() const
{
    return tokens_.at(index_ - 1);
}

bool QueryParser::atEnd() const
{
    return current().type == TokenType::END_OF_INPUT;
}

bool QueryParser::match(const TokenType type)
{
    if (current().type != type)
    {
        return false;
    }
    ++index_;
    return true;
}

const Token& QueryParser::consume(const TokenType type, const std::string_view expected)
{
    if (!match(type))
    {
        throw expectedError(current(), expected);
    }
    return previous();
}

bool QueryParser::isStartOfValue(const TokenType type) const
{
    return type == TokenType::STRING_LITERAL
        || type == TokenType::NUMBER_LITERAL
        || type == TokenType::NOW;
}

bool QueryParser::canStartIdentifierPath(const TokenType type) const
{
    return type == TokenType::IDENTIFIER;
}

bool QueryParser::isAliasCandidate(const TokenType type) const
{
    return type == TokenType::IDENTIFIER;
}

QueryStatement QueryParser::parseStatement()
{
    if (match(TokenType::SHOW))
    {
        return parseShowTables();
    }
    if (match(TokenType::DESCRIBE))
    {
        return parseDescribe();
    }
    if (match(TokenType::EXPLAIN))
    {
        return parseExplain();
    }
    if (match(TokenType::SELECT))
    {
        return parseSelect();
    }
    throw expectedError(current(), "SELECT, SHOW, DESCRIBE, or EXPLAIN");
}

ShowTablesStatement QueryParser::parseShowTables()
{
    consume(TokenType::TABLES, "TABLES");
    return ShowTablesStatement{};
}

DescribeStatement QueryParser::parseDescribe()
{
    const auto path = parseIdentifierPath();
    return DescribeStatement{
        .table_name = joinPath(path, 0)};
}

ExplainStatement QueryParser::parseExplain()
{
    consume(TokenType::SELECT, "SELECT");
    return ExplainStatement{
        .query = parseSelect()};
}

SelectStatement QueryParser::parseSelect()
{
    SelectStatement statement;
    statement.columns = parseSelectList();
    statement.select_all = statement.columns.empty();

    consume(TokenType::FROM, "FROM");
    statement.from = parseTableRef();
    statement.joins = parseJoinClauses();

    if (match(TokenType::WHERE))
    {
        statement.predicates = parseWhereClause();
    }
    if (match(TokenType::LIMIT))
    {
        const auto& token = consume(TokenType::NUMBER_LITERAL, "limit number");
        statement.limit = parseUnsignedInteger(token);
    }
    if (match(TokenType::PAGE))
    {
        consume(TokenType::TOKEN, "TOKEN");
        const auto& token = consume(TokenType::STRING_LITERAL, "page token string");
        statement.page_token = token.lexeme;
    }

    return statement;
}

std::vector<QualifiedColumn> QueryParser::parseSelectList()
{
    if (match(TokenType::STAR))
    {
        return {};
    }

    std::vector<QualifiedColumn> columns;
    columns.push_back(parseColumnReference());
    while (match(TokenType::COMMA))
    {
        columns.push_back(parseColumnReference());
    }
    return columns;
}

TableRef QueryParser::parseTableRef()
{
    const auto table_path = parseIdentifierPath();
    TableRef table{
        .table_name = joinPath(table_path, 0),
        .alias = std::nullopt};

    if (match(TokenType::AS))
    {
        table.alias = consume(TokenType::IDENTIFIER, "table alias").lexeme;
    }
    else if (isAliasCandidate(current().type))
    {
        table.alias = consume(TokenType::IDENTIFIER, "table alias").lexeme;
    }

    return table;
}

std::vector<JoinClause> QueryParser::parseJoinClauses()
{
    std::vector<JoinClause> joins;
    while (current().type == TokenType::INNER || current().type == TokenType::LEFT || current().type == TokenType::JOIN)
    {
        joins.push_back(parseJoinClause());
    }
    return joins;
}

JoinClause QueryParser::parseJoinClause()
{
    JoinType join_type = JoinType::INNER;
    if (match(TokenType::INNER))
    {
        consume(TokenType::JOIN, "JOIN");
    }
    else if (match(TokenType::LEFT))
    {
        match(TokenType::OUTER);
        consume(TokenType::JOIN, "JOIN");
        join_type = JoinType::LEFT_OUTER;
    }
    else
    {
        consume(TokenType::JOIN, "JOIN");
    }

    JoinClause clause;
    clause.type = join_type;
    clause.table = parseTableRef();
    consume(TokenType::ON, "ON");
    clause.condition = parseJoinCondition();
    return clause;
}

JoinCondition QueryParser::parseJoinCondition()
{
    const auto left = parseColumnReference();
    consume(TokenType::EQ, "=");
    const auto right = parseColumnReference();
    return JoinCondition{left, right};
}

std::vector<WherePredicate> QueryParser::parseWhereClause()
{
    std::vector<WherePredicate> predicates;
    predicates.push_back(parsePredicate());
    while (match(TokenType::AND))
    {
        predicates.push_back(parsePredicate());
    }
    return predicates;
}

WherePredicate QueryParser::parsePredicate()
{
    const auto column = parseColumnReference();
    if (match(TokenType::IN))
    {
        consume(TokenType::LPAREN, "(");
        std::vector<LiteralValue> values;
        values.push_back(parseValue());
        while (match(TokenType::COMMA))
        {
            values.push_back(parseValue());
        }
        consume(TokenType::RPAREN, ")");
        return InPredicate{column, std::move(values)};
    }
    if (match(TokenType::BETWEEN))
    {
        const auto lower = parseValue();
        consume(TokenType::AND, "AND");
        const auto upper = parseValue();
        return RangePredicate{column, lower, upper};
    }
    if (match(TokenType::LIKE))
    {
        return OpPredicate{column, PredicateBinaryOp::LIKE, parseValue()};
    }
    if (match(TokenType::CONTAINS))
    {
        return OpPredicate{column, PredicateBinaryOp::CONTAINS, parseValue()};
    }
    if (match(TokenType::PREFIX))
    {
        return OpPredicate{column, PredicateBinaryOp::PREFIX, parseValue()};
    }
    if (match(TokenType::EQ))
    {
        return EqPredicate{column, parseValue()};
    }
    if (match(TokenType::NEQ))
    {
        return OpPredicate{column, PredicateBinaryOp::NEQ, parseValue()};
    }
    if (match(TokenType::LT))
    {
        return OpPredicate{column, PredicateBinaryOp::LT, parseValue()};
    }
    if (match(TokenType::LTE))
    {
        return OpPredicate{column, PredicateBinaryOp::LTE, parseValue()};
    }
    if (match(TokenType::GT))
    {
        return OpPredicate{column, PredicateBinaryOp::GT, parseValue()};
    }
    if (match(TokenType::GTE))
    {
        return OpPredicate{column, PredicateBinaryOp::GTE, parseValue()};
    }

    throw expectedError(current(), "predicate operator");
}

LiteralValue QueryParser::parseValue()
{
    if (match(TokenType::STRING_LITERAL))
    {
        return previous().lexeme;
    }
    if (match(TokenType::NUMBER_LITERAL))
    {
        return parseInteger(previous());
    }
    if (match(TokenType::NOW))
    {
        int64_t offset_seconds = 0;
        if (match(TokenType::PLUS))
        {
            const auto& duration = consume(TokenType::DURATION_LITERAL, "duration literal");
            offset_seconds = durationToSeconds(duration);
        }
        else if (match(TokenType::MINUS))
        {
            const auto& duration = consume(TokenType::DURATION_LITERAL, "duration literal");
            offset_seconds = -durationToSeconds(duration);
        }
        return NowLiteral{offset_seconds};
    }

    throw expectedError(current(), "value");
}

QualifiedColumn QueryParser::parseColumnReference()
{
    const auto path = parseIdentifierPath();
    QualifiedColumn column;
    column.path = path;
    if (path.size() == 1)
    {
        column.name = path.front();
        return column;
    }

    if (path.front() == "attr")
    {
        column.name = joinPath(path, 0);
        return column;
    }

    column.qualifier = path.front();
    column.name = joinPath(path, 1);
    return column;
}

std::vector<std::string> QueryParser::parseIdentifierPath()
{
    if (!canStartIdentifierPath(current().type))
    {
        throw expectedError(current(), "identifier");
    }

    std::vector<std::string> path;
    path.push_back(consume(TokenType::IDENTIFIER, "identifier").lexeme);
    while (match(TokenType::DOT))
    {
        path.push_back(consume(TokenType::IDENTIFIER, "identifier").lexeme);
    }
    return path;
}

QueryStatement parseQuery(const std::string_view sql)
{
    return QueryParser::parse(sql);
}

} // namespace mldp_pvxs_driver::query
