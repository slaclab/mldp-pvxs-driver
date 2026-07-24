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
#include <query/parser/QueryParser.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <vector>

using namespace mldp_pvxs_driver::query;

namespace {

TEST(QueryLexerTest, TokenizesKeywordsAndDurationLiteralsCaseInsensitive)
{
    const auto tokens = Lexer("select * from mldp.pv_stats where time >= now-60s").tokenize();
    ASSERT_GE(tokens.size(), 12);
    EXPECT_EQ(tokens[0].type, TokenType::SELECT);
    EXPECT_EQ(tokens[1].type, TokenType::STAR);
    EXPECT_EQ(tokens[2].type, TokenType::FROM);
    EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[4].type, TokenType::DOT);
    EXPECT_EQ(tokens[5].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[6].type, TokenType::WHERE);
    EXPECT_EQ(tokens[7].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[8].type, TokenType::GTE);
    EXPECT_EQ(tokens[9].type, TokenType::NOW);
    EXPECT_EQ(tokens[10].type, TokenType::MINUS);
    EXPECT_EQ(tokens[11].type, TokenType::DURATION_LITERAL);
    EXPECT_EQ(tokens[11].lexeme, "60s");
}

TEST(QueryLexerTest, TokenizesEveryPunctuationAndOperatorClass)
{
    const auto tokens = Lexer("* , . ( ) + - = != < <= > >= 123 45m 'single' \"double\"").tokenize();
    const std::vector<TokenType> expected = {
        TokenType::STAR, TokenType::COMMA, TokenType::DOT, TokenType::LPAREN, TokenType::RPAREN,
        TokenType::PLUS, TokenType::MINUS, TokenType::EQ, TokenType::NEQ, TokenType::LT,
        TokenType::LTE, TokenType::GT, TokenType::GTE, TokenType::NUMBER_LITERAL,
        TokenType::DURATION_LITERAL, TokenType::STRING_LITERAL, TokenType::STRING_LITERAL,
        TokenType::END_OF_INPUT};
    ASSERT_EQ(tokens.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        EXPECT_EQ(tokens[index].type, expected[index]) << index;
    }
    EXPECT_EQ(tokens[15].lexeme, "single");
    EXPECT_EQ(tokens[16].lexeme, "double");
}

TEST(QueryLexerTest, TokenizesEveryKeywordCaseInsensitively)
{
    const auto tokens = Lexer(
        "select from where and in like between limit page token show tables describe explain as inner left outer join on now prefix contains")
                            .tokenize();
    const std::vector<TokenType> expected = {
        TokenType::SELECT, TokenType::FROM, TokenType::WHERE, TokenType::AND, TokenType::IN,
        TokenType::LIKE, TokenType::BETWEEN, TokenType::LIMIT, TokenType::PAGE, TokenType::TOKEN,
        TokenType::SHOW, TokenType::TABLES, TokenType::DESCRIBE, TokenType::EXPLAIN, TokenType::AS,
        TokenType::INNER, TokenType::LEFT, TokenType::OUTER, TokenType::JOIN, TokenType::ON,
        TokenType::NOW, TokenType::PREFIX, TokenType::CONTAINS, TokenType::END_OF_INPUT};
    ASSERT_EQ(tokens.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        EXPECT_EQ(tokens[index].type, expected[index]) << index;
    }
}

TEST(QueryLexerTest, RejectsUnknownAndUnterminatedInputAtItsSourcePosition)
{
    for (const std::string_view sql : {"SELECT @", "SELECT 'unterminated"})
    {
        try
        {
            (void)Lexer(sql).tokenize();
            FAIL() << "Expected ParseError for " << sql;
        }
        catch (const ParseError& error)
        {
            EXPECT_EQ(error.line(), 1);
            EXPECT_GE(error.column(), 1);
        }
    }
}

TEST(QueryParserTest, ParsesSelectJoinPredicatesLimitAndPageToken)
{
    const auto statement = parseQuery(
        "SELECT ts.pv, meta.attributes.owner " "FROM mldp.pv_stats AS ts " "LEFT JOIN mldp.pv_metadata meta ON ts.pv = meta.pv " "WHERE ts.pv IN ('A', 'B') AND ts.time >= NOW-60s " "LIMIT 10 PAGE TOKEN 'next-1'");

    ASSERT_TRUE(std::holds_alternative<SelectStatement>(statement));
    const auto& select = std::get<SelectStatement>(statement);
    EXPECT_FALSE(select.select_all);
    ASSERT_EQ(select.columns.size(), 2);
    EXPECT_EQ(select.columns[0].qualifier.value_or(""), "ts");
    EXPECT_EQ(select.columns[0].name, "pv");
    EXPECT_EQ(select.columns[1].qualifier.value_or(""), "meta");
    EXPECT_EQ(select.columns[1].name, "attributes.owner");

    EXPECT_EQ(select.from.table_name, "mldp.pv_stats");
    EXPECT_EQ(select.from.alias.value_or(""), "ts");
    ASSERT_EQ(select.joins.size(), 1);
    EXPECT_EQ(select.joins[0].type, JoinType::LEFT_OUTER);
    EXPECT_EQ(select.joins[0].table.table_name, "mldp.pv_metadata");
    EXPECT_EQ(select.joins[0].table.alias.value_or(""), "meta");

    ASSERT_EQ(select.predicates.size(), 2);
    EXPECT_TRUE(std::holds_alternative<InPredicate>(select.predicates[0]));
    const auto& in = std::get<InPredicate>(select.predicates[0]);
    ASSERT_EQ(in.values.size(), 2);
    EXPECT_EQ(std::get<std::string>(in.values[0]), "A");
    EXPECT_EQ(std::get<std::string>(in.values[1]), "B");
    EXPECT_TRUE(std::holds_alternative<OpPredicate>(select.predicates[1]));
    const auto& range = std::get<OpPredicate>(select.predicates[1]);
    EXPECT_EQ(range.op, PredicateBinaryOp::GTE);
    ASSERT_TRUE(std::holds_alternative<NowLiteral>(range.value));
    EXPECT_EQ(std::get<NowLiteral>(range.value).offset_seconds, -60);

    ASSERT_TRUE(select.limit.has_value());
    EXPECT_EQ(*select.limit, 10);
    ASSERT_TRUE(select.page_token.has_value());
    EXPECT_EQ(*select.page_token, "next-1");
}

TEST(QueryParserTest, ParsesUnlimitedInPredicateLiteralsInOrder)
{
    const auto statement = parseQuery(
        "SELECT pv, time, value FROM mldp.time_series "
        "WHERE pv IN ('mldp_sample:MAGNET:01:VALUE', 'mldp_sample:RF:02:VALUE', 'mldp_sample:VACUUM:03:VALUE', 42, NOW-10h) "
        "AND time >= NOW-10h AND time <= NOW");

    const auto& select = std::get<SelectStatement>(statement);
    ASSERT_EQ(select.predicates.size(), 3);
    ASSERT_TRUE(std::holds_alternative<InPredicate>(select.predicates.front()));
    const auto& in = std::get<InPredicate>(select.predicates.front());
    ASSERT_EQ(in.values.size(), 5);
    EXPECT_EQ(std::get<std::string>(in.values[0]), "mldp_sample:MAGNET:01:VALUE");
    EXPECT_EQ(std::get<std::string>(in.values[1]), "mldp_sample:RF:02:VALUE");
    EXPECT_EQ(std::get<std::string>(in.values[2]), "mldp_sample:VACUUM:03:VALUE");
    EXPECT_EQ(std::get<int64_t>(in.values[3]), 42);
    ASSERT_TRUE(std::holds_alternative<NowLiteral>(in.values[4]));
    EXPECT_EQ(std::get<NowLiteral>(in.values[4]).offset_seconds, -36000);
}

TEST(QueryParserTest, RejectsEmptyAndMalformedInPredicateLists)
{
    for (const std::string_view sql : {
             "SELECT pv FROM mldp.time_series WHERE pv IN ()",
             "SELECT pv FROM mldp.time_series WHERE pv IN ('A',)",
             "SELECT pv FROM mldp.time_series WHERE pv IN ('A',, 'B')"})
    {
        EXPECT_THROW((void)parseQuery(sql), ParseError) << sql;
    }
}

TEST(QueryParserTest, PreservesLikeAsItsOwnPredicateOperator)
{
    const auto statement = parseQuery("SELECT * FROM mldp.configuration WHERE name LIKE 'beam%'");
    const auto& select = std::get<SelectStatement>(statement);
    ASSERT_EQ(select.predicates.size(), 1);
    ASSERT_TRUE(std::holds_alternative<OpPredicate>(select.predicates.front()));
    EXPECT_EQ(std::get<OpPredicate>(select.predicates.front()).op, PredicateBinaryOp::LIKE);
}

TEST(QueryParserTest, ParsesMultiKeyOrderBy)
{
    const auto statement = parseQuery("SELECT pv FROM mldp.pv_metadata ORDER BY attributes.device_group, attributes.ordinal DESC LIMIT 10");
    const auto& select = std::get<SelectStatement>(statement);
    ASSERT_EQ(select.order_by.size(), 2);
    EXPECT_EQ(select.order_by[0].column.name, "attributes.device_group");
    EXPECT_EQ(select.order_by[0].direction, SortDirection::ASCENDING);
    EXPECT_EQ(select.order_by[1].column.name, "attributes.ordinal");
    EXPECT_EQ(select.order_by[1].direction, SortDirection::DESCENDING);
    ASSERT_TRUE(select.limit.has_value());
    EXPECT_EQ(*select.limit, 10);
}

TEST(QueryParserTest, ParsesSpecialTimeSeriesTableSelectStarAndMetadataFilters)
{
    const auto statement = parseQuery(
        "SELECT * FROM mldp.time_series_table WHERE pv IN ('SYS:MAGNET:CURRENT', 'SYS:VACUUM:PRESSURE') "
        "AND column_type = 'double' AND attributes.namespace = 'mldp_sample' AND time >= NOW-1h");
    const auto& select = std::get<SelectStatement>(statement);
    EXPECT_TRUE(select.select_all);
    EXPECT_EQ(select.from.table_name, "mldp.time_series_table");
    ASSERT_EQ(select.predicates.size(), 4);
}

TEST(QueryParserTest, ParsesWideTablePvAndWindowSubqueries)
{
    const auto statement = parseQuery(
        "SELECT * FROM mldp.time_series_table "
        "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE tag = 'magnet') "
        "AND window IN (SELECT time, end_time FROM mldp.configuration_activation WHERE attributes.namespace = 'mldp_sample' "
        "AND end_time IS NOT NULL)");
    const auto& select = std::get<SelectStatement>(statement);
    ASSERT_EQ(select.predicates.size(), 2);
    const auto& pv = std::get<InPredicate>(select.predicates[0]);
    ASSERT_NE(pv.subquery, nullptr);
    EXPECT_EQ(pv.subquery->columns.front().name, "pv");
    const auto& window = std::get<InPredicate>(select.predicates[1]);
    ASSERT_NE(window.subquery, nullptr);
    ASSERT_EQ(window.subquery->predicates.size(), 2);
    EXPECT_TRUE(std::holds_alternative<IsNotNullPredicate>(window.subquery->predicates[1]));
}

TEST(QueryParserTest, ParsesShowTablesDescribeAndExplain)
{
    auto show_tables = parseQuery("SHOW TABLES");
    EXPECT_TRUE(std::holds_alternative<ShowTablesStatement>(show_tables));

    auto describe = parseQuery("DESCRIBE mldp.pv_metadata");
    ASSERT_TRUE(std::holds_alternative<DescribeStatement>(describe));
    EXPECT_EQ(std::get<DescribeStatement>(describe).table_name, "mldp.pv_metadata");

    auto explain = parseQuery("EXPLAIN SELECT * FROM mldp.pv_stats");
    ASSERT_TRUE(std::holds_alternative<ExplainStatement>(explain));
    EXPECT_TRUE(std::get<ExplainStatement>(explain).query.select_all);
}

TEST(QueryParserTest, ParsesCreateTempPersistentAndDropTableStatements)
{
    const auto temporary = parseQuery("CREATE TEMP TABLE recent_samples AS SELECT pv, value FROM mldp.time_series WHERE pv = 'A'");
    ASSERT_TRUE(std::holds_alternative<CreateTableStatement>(temporary));
    EXPECT_TRUE(std::get<CreateTableStatement>(temporary).temporary);
    EXPECT_EQ(std::get<CreateTableStatement>(temporary).table_name, "recent_samples");
    EXPECT_EQ(std::get<CreateTableStatement>(temporary).query.from.table_name, "mldp.time_series");

    const auto persistent = parseQuery("CREATE TABLE production_samples AS SELECT pv FROM mldp.time_series");
    ASSERT_TRUE(std::holds_alternative<CreateTableStatement>(persistent));
    EXPECT_FALSE(std::get<CreateTableStatement>(persistent).temporary);
    EXPECT_EQ(std::get<CreateTableStatement>(persistent).table_name, "production_samples");

    const auto drop = parseQuery("DROP TABLE production_samples");
    ASSERT_TRUE(std::holds_alternative<DropTableStatement>(drop));
    EXPECT_EQ(std::get<DropTableStatement>(drop).table_name, "production_samples");
}

TEST(QueryParserTest, ParsesDerivedTableSourcesAndRequiresAliases)
{
    const auto statement = parseQuery("SELECT recent.pv FROM (SELECT pv FROM mldp.time_series WHERE pv = 'A') AS recent");
    const auto& select = std::get<SelectStatement>(statement);
    ASSERT_NE(select.from.derived_query, nullptr);
    EXPECT_EQ(select.from.alias.value_or(""), "recent");
    EXPECT_EQ(select.from.derived_query->from.table_name, "mldp.time_series");

    const auto joined = parseQuery("SELECT r.pv FROM fake.meta m JOIN (SELECT pv FROM fake.samples) r ON m.pv = r.pv");
    const auto& join_select = std::get<SelectStatement>(joined);
    ASSERT_EQ(join_select.joins.size(), 1U);
    EXPECT_NE(join_select.joins.front().table.derived_query, nullptr);

    EXPECT_THROW((void)parseQuery("SELECT pv FROM (SELECT pv FROM mldp.time_series)"), ParseError);

    const auto in_subquery = parseQuery("SELECT pv FROM mldp.time_series WHERE pv IN (SELECT pv FROM mldp.time_series)");
    const auto& in_select = std::get<SelectStatement>(in_subquery);
    ASSERT_EQ(in_select.predicates.size(), 1U);
    const auto& in = std::get<InPredicate>(in_select.predicates.front());
    EXPECT_NE(in.subquery, nullptr);
    EXPECT_EQ(in.subquery->from.table_name, "mldp.time_series");
}

TEST(QueryParserTest, ParsesInnerLeftAndMultiJoinChains)
{
    const auto statement = parseQuery(
        "SELECT a.pv FROM fake.a a INNER JOIN fake.b b ON a.pv = b.pv LEFT OUTER JOIN fake.c c ON b.pv = c.pv");
    ASSERT_TRUE(std::holds_alternative<SelectStatement>(statement));
    const auto& joins = std::get<SelectStatement>(statement).joins;
    ASSERT_EQ(joins.size(), 2);
    EXPECT_EQ(joins[0].type, JoinType::INNER);
    EXPECT_EQ(joins[1].type, JoinType::LEFT_OUTER);
    EXPECT_EQ(joins[1].condition.left.qualifier.value_or(""), "b");
    EXPECT_EQ(joins[1].condition.right.qualifier.value_or(""), "c");
}

TEST(QueryParserTest, ReportsParseErrorsWithPosition)
{
    try
    {
        (void)parseQuery("SELECT FROM mldp.pv_stats");
        FAIL() << "Expected ParseError";
    }
    catch (const ParseError& error)
    {
        EXPECT_GE(error.line(), 1);
        EXPECT_GE(error.column(), 1);
        EXPECT_NE(std::string(error.what()).find("Expected"), std::string::npos);
    }
}

} // namespace
