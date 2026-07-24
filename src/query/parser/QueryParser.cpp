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

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
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
    const auto trim = [](std::string_view value)
    {
        const auto begin = value.find_first_not_of(" \t\r\n");
        const auto end = value.find_last_not_of(" \t\r\n;");
        return begin == std::string_view::npos ? std::string_view{} : value.substr(begin, end - begin + 1);
    };
    const auto uppercase = [](std::string_view value)
    {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character)
                       { return static_cast<char>(std::toupper(character)); });
        return result;
    };
    const auto text = trim(sql);
    const auto starts = [&text](const std::string_view prefix)
    {
        if (text.size() < prefix.size()) return false;
        return std::equal(prefix.begin(), prefix.end(), text.begin(), [](const char left, const char right)
                          { return std::toupper(static_cast<unsigned char>(left)) == std::toupper(static_cast<unsigned char>(right)); });
    };
    const auto parseCreate = [&](const bool temporary) -> QueryStatement
    {
        const auto prefix = temporary ? std::string_view{"CREATE TEMP TABLE "} : std::string_view{"CREATE TABLE "};
        const auto rest = trim(text.substr(prefix.size()));
        std::string upper_rest(rest);
        std::transform(upper_rest.begin(), upper_rest.end(), upper_rest.begin(), [](const unsigned char character)
                       { return static_cast<char>(std::toupper(character)); });
        const auto as = upper_rest.find(" AS ");
        if (as == std::string_view::npos || as == 0) throw ParseError("CREATE TABLE requires name AS SELECT", TokenPosition{});
        const auto child = trim(rest.substr(as + 4));
        const auto parsed = QueryParser::parse(child);
        if (!std::holds_alternative<SelectStatement>(parsed)) throw ParseError("CREATE TABLE requires a SELECT query", TokenPosition{});
        return CreateTableStatement{.table_name = std::string(trim(rest.substr(0, as))), .temporary = temporary,
                                    .query = std::get<SelectStatement>(parsed)};
    };
    if (starts("CREATE TEMP TABLE ")) return parseCreate(true);
    if (starts("CREATE TABLE ")) return parseCreate(false);
    if (starts("DROP TABLE "))
    {
        const auto name = trim(text.substr(std::string_view{"DROP TABLE "}.size()));
        if (name.empty() || name.find_first_of(" \t\r\n") != std::string_view::npos) throw ParseError("DROP TABLE requires one table name", TokenPosition{});
        return DropTableStatement{.table_name = std::string(name)};
    }

    // The generated grammar deliberately keeps scalar subqueries out of
    // scope.  Recognize only parenthesized SELECTs in FROM/JOIN positions,
    // parse their bodies recursively, and replace them with private table
    // markers for the normal SELECT grammar.
    std::unordered_map<std::string, std::shared_ptr<SelectStatement>> derived;
    std::string rewritten(sql);
    std::size_t search = 0;
    unsigned int derived_number = 0;
    while ((search = rewritten.find('(', search)) != std::string::npos)
    {
        const auto before = trim(std::string_view(rewritten).substr(0, search));
        const auto word_start = before.find_last_of(" \t\r\n");
        const auto previous = uppercase(before.substr(word_start == std::string_view::npos ? 0 : word_start + 1));
        if (previous != "FROM" && previous != "JOIN")
        {
            ++search;
            continue;
        }
        std::size_t depth = 1;
        std::size_t end = search + 1;
        for (; end < rewritten.size() && depth != 0; ++end)
        {
            if (rewritten[end] == '(') ++depth;
            else if (rewritten[end] == ')') --depth;
        }
        if (depth != 0) throw ParseError("Unclosed derived table query", TokenPosition{});
        const auto child_sql = trim(std::string_view(rewritten).substr(search + 1, end - search - 2));
        const auto child = QueryParser::parse(child_sql);
        if (!std::holds_alternative<SelectStatement>(child)) throw ParseError("Derived table must contain SELECT", TokenPosition{});
        std::size_t alias_start = end;
        while (alias_start < rewritten.size() && std::isspace(static_cast<unsigned char>(rewritten[alias_start]))) ++alias_start;
        if (uppercase(std::string_view(rewritten).substr(alias_start, 3)) == "AS ")
        {
            alias_start += 3;
            while (alias_start < rewritten.size() && std::isspace(static_cast<unsigned char>(rewritten[alias_start]))) ++alias_start;
        }
        std::size_t alias_end = alias_start;
        while (alias_end < rewritten.size() && (std::isalnum(static_cast<unsigned char>(rewritten[alias_end])) || rewritten[alias_end] == '_')) ++alias_end;
        if (alias_end == alias_start) throw ParseError("Derived table requires an alias", TokenPosition{});
        const auto marker = "derivedsubquery" + std::to_string(derived_number++);
        derived.emplace(marker, std::make_shared<SelectStatement>(std::get<SelectStatement>(child)));
        rewritten.replace(search, end - search, marker);
        search = alias_end - (end - search) + marker.size();
    }
    if (!derived.empty())
    {
        auto result = QueryParser(Lexer(rewritten).tokenize()).parse();
        if (!std::holds_alternative<SelectStatement>(result)) return result;
        auto& select = std::get<SelectStatement>(result);
        const auto attach = [&derived](TableRef& ref)
        {
            if (const auto it = derived.find(ref.table_name); it != derived.end()) ref.derived_query = it->second;
        };
        attach(select.from);
        for (auto& join : select.joins) attach(join.table);
        return result;
    }
    return QueryParser(Lexer(sql).tokenize()).parse();
}

mldp_pvxs_driver::query::QueryStatement mldp_pvxs_driver::query::parseQuery(const std::string_view sql)
{
    return QueryParser::parse(sql);
}
