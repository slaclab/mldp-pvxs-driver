%require "3.7"
%language "c++"
%defines

%define api.namespace {mldp_pvxs_driver::query::generated}
%define api.parser.class {QueryBisonParser}
%define api.value.type variant
%define api.token.constructor
%define parse.error detailed
%locations

%code requires {
    #include <query/parser/QueryAST.h>
    #include <query/parser/Token.h>
    #include <query/parser/generated/QueryBisonContext.h>

    #include <cstdint>
    #include <optional>
    #include <string>
    #include <vector>

    namespace mldp_pvxs_driver::query::generated {

    struct SelectListValue {
        bool                                select_all{false};
        std::vector<QualifiedColumn>        columns;
    };

    } // namespace mldp_pvxs_driver::query::generated
}

%code {
    #include <charconv>
    #include <sstream>
    #include <stdexcept>

    namespace mldp_pvxs_driver::query::generated {

    static std::string joinPath(const std::vector<std::string>& path, const std::size_t start)
    {
        std::ostringstream out;
        for (std::size_t index = start; index < path.size(); ++index)
        {
            if (index > start)
            {
                out << '.';
            }
            out << path[index];
        }
        return out.str();
    }

    static int64_t durationToSeconds(const std::string& duration, const location& loc)
    {
        if (duration.size() < 2)
        {
            throw ParseError(
                "Invalid duration literal: " + duration,
                TokenPosition{0, static_cast<std::size_t>(loc.begin.line), static_cast<std::size_t>(loc.begin.column)});
        }

        int64_t value = 0;
        const auto* begin = duration.data();
        const auto* end = duration.data() + duration.size() - 1;
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc() || ptr != end)
        {
            throw ParseError(
                "Invalid duration literal: " + duration,
                TokenPosition{0, static_cast<std::size_t>(loc.begin.line), static_cast<std::size_t>(loc.begin.column)});
        }

        const char unit = duration.back();
        if (unit == 's' || unit == 'S')
        {
            return value;
        }
        if (unit == 'm' || unit == 'M')
        {
            return value * 60;
        }
        if (unit == 'h' || unit == 'H')
        {
            return value * 60 * 60;
        }

        throw ParseError(
            "Unsupported duration unit in: " + duration,
            TokenPosition{0, static_cast<std::size_t>(loc.begin.line), static_cast<std::size_t>(loc.begin.column)});
    }

    static QualifiedColumn makeColumn(std::vector<std::string> path)
    {
        QualifiedColumn column;
        column.path = path;
        if (path.size() == 1)
        {
            column.name = path.front();
            return column;
        }

        if (path.front() == "attr" || path.front() == "attributes" || path.front() == "provenance")
        {
            column.name = joinPath(path, 0);
            return column;
        }

        column.qualifier = path.front();
        column.name = joinPath(path, 1);
        return column;
    }

    static QueryBisonParser::symbol_type yylex(ParseContext& ctx)
    {
        Token token{
            .type = TokenType::END_OF_INPUT,
            .lexeme = "",
            .position = TokenPosition{0, 1, 1}};

        if (ctx.index < ctx.tokens.size())
        {
            token = ctx.tokens[ctx.index++];
            ctx.last_position = token.position;
            ctx.has_last_position = true;
        }
        else if (ctx.has_last_position)
        {
            token.position = ctx.last_position;
        }

        QueryBisonParser::location_type location;
        location.begin.line = static_cast<int>(token.position.line);
        location.begin.column = static_cast<int>(token.position.column);
        location.end.line = static_cast<int>(token.position.line);
        location.end.column = static_cast<int>(token.position.column + (token.lexeme.empty() ? 0 : token.lexeme.size()));

        switch (token.type)
        {
        case TokenType::END_OF_INPUT: return QueryBisonParser::make_END_OF_INPUT(location);
        case TokenType::IDENTIFIER: return QueryBisonParser::make_IDENTIFIER(token.lexeme, location);
        case TokenType::STRING_LITERAL: return QueryBisonParser::make_STRING_LITERAL(token.lexeme, location);
        case TokenType::NUMBER_LITERAL:
        {
            int64_t value = 0;
            const auto [ptr, ec] = std::from_chars(token.lexeme.data(), token.lexeme.data() + token.lexeme.size(), value);
            if (ec != std::errc() || ptr != token.lexeme.data() + token.lexeme.size())
            {
                throw ParseError("Invalid integer literal: " + token.lexeme, token.position);
            }
            return QueryBisonParser::make_NUMBER_LITERAL(value, location);
        }
        case TokenType::DURATION_LITERAL: return QueryBisonParser::make_DURATION_LITERAL(token.lexeme, location);
        case TokenType::SELECT: return QueryBisonParser::make_SELECT(location);
        case TokenType::FROM: return QueryBisonParser::make_FROM(location);
        case TokenType::WHERE: return QueryBisonParser::make_WHERE(location);
        case TokenType::AND: return QueryBisonParser::make_AND(location);
        case TokenType::IN: return QueryBisonParser::make_IN(location);
        case TokenType::LIKE: return QueryBisonParser::make_LIKE(location);
        case TokenType::BETWEEN: return QueryBisonParser::make_BETWEEN(location);
        case TokenType::LIMIT: return QueryBisonParser::make_LIMIT(location);
        case TokenType::PAGE: return QueryBisonParser::make_PAGE(location);
        case TokenType::TOKEN: return QueryBisonParser::make_TOKEN(location);
        case TokenType::SHOW: return QueryBisonParser::make_SHOW(location);
        case TokenType::TABLES: return QueryBisonParser::make_TABLES(location);
        case TokenType::DESCRIBE: return QueryBisonParser::make_DESCRIBE(location);
        case TokenType::EXPLAIN: return QueryBisonParser::make_EXPLAIN(location);
        case TokenType::AS: return QueryBisonParser::make_AS(location);
        case TokenType::INNER: return QueryBisonParser::make_INNER(location);
        case TokenType::LEFT: return QueryBisonParser::make_LEFT(location);
        case TokenType::OUTER: return QueryBisonParser::make_OUTER(location);
        case TokenType::JOIN: return QueryBisonParser::make_JOIN(location);
        case TokenType::ON: return QueryBisonParser::make_ON(location);
        case TokenType::NOW: return QueryBisonParser::make_NOW(location);
        case TokenType::PREFIX: return QueryBisonParser::make_PREFIX(location);
        case TokenType::CONTAINS: return QueryBisonParser::make_CONTAINS(location);
        case TokenType::ORDER: return QueryBisonParser::make_ORDER(location);
        case TokenType::BY: return QueryBisonParser::make_BY(location);
        case TokenType::ASC: return QueryBisonParser::make_ASC(location);
        case TokenType::DESC: return QueryBisonParser::make_DESC(location);
        case TokenType::STAR: return QueryBisonParser::make_STAR(location);
        case TokenType::COMMA: return QueryBisonParser::make_COMMA(location);
        case TokenType::DOT: return QueryBisonParser::make_DOT(location);
        case TokenType::LPAREN: return QueryBisonParser::make_LPAREN(location);
        case TokenType::RPAREN: return QueryBisonParser::make_RPAREN(location);
        case TokenType::PLUS: return QueryBisonParser::make_PLUS(location);
        case TokenType::MINUS: return QueryBisonParser::make_MINUS(location);
        case TokenType::EQ: return QueryBisonParser::make_EQ(location);
        case TokenType::NEQ: return QueryBisonParser::make_NEQ(location);
        case TokenType::LT: return QueryBisonParser::make_LT(location);
        case TokenType::LTE: return QueryBisonParser::make_LTE(location);
        case TokenType::GT: return QueryBisonParser::make_GT(location);
        case TokenType::GTE: return QueryBisonParser::make_GTE(location);
        }
        return QueryBisonParser::make_END_OF_INPUT(location);
    }

    void QueryBisonParser::error(const QueryBisonParser::location_type& location, const std::string& message)
    {
        TokenPosition position{
            .offset = 0,
            .line = static_cast<std::size_t>(location.begin.line),
            .column = static_cast<std::size_t>(location.begin.column)};
        throw ParseError("Expected valid query syntax: " + message, position);
    }

    } // namespace mldp_pvxs_driver::query::generated
}

%parse-param { mldp_pvxs_driver::query::generated::ParseContext& ctx }
%lex-param { mldp_pvxs_driver::query::generated::ParseContext& ctx }

%token END_OF_INPUT 0
%token <std::string> IDENTIFIER STRING_LITERAL DURATION_LITERAL
%token <int64_t> NUMBER_LITERAL
%token SELECT FROM WHERE AND IN LIKE BETWEEN LIMIT PAGE TOKEN SHOW TABLES DESCRIBE EXPLAIN AS INNER LEFT OUTER JOIN ON NOW PREFIX CONTAINS ORDER BY ASC DESC
%token STAR COMMA DOT LPAREN RPAREN PLUS MINUS EQ NEQ LT LTE GT GTE

%type <mldp_pvxs_driver::query::QueryStatement> statement
%type <mldp_pvxs_driver::query::SelectStatement> select_stmt
%type <mldp_pvxs_driver::query::generated::SelectListValue> select_list
%type <std::vector<mldp_pvxs_driver::query::QualifiedColumn>> column_list
%type <mldp_pvxs_driver::query::QualifiedColumn> column_ref
%type <std::vector<std::string>> identifier_path
%type <mldp_pvxs_driver::query::TableRef> table_ref
%type <std::optional<std::string>> alias_opt
%type <std::vector<mldp_pvxs_driver::query::JoinClause>> join_clauses
%type <mldp_pvxs_driver::query::JoinClause> join_clause
%type <std::vector<mldp_pvxs_driver::query::WherePredicate>> where_opt predicate_list
%type <mldp_pvxs_driver::query::WherePredicate> predicate
%type <mldp_pvxs_driver::query::LiteralValue> literal now_literal
%type <int64_t> signed_duration
%type <std::optional<uint64_t>> limit_opt
%type <std::optional<std::string>> page_opt
%type <std::vector<mldp_pvxs_driver::query::OrderByItem>> order_by_opt order_by_list
%type <mldp_pvxs_driver::query::OrderByItem> order_by_item

%%

input
    : statement END_OF_INPUT
      {
          ctx.result = std::move($1);
      }
    ;

statement
    : select_stmt
      { $$ = std::move($1); }
    | SHOW TABLES
      { $$ = mldp_pvxs_driver::query::ShowTablesStatement{}; }
    | DESCRIBE identifier_path
      {
          $$ = mldp_pvxs_driver::query::DescribeStatement{ .table_name = joinPath($2, 0) };
      }
    | EXPLAIN select_stmt
      {
          $$ = mldp_pvxs_driver::query::ExplainStatement{ .query = std::move($2) };
      }
    ;

select_stmt
    : SELECT select_list FROM table_ref join_clauses where_opt order_by_opt limit_opt page_opt
      {
          mldp_pvxs_driver::query::SelectStatement statement;
          statement.select_all = $2.select_all;
          statement.columns = std::move($2.columns);
          statement.from = std::move($4);
          statement.joins = std::move($5);
          statement.predicates = std::move($6);
          statement.order_by = std::move($7);
          statement.limit = std::move($8);
          statement.page_token = std::move($9);
          $$ = std::move(statement);
      }
    ;

order_by_opt
    : /* empty */
      { $$ = {}; }
    | ORDER BY order_by_list
      { $$ = std::move($3); }
    ;

order_by_list
    : order_by_item
      { $$ = std::vector<mldp_pvxs_driver::query::OrderByItem>{std::move($1)}; }
    | order_by_list COMMA order_by_item
      { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

order_by_item
    : column_ref
      { $$ = mldp_pvxs_driver::query::OrderByItem{.column = std::move($1)}; }
    | column_ref ASC
      { $$ = mldp_pvxs_driver::query::OrderByItem{.column = std::move($1), .direction = mldp_pvxs_driver::query::SortDirection::ASCENDING}; }
    | column_ref DESC
      { $$ = mldp_pvxs_driver::query::OrderByItem{.column = std::move($1), .direction = mldp_pvxs_driver::query::SortDirection::DESCENDING}; }
    ;

select_list
    : STAR
      {
          $$ = mldp_pvxs_driver::query::generated::SelectListValue{
              .select_all = true,
              .columns = {}
          };
      }
    | column_list
      {
          $$ = mldp_pvxs_driver::query::generated::SelectListValue{
              .select_all = false,
              .columns = std::move($1)
          };
      }
    ;

column_list
    : column_ref
      {
          $$ = std::vector<mldp_pvxs_driver::query::QualifiedColumn>{std::move($1)};
      }
    | column_list COMMA column_ref
      {
          $1.push_back(std::move($3));
          $$ = std::move($1);
      }
    ;

table_ref
    : identifier_path alias_opt
      {
          $$ = mldp_pvxs_driver::query::TableRef{
              .table_name = joinPath($1, 0),
              .alias = std::move($2)
          };
      }
    ;

alias_opt
    : /* empty */
      { $$ = std::nullopt; }
    | AS IDENTIFIER
      { $$ = std::optional<std::string>{$2}; }
    | IDENTIFIER
      { $$ = std::optional<std::string>{$1}; }
    ;

join_clauses
    : /* empty */
      { $$ = {}; }
    | join_clauses join_clause
      {
          $1.push_back(std::move($2));
          $$ = std::move($1);
      }
    ;

join_clause
    : JOIN table_ref ON column_ref EQ column_ref
      {
          $$ = mldp_pvxs_driver::query::JoinClause{
              .type = mldp_pvxs_driver::query::JoinType::INNER,
              .table = std::move($2),
              .condition = mldp_pvxs_driver::query::JoinCondition{
                  .left = std::move($4),
                  .right = std::move($6)
              }
          };
      }
    | INNER JOIN table_ref ON column_ref EQ column_ref
      {
          $$ = mldp_pvxs_driver::query::JoinClause{
              .type = mldp_pvxs_driver::query::JoinType::INNER,
              .table = std::move($3),
              .condition = mldp_pvxs_driver::query::JoinCondition{
                  .left = std::move($5),
                  .right = std::move($7)
              }
          };
      }
    | LEFT JOIN table_ref ON column_ref EQ column_ref
      {
          $$ = mldp_pvxs_driver::query::JoinClause{
              .type = mldp_pvxs_driver::query::JoinType::LEFT_OUTER,
              .table = std::move($3),
              .condition = mldp_pvxs_driver::query::JoinCondition{
                  .left = std::move($5),
                  .right = std::move($7)
              }
          };
      }
    | LEFT OUTER JOIN table_ref ON column_ref EQ column_ref
      {
          $$ = mldp_pvxs_driver::query::JoinClause{
              .type = mldp_pvxs_driver::query::JoinType::LEFT_OUTER,
              .table = std::move($4),
              .condition = mldp_pvxs_driver::query::JoinCondition{
                  .left = std::move($6),
                  .right = std::move($8)
              }
          };
      }
    ;

where_opt
    : /* empty */
      { $$ = {}; }
    | WHERE predicate_list
      { $$ = std::move($2); }
    ;

predicate_list
    : predicate
      { $$ = std::vector<mldp_pvxs_driver::query::WherePredicate>{std::move($1)}; }
    | predicate_list AND predicate
      {
          $1.push_back(std::move($3));
          $$ = std::move($1);
      }
    ;

predicate
    : column_ref IN LPAREN literal RPAREN
      {
          $$ = mldp_pvxs_driver::query::InPredicate{
              .column = std::move($1),
              .values = std::vector<mldp_pvxs_driver::query::LiteralValue>{std::move($4)}
          };
      }
    | column_ref IN LPAREN literal COMMA literal RPAREN
      {
          $$ = mldp_pvxs_driver::query::InPredicate{
              .column = std::move($1),
              .values = std::vector<mldp_pvxs_driver::query::LiteralValue>{std::move($4), std::move($6)}
          };
      }
    | column_ref BETWEEN literal AND literal
      {
          $$ = mldp_pvxs_driver::query::RangePredicate{
              .column = std::move($1),
              .lower = std::move($3),
              .upper = std::move($5)
          };
      }
    | column_ref LIKE literal
      {
          $$ = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move($1),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LIKE,
              .value = std::move($3)
          };
      }
    | column_ref CONTAINS literal
      {
          $$ = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move($1),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::CONTAINS,
              .value = std::move($3)
          };
      }
    | column_ref PREFIX literal
      {
          $$ = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move($1),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::PREFIX,
              .value = std::move($3)
          };
      }
    | column_ref EQ literal
      {
          $$ = mldp_pvxs_driver::query::EqPredicate{
              .column = std::move($1),
              .value = std::move($3)
          };
      }
    | column_ref NEQ literal
      {
          $$ = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move($1),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::NEQ,
              .value = std::move($3)
          };
      }
    | column_ref LT literal
      {
          $$ = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move($1),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LT,
              .value = std::move($3)
          };
      }
    | column_ref LTE literal
      {
          $$ = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move($1),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LTE,
              .value = std::move($3)
          };
      }
    | column_ref GT literal
      {
          $$ = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move($1),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::GT,
              .value = std::move($3)
          };
      }
    | column_ref GTE literal
      {
          $$ = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move($1),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::GTE,
              .value = std::move($3)
          };
      }
    ;

literal
    : STRING_LITERAL
      { $$ = mldp_pvxs_driver::query::LiteralValue{$1}; }
    | NUMBER_LITERAL
      { $$ = mldp_pvxs_driver::query::LiteralValue{$1}; }
    | now_literal
      { $$ = std::move($1); }
    ;

now_literal
    : NOW
      { $$ = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::NowLiteral{0}}; }
    | NOW signed_duration
      { $$ = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::NowLiteral{$2}}; }
    ;

signed_duration
    : PLUS DURATION_LITERAL
      { $$ = durationToSeconds($2, @2); }
    | MINUS DURATION_LITERAL
      { $$ = -durationToSeconds($2, @2); }
    ;

column_ref
    : identifier_path
      {
          $$ = makeColumn(std::move($1));
      }
    ;

identifier_path
    : IDENTIFIER
      { $$ = std::vector<std::string>{$1}; }
    | identifier_path DOT IDENTIFIER
      {
          $1.push_back($3);
          $$ = std::move($1);
      }
    ;

limit_opt
    : /* empty */
      { $$ = std::nullopt; }
    | LIMIT NUMBER_LITERAL
      {
          if ($2 < 0)
          {
              throw ParseError("LIMIT must be non-negative", TokenPosition{0, static_cast<std::size_t>(@2.begin.line), static_cast<std::size_t>(@2.begin.column)});
          }
          $$ = std::optional<uint64_t>{static_cast<uint64_t>($2)};
      }
    ;

page_opt
    : /* empty */
      { $$ = std::nullopt; }
    | PAGE TOKEN STRING_LITERAL
      { $$ = std::optional<std::string>{$3}; }
    ;

%%
