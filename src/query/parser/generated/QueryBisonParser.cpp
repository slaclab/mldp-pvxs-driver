// A Bison parser, made by GNU Bison 3.7.4.

// Skeleton implementation for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2020 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.





#include "QueryBisonParser.hpp"


// Unqualified %code blocks.
#line 33 "src/query/parser/grammar/QueryBisonParser.y"

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
        if (unit == 'd' || unit == 'D')
        {
            return value * 24 * 60 * 60;
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

    static ExpressionPtr makeExpression(ExpressionValue value)
    {
        return std::make_shared<Expression>(Expression{.value = std::move(value)});
    }

    static LiteralValue legacyLiteral(const ExpressionPtr& expression)
    {
        if (expression && std::holds_alternative<LiteralValue>(expression->value))
            return std::get<LiteralValue>(expression->value);
        return std::string{};
    }

    static std::vector<LiteralValue> legacyLiterals(const std::vector<ExpressionPtr>& expressions)
    {
        std::vector<LiteralValue> values;
        values.reserve(expressions.size());
        for (const auto& expression : expressions)
            values.push_back(legacyLiteral(expression));
        return values;
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
        case TokenType::TRUE: return QueryBisonParser::make_TRUE(location);
        case TokenType::FALSE: return QueryBisonParser::make_FALSE(location);
        case TokenType::TIMESTAMP_NS: return QueryBisonParser::make_TIMESTAMP_NS(location);
        case TokenType::DURATION_NS: return QueryBisonParser::make_DURATION_NS(location);
        case TokenType::SELECT: return QueryBisonParser::make_SELECT(location);
        case TokenType::FROM: return QueryBisonParser::make_FROM(location);
        case TokenType::WHERE: return QueryBisonParser::make_WHERE(location);
        case TokenType::IS: return QueryBisonParser::make_IS(location);
        case TokenType::AND: return QueryBisonParser::make_AND(location);
        case TokenType::OR: return QueryBisonParser::make_OR(location);
        case TokenType::NOT: return QueryBisonParser::make_NOT(location);
        case TokenType::NULL_LITERAL: return QueryBisonParser::make_NULL_LITERAL(location);
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
        case TokenType::SLASH: return QueryBisonParser::make_SLASH(location);
        case TokenType::COMMA: return QueryBisonParser::make_COMMA(location);
        case TokenType::SEMICOLON: return QueryBisonParser::make_SEMICOLON(location);
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

#line 258 "src/query/parser/generated/QueryBisonParser.cpp"


#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> // FIXME: INFRINGES ON USER NAME SPACE.
#   define YY_(msgid) dgettext ("bison-runtime", msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(msgid) msgid
# endif
#endif


// Whether we are compiled with exception support.
#ifndef YY_EXCEPTIONS
# if defined __GNUC__ && !defined __EXCEPTIONS
#  define YY_EXCEPTIONS 0
# else
#  define YY_EXCEPTIONS 1
# endif
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K].location)
/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

# ifndef YYLLOC_DEFAULT
#  define YYLLOC_DEFAULT(Current, Rhs, N)                               \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).begin  = YYRHSLOC (Rhs, 1).begin;                   \
          (Current).end    = YYRHSLOC (Rhs, N).end;                     \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).begin = (Current).end = YYRHSLOC (Rhs, 0).end;      \
        }                                                               \
    while (false)
# endif


// Enable debugging if requested.
#if YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
# define YYCDEBUG if (yydebug_) (*yycdebug_)

# define YY_SYMBOL_PRINT(Title, Symbol)         \
  do {                                          \
    if (yydebug_)                               \
    {                                           \
      *yycdebug_ << Title << ' ';               \
      yy_print_ (*yycdebug_, Symbol);           \
      *yycdebug_ << '\n';                       \
    }                                           \
  } while (false)

# define YY_REDUCE_PRINT(Rule)          \
  do {                                  \
    if (yydebug_)                       \
      yy_reduce_print_ (Rule);          \
  } while (false)

# define YY_STACK_PRINT()               \
  do {                                  \
    if (yydebug_)                       \
      yy_stack_print_ ();                \
  } while (false)

#else // !YYDEBUG

# define YYCDEBUG if (false) std::cerr
# define YY_SYMBOL_PRINT(Title, Symbol)  YYUSE (Symbol)
# define YY_REDUCE_PRINT(Rule)           static_cast<void> (0)
# define YY_STACK_PRINT()                static_cast<void> (0)

#endif // !YYDEBUG

#define yyerrok         (yyerrstatus_ = 0)
#define yyclearin       (yyla.clear ())

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYRECOVERING()  (!!yyerrstatus_)

#line 5 "src/query/parser/grammar/QueryBisonParser.y"
namespace mldp_pvxs_driver { namespace query { namespace generated {
#line 351 "src/query/parser/generated/QueryBisonParser.cpp"

  /// Build a parser object.
  QueryBisonParser::QueryBisonParser (mldp_pvxs_driver::query::generated::ParseContext& ctx_yyarg)
#if YYDEBUG
    : yydebug_ (false),
      yycdebug_ (&std::cerr),
#else
    :
#endif
      ctx (ctx_yyarg)
  {}

  QueryBisonParser::~QueryBisonParser ()
  {}

  QueryBisonParser::syntax_error::~syntax_error () YY_NOEXCEPT YY_NOTHROW
  {}

  /*---------------.
  | symbol kinds.  |
  `---------------*/



  // by_state.
  QueryBisonParser::by_state::by_state () YY_NOEXCEPT
    : state (empty_state)
  {}

  QueryBisonParser::by_state::by_state (const by_state& that) YY_NOEXCEPT
    : state (that.state)
  {}

  void
  QueryBisonParser::by_state::clear () YY_NOEXCEPT
  {
    state = empty_state;
  }

  void
  QueryBisonParser::by_state::move (by_state& that)
  {
    state = that.state;
    that.clear ();
  }

  QueryBisonParser::by_state::by_state (state_type s) YY_NOEXCEPT
    : state (s)
  {}

  QueryBisonParser::symbol_kind_type
  QueryBisonParser::by_state::kind () const YY_NOEXCEPT
  {
    if (state == empty_state)
      return symbol_kind::S_YYEMPTY;
    else
      return YY_CAST (symbol_kind_type, yystos_[+state]);
  }

  QueryBisonParser::stack_symbol_type::stack_symbol_type ()
  {}

  QueryBisonParser::stack_symbol_type::stack_symbol_type (YY_RVREF (stack_symbol_type) that)
    : super_type (YY_MOVE (that.state), YY_MOVE (that.location))
  {
    switch (that.kind ())
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_integer: // signed_integer
      case symbol_kind::S_signed_duration: // signed_duration
        value.YY_MOVE_OR_COPY< int64_t > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_expression: // expression
      case symbol_kind::S_legacy_expression: // legacy_expression
      case symbol_kind::S_or_expression: // or_expression
      case symbol_kind::S_and_expression: // and_expression
      case symbol_kind::S_comparison_expression: // comparison_expression
      case symbol_kind::S_additive_expression: // additive_expression
      case symbol_kind::S_multiplicative_expression: // multiplicative_expression
      case symbol_kind::S_unary_expression: // unary_expression
      case symbol_kind::S_primary_expression: // primary_expression
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::ExpressionPtr > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_window_option: // window_option
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::InPredicate::WindowShardOption > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_join_clause: // join_clause
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::JoinClause > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::LiteralValue > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::OrderByItem > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_column_ref: // column_ref
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::QualifiedColumn > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_statement: // statement
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::QueryStatement > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_item: // select_item
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::SelectItem > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::SelectStatement > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_table_ref: // table_ref
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::TableRef > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_predicate: // predicate
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::WherePredicate > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_list: // select_list
        value.YY_MOVE_OR_COPY< mldp_pvxs_driver::query::generated::SelectListValue > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        value.YY_MOVE_OR_COPY< std::optional<std::string> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        value.YY_MOVE_OR_COPY< std::optional<uint64_t> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        value.YY_MOVE_OR_COPY< std::string > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_expression_list: // expression_list
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::ExpressionPtr> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_window_option_list: // window_option_list
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::JoinClause> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::OrderByItem> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_item_list: // select_item_list
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::SelectItem> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::WherePredicate> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        value.YY_MOVE_OR_COPY< std::vector<std::string> > (YY_MOVE (that.value));
        break;

      default:
        break;
    }

#if 201103L <= YY_CPLUSPLUS
    // that is emptied.
    that.state = empty_state;
#endif
  }

  QueryBisonParser::stack_symbol_type::stack_symbol_type (state_type s, YY_MOVE_REF (symbol_type) that)
    : super_type (s, YY_MOVE (that.location))
  {
    switch (that.kind ())
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_integer: // signed_integer
      case symbol_kind::S_signed_duration: // signed_duration
        value.move< int64_t > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_expression: // expression
      case symbol_kind::S_legacy_expression: // legacy_expression
      case symbol_kind::S_or_expression: // or_expression
      case symbol_kind::S_and_expression: // and_expression
      case symbol_kind::S_comparison_expression: // comparison_expression
      case symbol_kind::S_additive_expression: // additive_expression
      case symbol_kind::S_multiplicative_expression: // multiplicative_expression
      case symbol_kind::S_unary_expression: // unary_expression
      case symbol_kind::S_primary_expression: // primary_expression
        value.move< mldp_pvxs_driver::query::ExpressionPtr > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_window_option: // window_option
        value.move< mldp_pvxs_driver::query::InPredicate::WindowShardOption > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_join_clause: // join_clause
        value.move< mldp_pvxs_driver::query::JoinClause > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        value.move< mldp_pvxs_driver::query::LiteralValue > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        value.move< mldp_pvxs_driver::query::OrderByItem > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_column_ref: // column_ref
        value.move< mldp_pvxs_driver::query::QualifiedColumn > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_statement: // statement
        value.move< mldp_pvxs_driver::query::QueryStatement > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_item: // select_item
        value.move< mldp_pvxs_driver::query::SelectItem > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        value.move< mldp_pvxs_driver::query::SelectStatement > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_table_ref: // table_ref
        value.move< mldp_pvxs_driver::query::TableRef > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_predicate: // predicate
        value.move< mldp_pvxs_driver::query::WherePredicate > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_list: // select_list
        value.move< mldp_pvxs_driver::query::generated::SelectListValue > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        value.move< std::optional<std::string> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        value.move< std::optional<uint64_t> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        value.move< std::string > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_expression_list: // expression_list
        value.move< std::vector<mldp_pvxs_driver::query::ExpressionPtr> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_window_option_list: // window_option_list
        value.move< std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        value.move< std::vector<mldp_pvxs_driver::query::JoinClause> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.move< std::vector<mldp_pvxs_driver::query::OrderByItem> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_item_list: // select_item_list
        value.move< std::vector<mldp_pvxs_driver::query::SelectItem> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        value.move< std::vector<mldp_pvxs_driver::query::WherePredicate> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        value.move< std::vector<std::string> > (YY_MOVE (that.value));
        break;

      default:
        break;
    }

    // that is emptied.
    that.kind_ = symbol_kind::S_YYEMPTY;
  }

#if YY_CPLUSPLUS < 201103L
  QueryBisonParser::stack_symbol_type&
  QueryBisonParser::stack_symbol_type::operator= (const stack_symbol_type& that)
  {
    state = that.state;
    switch (that.kind ())
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_integer: // signed_integer
      case symbol_kind::S_signed_duration: // signed_duration
        value.copy< int64_t > (that.value);
        break;

      case symbol_kind::S_expression: // expression
      case symbol_kind::S_legacy_expression: // legacy_expression
      case symbol_kind::S_or_expression: // or_expression
      case symbol_kind::S_and_expression: // and_expression
      case symbol_kind::S_comparison_expression: // comparison_expression
      case symbol_kind::S_additive_expression: // additive_expression
      case symbol_kind::S_multiplicative_expression: // multiplicative_expression
      case symbol_kind::S_unary_expression: // unary_expression
      case symbol_kind::S_primary_expression: // primary_expression
        value.copy< mldp_pvxs_driver::query::ExpressionPtr > (that.value);
        break;

      case symbol_kind::S_window_option: // window_option
        value.copy< mldp_pvxs_driver::query::InPredicate::WindowShardOption > (that.value);
        break;

      case symbol_kind::S_join_clause: // join_clause
        value.copy< mldp_pvxs_driver::query::JoinClause > (that.value);
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        value.copy< mldp_pvxs_driver::query::LiteralValue > (that.value);
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        value.copy< mldp_pvxs_driver::query::OrderByItem > (that.value);
        break;

      case symbol_kind::S_column_ref: // column_ref
        value.copy< mldp_pvxs_driver::query::QualifiedColumn > (that.value);
        break;

      case symbol_kind::S_statement: // statement
        value.copy< mldp_pvxs_driver::query::QueryStatement > (that.value);
        break;

      case symbol_kind::S_select_item: // select_item
        value.copy< mldp_pvxs_driver::query::SelectItem > (that.value);
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        value.copy< mldp_pvxs_driver::query::SelectStatement > (that.value);
        break;

      case symbol_kind::S_table_ref: // table_ref
        value.copy< mldp_pvxs_driver::query::TableRef > (that.value);
        break;

      case symbol_kind::S_predicate: // predicate
        value.copy< mldp_pvxs_driver::query::WherePredicate > (that.value);
        break;

      case symbol_kind::S_select_list: // select_list
        value.copy< mldp_pvxs_driver::query::generated::SelectListValue > (that.value);
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        value.copy< std::optional<std::string> > (that.value);
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        value.copy< std::optional<uint64_t> > (that.value);
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        value.copy< std::string > (that.value);
        break;

      case symbol_kind::S_expression_list: // expression_list
        value.copy< std::vector<mldp_pvxs_driver::query::ExpressionPtr> > (that.value);
        break;

      case symbol_kind::S_window_option_list: // window_option_list
        value.copy< std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > (that.value);
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        value.copy< std::vector<mldp_pvxs_driver::query::JoinClause> > (that.value);
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.copy< std::vector<mldp_pvxs_driver::query::OrderByItem> > (that.value);
        break;

      case symbol_kind::S_select_item_list: // select_item_list
        value.copy< std::vector<mldp_pvxs_driver::query::SelectItem> > (that.value);
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        value.copy< std::vector<mldp_pvxs_driver::query::WherePredicate> > (that.value);
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        value.copy< std::vector<std::string> > (that.value);
        break;

      default:
        break;
    }

    location = that.location;
    return *this;
  }

  QueryBisonParser::stack_symbol_type&
  QueryBisonParser::stack_symbol_type::operator= (stack_symbol_type& that)
  {
    state = that.state;
    switch (that.kind ())
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_integer: // signed_integer
      case symbol_kind::S_signed_duration: // signed_duration
        value.move< int64_t > (that.value);
        break;

      case symbol_kind::S_expression: // expression
      case symbol_kind::S_legacy_expression: // legacy_expression
      case symbol_kind::S_or_expression: // or_expression
      case symbol_kind::S_and_expression: // and_expression
      case symbol_kind::S_comparison_expression: // comparison_expression
      case symbol_kind::S_additive_expression: // additive_expression
      case symbol_kind::S_multiplicative_expression: // multiplicative_expression
      case symbol_kind::S_unary_expression: // unary_expression
      case symbol_kind::S_primary_expression: // primary_expression
        value.move< mldp_pvxs_driver::query::ExpressionPtr > (that.value);
        break;

      case symbol_kind::S_window_option: // window_option
        value.move< mldp_pvxs_driver::query::InPredicate::WindowShardOption > (that.value);
        break;

      case symbol_kind::S_join_clause: // join_clause
        value.move< mldp_pvxs_driver::query::JoinClause > (that.value);
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        value.move< mldp_pvxs_driver::query::LiteralValue > (that.value);
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        value.move< mldp_pvxs_driver::query::OrderByItem > (that.value);
        break;

      case symbol_kind::S_column_ref: // column_ref
        value.move< mldp_pvxs_driver::query::QualifiedColumn > (that.value);
        break;

      case symbol_kind::S_statement: // statement
        value.move< mldp_pvxs_driver::query::QueryStatement > (that.value);
        break;

      case symbol_kind::S_select_item: // select_item
        value.move< mldp_pvxs_driver::query::SelectItem > (that.value);
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        value.move< mldp_pvxs_driver::query::SelectStatement > (that.value);
        break;

      case symbol_kind::S_table_ref: // table_ref
        value.move< mldp_pvxs_driver::query::TableRef > (that.value);
        break;

      case symbol_kind::S_predicate: // predicate
        value.move< mldp_pvxs_driver::query::WherePredicate > (that.value);
        break;

      case symbol_kind::S_select_list: // select_list
        value.move< mldp_pvxs_driver::query::generated::SelectListValue > (that.value);
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        value.move< std::optional<std::string> > (that.value);
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        value.move< std::optional<uint64_t> > (that.value);
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        value.move< std::string > (that.value);
        break;

      case symbol_kind::S_expression_list: // expression_list
        value.move< std::vector<mldp_pvxs_driver::query::ExpressionPtr> > (that.value);
        break;

      case symbol_kind::S_window_option_list: // window_option_list
        value.move< std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > (that.value);
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        value.move< std::vector<mldp_pvxs_driver::query::JoinClause> > (that.value);
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.move< std::vector<mldp_pvxs_driver::query::OrderByItem> > (that.value);
        break;

      case symbol_kind::S_select_item_list: // select_item_list
        value.move< std::vector<mldp_pvxs_driver::query::SelectItem> > (that.value);
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        value.move< std::vector<mldp_pvxs_driver::query::WherePredicate> > (that.value);
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        value.move< std::vector<std::string> > (that.value);
        break;

      default:
        break;
    }

    location = that.location;
    // that is emptied.
    that.state = empty_state;
    return *this;
  }
#endif

  template <typename Base>
  void
  QueryBisonParser::yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const
  {
    if (yymsg)
      YY_SYMBOL_PRINT (yymsg, yysym);
  }

#if YYDEBUG
  template <typename Base>
  void
  QueryBisonParser::yy_print_ (std::ostream& yyo, const basic_symbol<Base>& yysym) const
  {
    std::ostream& yyoutput = yyo;
    YYUSE (yyoutput);
    if (yysym.empty ())
      yyo << "empty symbol";
    else
      {
        symbol_kind_type yykind = yysym.kind ();
        yyo << (yykind < YYNTOKENS ? "token" : "nterm")
            << ' ' << yysym.name () << " ("
            << yysym.location << ": ";
        YYUSE (yykind);
        yyo << ')';
      }
  }
#endif

  void
  QueryBisonParser::yypush_ (const char* m, YY_MOVE_REF (stack_symbol_type) sym)
  {
    if (m)
      YY_SYMBOL_PRINT (m, sym);
    yystack_.push (YY_MOVE (sym));
  }

  void
  QueryBisonParser::yypush_ (const char* m, state_type s, YY_MOVE_REF (symbol_type) sym)
  {
#if 201103L <= YY_CPLUSPLUS
    yypush_ (m, stack_symbol_type (s, std::move (sym)));
#else
    stack_symbol_type ss (s, sym);
    yypush_ (m, ss);
#endif
  }

  void
  QueryBisonParser::yypop_ (int n)
  {
    yystack_.pop (n);
  }

#if YYDEBUG
  std::ostream&
  QueryBisonParser::debug_stream () const
  {
    return *yycdebug_;
  }

  void
  QueryBisonParser::set_debug_stream (std::ostream& o)
  {
    yycdebug_ = &o;
  }


  QueryBisonParser::debug_level_type
  QueryBisonParser::debug_level () const
  {
    return yydebug_;
  }

  void
  QueryBisonParser::set_debug_level (debug_level_type l)
  {
    yydebug_ = l;
  }
#endif // YYDEBUG

  QueryBisonParser::state_type
  QueryBisonParser::yy_lr_goto_state_ (state_type yystate, int yysym)
  {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
      return yytable_[yyr];
    else
      return yydefgoto_[yysym - YYNTOKENS];
  }

  bool
  QueryBisonParser::yy_pact_value_is_default_ (int yyvalue)
  {
    return yyvalue == yypact_ninf_;
  }

  bool
  QueryBisonParser::yy_table_value_is_error_ (int yyvalue)
  {
    return yyvalue == yytable_ninf_;
  }

  int
  QueryBisonParser::operator() ()
  {
    return parse ();
  }

  int
  QueryBisonParser::parse ()
  {
    int yyn;
    /// Length of the RHS of the rule being reduced.
    int yylen = 0;

    // Error handling.
    int yynerrs_ = 0;
    int yyerrstatus_ = 0;

    /// The lookahead symbol.
    symbol_type yyla;

    /// The locations where the error started and ended.
    stack_symbol_type yyerror_range[3];

    /// The return value of parse ().
    int yyresult;

#if YY_EXCEPTIONS
    try
#endif // YY_EXCEPTIONS
      {
    YYCDEBUG << "Starting parse\n";


    /* Initialize the stack.  The initial state will be set in
       yynewstate, since the latter expects the semantical and the
       location values to have been already stored, initialize these
       stacks with a primary value.  */
    yystack_.clear ();
    yypush_ (YY_NULLPTR, 0, YY_MOVE (yyla));

  /*-----------------------------------------------.
  | yynewstate -- push a new symbol on the stack.  |
  `-----------------------------------------------*/
  yynewstate:
    YYCDEBUG << "Entering state " << int (yystack_[0].state) << '\n';
    YY_STACK_PRINT ();

    // Accept?
    if (yystack_[0].state == yyfinal_)
      YYACCEPT;

    goto yybackup;


  /*-----------.
  | yybackup.  |
  `-----------*/
  yybackup:
    // Try to take a decision without lookahead.
    yyn = yypact_[+yystack_[0].state];
    if (yy_pact_value_is_default_ (yyn))
      goto yydefault;

    // Read a lookahead token.
    if (yyla.empty ())
      {
        YYCDEBUG << "Reading a token\n";
#if YY_EXCEPTIONS
        try
#endif // YY_EXCEPTIONS
          {
            symbol_type yylookahead (yylex (ctx));
            yyla.move (yylookahead);
          }
#if YY_EXCEPTIONS
        catch (const syntax_error& yyexc)
          {
            YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
            error (yyexc);
            goto yyerrlab1;
          }
#endif // YY_EXCEPTIONS
      }
    YY_SYMBOL_PRINT ("Next token is", yyla);

    if (yyla.kind () == symbol_kind::S_YYerror)
    {
      // The scanner already issued an error message, process directly
      // to error recovery.  But do not keep the error token as
      // lookahead, it is too special and may lead us to an endless
      // loop in error recovery. */
      yyla.kind_ = symbol_kind::S_YYUNDEF;
      goto yyerrlab1;
    }

    /* If the proper action on seeing token YYLA.TYPE is to reduce or
       to detect an error, take that action.  */
    yyn += yyla.kind ();
    if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.kind ())
      {
        goto yydefault;
      }

    // Reduce or error.
    yyn = yytable_[yyn];
    if (yyn <= 0)
      {
        if (yy_table_value_is_error_ (yyn))
          goto yyerrlab;
        yyn = -yyn;
        goto yyreduce;
      }

    // Count tokens shifted since error; after three, turn off error status.
    if (yyerrstatus_)
      --yyerrstatus_;

    // Shift the lookahead token.
    yypush_ ("Shifting", state_type (yyn), YY_MOVE (yyla));
    goto yynewstate;


  /*-----------------------------------------------------------.
  | yydefault -- do the default action for the current state.  |
  `-----------------------------------------------------------*/
  yydefault:
    yyn = yydefact_[+yystack_[0].state];
    if (yyn == 0)
      goto yyerrlab;
    goto yyreduce;


  /*-----------------------------.
  | yyreduce -- do a reduction.  |
  `-----------------------------*/
  yyreduce:
    yylen = yyr2_[yyn];
    {
      stack_symbol_type yylhs;
      yylhs.state = yy_lr_goto_state_ (yystack_[yylen].state, yyr1_[yyn]);
      /* Variants are always initialized to an empty instance of the
         correct type. The default '$$ = $1' action is NOT applied
         when using variants.  */
      switch (yyr1_[yyn])
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_integer: // signed_integer
      case symbol_kind::S_signed_duration: // signed_duration
        yylhs.value.emplace< int64_t > ();
        break;

      case symbol_kind::S_expression: // expression
      case symbol_kind::S_legacy_expression: // legacy_expression
      case symbol_kind::S_or_expression: // or_expression
      case symbol_kind::S_and_expression: // and_expression
      case symbol_kind::S_comparison_expression: // comparison_expression
      case symbol_kind::S_additive_expression: // additive_expression
      case symbol_kind::S_multiplicative_expression: // multiplicative_expression
      case symbol_kind::S_unary_expression: // unary_expression
      case symbol_kind::S_primary_expression: // primary_expression
        yylhs.value.emplace< mldp_pvxs_driver::query::ExpressionPtr > ();
        break;

      case symbol_kind::S_window_option: // window_option
        yylhs.value.emplace< mldp_pvxs_driver::query::InPredicate::WindowShardOption > ();
        break;

      case symbol_kind::S_join_clause: // join_clause
        yylhs.value.emplace< mldp_pvxs_driver::query::JoinClause > ();
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        yylhs.value.emplace< mldp_pvxs_driver::query::LiteralValue > ();
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        yylhs.value.emplace< mldp_pvxs_driver::query::OrderByItem > ();
        break;

      case symbol_kind::S_column_ref: // column_ref
        yylhs.value.emplace< mldp_pvxs_driver::query::QualifiedColumn > ();
        break;

      case symbol_kind::S_statement: // statement
        yylhs.value.emplace< mldp_pvxs_driver::query::QueryStatement > ();
        break;

      case symbol_kind::S_select_item: // select_item
        yylhs.value.emplace< mldp_pvxs_driver::query::SelectItem > ();
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        yylhs.value.emplace< mldp_pvxs_driver::query::SelectStatement > ();
        break;

      case symbol_kind::S_table_ref: // table_ref
        yylhs.value.emplace< mldp_pvxs_driver::query::TableRef > ();
        break;

      case symbol_kind::S_predicate: // predicate
        yylhs.value.emplace< mldp_pvxs_driver::query::WherePredicate > ();
        break;

      case symbol_kind::S_select_list: // select_list
        yylhs.value.emplace< mldp_pvxs_driver::query::generated::SelectListValue > ();
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        yylhs.value.emplace< std::optional<std::string> > ();
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        yylhs.value.emplace< std::optional<uint64_t> > ();
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        yylhs.value.emplace< std::string > ();
        break;

      case symbol_kind::S_expression_list: // expression_list
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::ExpressionPtr> > ();
        break;

      case symbol_kind::S_window_option_list: // window_option_list
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > ();
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::JoinClause> > ();
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::OrderByItem> > ();
        break;

      case symbol_kind::S_select_item_list: // select_item_list
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::SelectItem> > ();
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::WherePredicate> > ();
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        yylhs.value.emplace< std::vector<std::string> > ();
        break;

      default:
        break;
    }


      // Default location.
      {
        stack_type::slice range (yystack_, yylen);
        YYLLOC_DEFAULT (yylhs.location, range, yylen);
        yyerror_range[1].location = yylhs.location;
      }

      // Perform the reduction.
      YY_REDUCE_PRINT (yyn);
#if YY_EXCEPTIONS
      try
#endif // YY_EXCEPTIONS
        {
          switch (yyn)
            {
  case 2: // input: statement END_OF_INPUT
#line 289 "src/query/parser/grammar/QueryBisonParser.y"
      {
          ctx.result = std::move(yystack_[1].value.as < mldp_pvxs_driver::query::QueryStatement > ());
      }
#line 1287 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 3: // statement: select_stmt
#line 296 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::SelectStatement > ()); }
#line 1293 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 4: // statement: SHOW TABLES
#line 298 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::ShowTablesStatement{}; }
#line 1299 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 5: // statement: SHOW FUNCTIONS
#line 300 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::ShowFunctionsStatement{}; }
#line 1305 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 6: // statement: SHOW OPERATORS
#line 302 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::ShowOperatorsStatement{}; }
#line 1311 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 7: // statement: DESCRIBE identifier_path
#line 304 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::DescribeStatement{ .table_name = joinPath(yystack_[0].value.as < std::vector<std::string> > (), 0) };
      }
#line 1319 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 8: // statement: EXPLAIN select_stmt
#line 308 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::ExplainStatement{ .query = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::SelectStatement > ()) };
      }
#line 1327 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 9: // select_stmt: SELECT select_list FROM table_ref join_clauses where_opt order_by_opt limit_opt page_opt
#line 315 "src/query/parser/grammar/QueryBisonParser.y"
      {
          mldp_pvxs_driver::query::SelectStatement statement;
          statement.select_all = yystack_[7].value.as < mldp_pvxs_driver::query::generated::SelectListValue > ().select_all;
          statement.select_items = std::move(yystack_[7].value.as < mldp_pvxs_driver::query::generated::SelectListValue > ().items);
          for (const auto& item : statement.select_items)
          {
              if (item.expression && std::holds_alternative<QualifiedColumn>(item.expression->value))
                  statement.columns.push_back(std::get<QualifiedColumn>(item.expression->value));
          }
          statement.from = std::move(yystack_[5].value.as < mldp_pvxs_driver::query::TableRef > ());
          statement.joins = std::move(yystack_[4].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ());
          statement.predicates = std::move(yystack_[3].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ());
          statement.order_by = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > ());
          statement.limit = std::move(yystack_[1].value.as < std::optional<uint64_t> > ());
          statement.page_token = std::move(yystack_[0].value.as < std::optional<std::string> > ());
          yylhs.value.as < mldp_pvxs_driver::query::SelectStatement > () = std::move(statement);
      }
#line 1349 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 10: // order_by_opt: %empty
#line 336 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > () = {}; }
#line 1355 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 11: // order_by_opt: ORDER BY order_by_list
#line 338 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > () = std::move(yystack_[0].value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > ()); }
#line 1361 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 12: // order_by_list: order_by_item
#line 343 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > () = std::vector<mldp_pvxs_driver::query::OrderByItem>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::OrderByItem > ())}; }
#line 1367 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 13: // order_by_list: order_by_list COMMA order_by_item
#line 345 "src/query/parser/grammar/QueryBisonParser.y"
      { yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::OrderByItem > ())); yylhs.value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > ()); }
#line 1373 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 14: // order_by_item: expression
#line 350 "src/query/parser/grammar/QueryBisonParser.y"
      {
          mldp_pvxs_driver::query::OrderByItem item{.expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())};
          if (std::holds_alternative<QualifiedColumn>(item.expression->value)) item.column = std::get<QualifiedColumn>(item.expression->value);
          yylhs.value.as < mldp_pvxs_driver::query::OrderByItem > () = std::move(item);
      }
#line 1383 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 15: // order_by_item: expression ASC
#line 356 "src/query/parser/grammar/QueryBisonParser.y"
      {
          mldp_pvxs_driver::query::OrderByItem item{.expression = std::move(yystack_[1].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), .direction = mldp_pvxs_driver::query::SortDirection::ASCENDING};
          if (std::holds_alternative<QualifiedColumn>(item.expression->value)) item.column = std::get<QualifiedColumn>(item.expression->value);
          yylhs.value.as < mldp_pvxs_driver::query::OrderByItem > () = std::move(item);
      }
#line 1393 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 16: // order_by_item: expression DESC
#line 362 "src/query/parser/grammar/QueryBisonParser.y"
      {
          mldp_pvxs_driver::query::OrderByItem item{.expression = std::move(yystack_[1].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), .direction = mldp_pvxs_driver::query::SortDirection::DESCENDING};
          if (std::holds_alternative<QualifiedColumn>(item.expression->value)) item.column = std::get<QualifiedColumn>(item.expression->value);
          yylhs.value.as < mldp_pvxs_driver::query::OrderByItem > () = std::move(item);
      }
#line 1403 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 17: // select_list: STAR
#line 371 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::generated::SelectListValue > () = mldp_pvxs_driver::query::generated::SelectListValue{
              .select_all = true,
              .columns = {}, .items = {}
          };
      }
#line 1414 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 18: // select_list: select_item_list
#line 378 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::generated::SelectListValue > () = mldp_pvxs_driver::query::generated::SelectListValue{.select_all = false, .columns = {}, .items = std::move(yystack_[0].value.as < std::vector<mldp_pvxs_driver::query::SelectItem> > ())}; }
#line 1420 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 19: // select_item_list: select_item
#line 383 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::SelectItem> > () = std::vector<mldp_pvxs_driver::query::SelectItem>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::SelectItem > ())}; }
#line 1426 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 20: // select_item_list: select_item_list COMMA select_item
#line 385 "src/query/parser/grammar/QueryBisonParser.y"
      { yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::SelectItem> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::SelectItem > ())); yylhs.value.as < std::vector<mldp_pvxs_driver::query::SelectItem> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::SelectItem> > ()); }
#line 1432 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 21: // select_item: expression
#line 390 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::SelectItem > () = mldp_pvxs_driver::query::SelectItem{.expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}; }
#line 1438 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 22: // select_item: expression AS IDENTIFIER
#line 392 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::SelectItem > () = mldp_pvxs_driver::query::SelectItem{.expression = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), .alias = yystack_[0].value.as < std::string > ()}; }
#line 1444 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 23: // table_ref: identifier_path alias_opt
#line 397 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::TableRef > () = mldp_pvxs_driver::query::TableRef{
              .table_name = joinPath(yystack_[1].value.as < std::vector<std::string> > (), 0),
              .alias = std::move(yystack_[0].value.as < std::optional<std::string> > ())
          };
      }
#line 1455 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 24: // table_ref: LPAREN select_stmt RPAREN alias_opt
#line 404 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::TableRef > () = mldp_pvxs_driver::query::TableRef{
              .table_name = "<derived>", .alias = std::move(yystack_[0].value.as < std::optional<std::string> > ()),
              .derived_query = std::make_shared<mldp_pvxs_driver::query::SelectStatement>(std::move(yystack_[2].value.as < mldp_pvxs_driver::query::SelectStatement > ()))
          };
      }
#line 1466 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 25: // alias_opt: %empty
#line 414 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::nullopt; }
#line 1472 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 26: // alias_opt: AS IDENTIFIER
#line 416 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1478 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 27: // alias_opt: IDENTIFIER
#line 418 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1484 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 28: // join_clauses: %empty
#line 423 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > () = {}; }
#line 1490 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 29: // join_clauses: join_clauses join_clause
#line 425 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::JoinClause > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > () = std::move(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ());
      }
#line 1499 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 30: // join_clause: JOIN table_ref ON column_ref EQ column_ref
#line 433 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::JoinClause > () = mldp_pvxs_driver::query::JoinClause{
              .type = mldp_pvxs_driver::query::JoinType::INNER,
              .table = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::TableRef > ()),
              .condition = mldp_pvxs_driver::query::JoinCondition{
                  .left = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
                  .right = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())
              }
          };
      }
#line 1514 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 31: // join_clause: INNER JOIN table_ref ON column_ref EQ column_ref
#line 444 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::JoinClause > () = mldp_pvxs_driver::query::JoinClause{
              .type = mldp_pvxs_driver::query::JoinType::INNER,
              .table = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::TableRef > ()),
              .condition = mldp_pvxs_driver::query::JoinCondition{
                  .left = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
                  .right = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())
              }
          };
      }
#line 1529 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 32: // join_clause: LEFT JOIN table_ref ON column_ref EQ column_ref
#line 455 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::JoinClause > () = mldp_pvxs_driver::query::JoinClause{
              .type = mldp_pvxs_driver::query::JoinType::LEFT_OUTER,
              .table = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::TableRef > ()),
              .condition = mldp_pvxs_driver::query::JoinCondition{
                  .left = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
                  .right = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())
              }
          };
      }
#line 1544 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 33: // join_clause: LEFT OUTER JOIN table_ref ON column_ref EQ column_ref
#line 466 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::JoinClause > () = mldp_pvxs_driver::query::JoinClause{
              .type = mldp_pvxs_driver::query::JoinType::LEFT_OUTER,
              .table = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::TableRef > ()),
              .condition = mldp_pvxs_driver::query::JoinCondition{
                  .left = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
                  .right = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())
              }
          };
      }
#line 1559 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 34: // where_opt: %empty
#line 480 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = {}; }
#line 1565 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 35: // where_opt: WHERE predicate_list
#line 482 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::move(yystack_[0].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ()); }
#line 1571 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 36: // predicate_list: predicate
#line 487 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::vector<mldp_pvxs_driver::query::WherePredicate>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::WherePredicate > ())}; }
#line 1577 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 37: // predicate_list: predicate_list AND predicate
#line 489 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::WherePredicate > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ());
      }
#line 1586 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 38: // predicate: column_ref IN LPAREN expression_list RPAREN
#line 497 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::InPredicate{
              .column = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .values = legacyLiterals(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > ()),
              .expressions = std::move(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > ())
          };
      }
#line 1598 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 39: // predicate: column_ref IN LPAREN select_stmt RPAREN
#line 505 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::InPredicate{
              .column = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .subquery = std::make_shared<mldp_pvxs_driver::query::SelectStatement>(std::move(yystack_[1].value.as < mldp_pvxs_driver::query::SelectStatement > ()))
          };
      }
#line 1609 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 40: // predicate: column_ref IN LPAREN expression_list SEMICOLON window_option_list RPAREN
#line 512 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::InPredicate{
              .column = std::move(yystack_[6].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .values = legacyLiterals(yystack_[3].value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > ()),
              .expressions = std::move(yystack_[3].value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > ()),
              .window_options = std::move(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > ())
          };
      }
#line 1622 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 41: // predicate: column_ref IN LPAREN select_stmt SEMICOLON window_option_list RPAREN
#line 521 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::InPredicate{
              .column = std::move(yystack_[6].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .subquery = std::make_shared<mldp_pvxs_driver::query::SelectStatement>(std::move(yystack_[3].value.as < mldp_pvxs_driver::query::SelectStatement > ())),
              .window_options = std::move(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > ())
          };
      }
#line 1634 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 42: // predicate: column_ref IS NOT NULL_LITERAL
#line 529 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::IsNotNullPredicate{.column = std::move(yystack_[3].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())};
      }
#line 1642 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 43: // predicate: column_ref BETWEEN legacy_expression AND legacy_expression
#line 533 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::RangePredicate{
              .column = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .lower = legacyLiteral(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), .upper = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .lower_expression = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .upper_expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1655 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 44: // predicate: column_ref LIKE legacy_expression
#line 542 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LIKE,
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1668 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 45: // predicate: column_ref CONTAINS legacy_expression
#line 551 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::CONTAINS,
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1681 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 46: // predicate: column_ref PREFIX legacy_expression
#line 560 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::PREFIX,
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1694 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 47: // predicate: column_ref EQ legacy_expression
#line 569 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::EqPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1706 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 48: // predicate: column_ref NEQ legacy_expression
#line 577 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::NEQ,
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1719 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 49: // predicate: column_ref LT legacy_expression
#line 586 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LT,
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1732 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 50: // predicate: column_ref LTE legacy_expression
#line 595 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LTE,
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1745 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 51: // predicate: column_ref GT legacy_expression
#line 604 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::GT,
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1758 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 52: // predicate: column_ref GTE legacy_expression
#line 613 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::GTE,
              .value = legacyLiteral(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()),
              .expression = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())
          };
      }
#line 1771 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 53: // window_option_list: window_option
#line 625 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > () = std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::InPredicate::WindowShardOption > ())}; }
#line 1777 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 54: // window_option_list: window_option_list COMMA window_option
#line 627 "src/query/parser/grammar/QueryBisonParser.y"
      { yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::InPredicate::WindowShardOption > ())); yylhs.value.as < std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::InPredicate::WindowShardOption> > ()); }
#line 1783 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 55: // window_option: IDENTIFIER DURATION_LITERAL
#line 632 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::InPredicate::WindowShardOption > () = mldp_pvxs_driver::query::InPredicate::WindowShardOption{.name = std::move(yystack_[1].value.as < std::string > ()), .value = mldp_pvxs_driver::query::DurationNsLiteral{durationToSeconds(yystack_[0].value.as < std::string > (), yylhs.location) * 1000000000LL}}; }
#line 1789 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 56: // window_option: IDENTIFIER NUMBER_LITERAL
#line 634 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::InPredicate::WindowShardOption > () = mldp_pvxs_driver::query::InPredicate::WindowShardOption{.name = std::move(yystack_[1].value.as < std::string > ()), .value = yystack_[0].value.as < int64_t > ()}; }
#line 1795 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 57: // expression_list: expression
#line 639 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > () = std::vector<mldp_pvxs_driver::query::ExpressionPtr>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())};
      }
#line 1803 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 58: // expression_list: expression_list COMMA expression
#line 643 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > ());
      }
#line 1812 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 59: // expression: or_expression
#line 651 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1818 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 60: // legacy_expression: primary_expression
#line 656 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1824 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 61: // or_expression: and_expression
#line 661 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1830 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 62: // or_expression: or_expression OR and_expression
#line 663 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"OR", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1836 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 63: // and_expression: comparison_expression
#line 668 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1842 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 64: // and_expression: and_expression AND comparison_expression
#line 670 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"AND", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1848 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 65: // comparison_expression: additive_expression
#line 675 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1854 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 66: // comparison_expression: additive_expression EQ additive_expression
#line 677 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"=", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1860 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 67: // comparison_expression: additive_expression NEQ additive_expression
#line 679 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"!=", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1866 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 68: // comparison_expression: additive_expression LT additive_expression
#line 681 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"<", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1872 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 69: // comparison_expression: additive_expression LTE additive_expression
#line 683 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"<=", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1878 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 70: // comparison_expression: additive_expression GT additive_expression
#line 685 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{">", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1884 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 71: // comparison_expression: additive_expression GTE additive_expression
#line 687 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{">=", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1890 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 72: // additive_expression: multiplicative_expression
#line 692 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1896 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 73: // additive_expression: additive_expression PLUS multiplicative_expression
#line 694 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"+", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1902 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 74: // additive_expression: additive_expression MINUS multiplicative_expression
#line 696 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"-", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1908 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 75: // multiplicative_expression: unary_expression
#line 701 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1914 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 76: // multiplicative_expression: multiplicative_expression STAR unary_expression
#line 703 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"*", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1920 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 77: // multiplicative_expression: multiplicative_expression SLASH unary_expression
#line 705 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{BinaryExpression{"/", std::move(yystack_[2].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()), std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1926 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 78: // unary_expression: primary_expression
#line 710 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1932 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 79: // unary_expression: PLUS unary_expression
#line 712 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{UnaryExpression{"+", std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1938 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 80: // unary_expression: MINUS unary_expression
#line 714 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{UnaryExpression{"-", std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1944 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 81: // unary_expression: NOT unary_expression
#line 716 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{UnaryExpression{"NOT", std::move(yystack_[0].value.as < mldp_pvxs_driver::query::ExpressionPtr > ())}}); }
#line 1950 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 82: // primary_expression: literal
#line 721 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())}); }
#line 1956 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 83: // primary_expression: column_ref
#line 723 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())}); }
#line 1962 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 84: // primary_expression: IDENTIFIER LPAREN expression_list RPAREN
#line 725 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = makeExpression(ExpressionValue{FunctionCall{.name = std::move(yystack_[3].value.as < std::string > ()), .arguments = std::move(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::ExpressionPtr> > ())}}); }
#line 1968 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 85: // primary_expression: LPAREN expression RPAREN
#line 727 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::ExpressionPtr > () = std::move(yystack_[1].value.as < mldp_pvxs_driver::query::ExpressionPtr > ()); }
#line 1974 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 86: // literal: STRING_LITERAL
#line 732 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{yystack_[0].value.as < std::string > ()}; }
#line 1980 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 87: // literal: NUMBER_LITERAL
#line 734 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{yystack_[0].value.as < int64_t > ()}; }
#line 1986 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 88: // literal: TRUE
#line 736 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{true}; }
#line 1992 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 89: // literal: FALSE
#line 738 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{false}; }
#line 1998 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 90: // literal: TIMESTAMP_NS LPAREN signed_integer RPAREN
#line 740 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::TimestampNsLiteral{yystack_[1].value.as < int64_t > ()}}; }
#line 2004 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 91: // literal: DURATION_NS LPAREN signed_integer RPAREN
#line 742 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::DurationNsLiteral{yystack_[1].value.as < int64_t > ()}}; }
#line 2010 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 92: // literal: DURATION_LITERAL
#line 744 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::DurationNsLiteral{durationToSeconds(yystack_[0].value.as < std::string > (), yystack_[0].location) * 1000000000LL}}; }
#line 2016 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 93: // literal: now_literal
#line 746 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ()); }
#line 2022 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 94: // signed_integer: NUMBER_LITERAL
#line 751 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < int64_t > () = yystack_[0].value.as < int64_t > (); }
#line 2028 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 95: // signed_integer: MINUS NUMBER_LITERAL
#line 753 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < int64_t > () = -yystack_[0].value.as < int64_t > (); }
#line 2034 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 96: // now_literal: NOW
#line 758 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::NowLiteral{0}}; }
#line 2040 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 97: // now_literal: NOW signed_duration
#line 760 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::NowLiteral{yystack_[0].value.as < int64_t > ()}}; }
#line 2046 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 98: // signed_duration: PLUS DURATION_LITERAL
#line 765 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < int64_t > () = durationToSeconds(yystack_[0].value.as < std::string > (), yystack_[0].location); }
#line 2052 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 99: // signed_duration: MINUS DURATION_LITERAL
#line 767 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < int64_t > () = -durationToSeconds(yystack_[0].value.as < std::string > (), yystack_[0].location); }
#line 2058 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 100: // column_ref: identifier_path
#line 772 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QualifiedColumn > () = makeColumn(std::move(yystack_[0].value.as < std::vector<std::string> > ()));
      }
#line 2066 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 101: // identifier_path: IDENTIFIER
#line 779 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<std::string> > () = std::vector<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 2072 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 102: // identifier_path: identifier_path DOT IDENTIFIER
#line 781 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<std::string> > ().push_back(yystack_[0].value.as < std::string > ());
          yylhs.value.as < std::vector<std::string> > () = std::move(yystack_[2].value.as < std::vector<std::string> > ());
      }
#line 2081 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 103: // limit_opt: %empty
#line 789 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<uint64_t> > () = std::nullopt; }
#line 2087 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 104: // limit_opt: LIMIT NUMBER_LITERAL
#line 791 "src/query/parser/grammar/QueryBisonParser.y"
      {
          if (yystack_[0].value.as < int64_t > () < 0)
          {
              throw ParseError("LIMIT must be non-negative", TokenPosition{0, static_cast<std::size_t>(yystack_[0].location.begin.line), static_cast<std::size_t>(yystack_[0].location.begin.column)});
          }
          yylhs.value.as < std::optional<uint64_t> > () = std::optional<uint64_t>{static_cast<uint64_t>(yystack_[0].value.as < int64_t > ())};
      }
#line 2099 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 105: // page_opt: %empty
#line 802 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::nullopt; }
#line 2105 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 106: // page_opt: PAGE TOKEN STRING_LITERAL
#line 804 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 2111 "src/query/parser/generated/QueryBisonParser.cpp"
    break;


#line 2115 "src/query/parser/generated/QueryBisonParser.cpp"

            default:
              break;
            }
        }
#if YY_EXCEPTIONS
      catch (const syntax_error& yyexc)
        {
          YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
          error (yyexc);
          YYERROR;
        }
#endif // YY_EXCEPTIONS
      YY_SYMBOL_PRINT ("-> $$ =", yylhs);
      yypop_ (yylen);
      yylen = 0;

      // Shift the result of the reduction.
      yypush_ (YY_NULLPTR, YY_MOVE (yylhs));
    }
    goto yynewstate;


  /*--------------------------------------.
  | yyerrlab -- here on detecting error.  |
  `--------------------------------------*/
  yyerrlab:
    // If not already recovering from an error, report this error.
    if (!yyerrstatus_)
      {
        ++yynerrs_;
        context yyctx (*this, yyla);
        std::string msg = yysyntax_error_ (yyctx);
        error (yyla.location, YY_MOVE (msg));
      }


    yyerror_range[1].location = yyla.location;
    if (yyerrstatus_ == 3)
      {
        /* If just tried and failed to reuse lookahead token after an
           error, discard it.  */

        // Return failure if at end of input.
        if (yyla.kind () == symbol_kind::S_YYEOF)
          YYABORT;
        else if (!yyla.empty ())
          {
            yy_destroy_ ("Error: discarding", yyla);
            yyla.clear ();
          }
      }

    // Else will try to reuse lookahead token after shifting the error token.
    goto yyerrlab1;


  /*---------------------------------------------------.
  | yyerrorlab -- error raised explicitly by YYERROR.  |
  `---------------------------------------------------*/
  yyerrorlab:
    /* Pacify compilers when the user code never invokes YYERROR and
       the label yyerrorlab therefore never appears in user code.  */
    if (false)
      YYERROR;

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYERROR.  */
    yypop_ (yylen);
    yylen = 0;
    YY_STACK_PRINT ();
    goto yyerrlab1;


  /*-------------------------------------------------------------.
  | yyerrlab1 -- common code for both syntax error and YYERROR.  |
  `-------------------------------------------------------------*/
  yyerrlab1:
    yyerrstatus_ = 3;   // Each real token shifted decrements this.
    // Pop stack until we find a state that shifts the error token.
    for (;;)
      {
        yyn = yypact_[+yystack_[0].state];
        if (!yy_pact_value_is_default_ (yyn))
          {
            yyn += symbol_kind::S_YYerror;
            if (0 <= yyn && yyn <= yylast_
                && yycheck_[yyn] == symbol_kind::S_YYerror)
              {
                yyn = yytable_[yyn];
                if (0 < yyn)
                  break;
              }
          }

        // Pop the current state because it cannot handle the error token.
        if (yystack_.size () == 1)
          YYABORT;

        yyerror_range[1].location = yystack_[0].location;
        yy_destroy_ ("Error: popping", yystack_[0]);
        yypop_ ();
        YY_STACK_PRINT ();
      }
    {
      stack_symbol_type error_token;

      yyerror_range[2].location = yyla.location;
      YYLLOC_DEFAULT (error_token.location, yyerror_range, 2);

      // Shift the error token.
      error_token.state = state_type (yyn);
      yypush_ ("Shifting", YY_MOVE (error_token));
    }
    goto yynewstate;


  /*-------------------------------------.
  | yyacceptlab -- YYACCEPT comes here.  |
  `-------------------------------------*/
  yyacceptlab:
    yyresult = 0;
    goto yyreturn;


  /*-----------------------------------.
  | yyabortlab -- YYABORT comes here.  |
  `-----------------------------------*/
  yyabortlab:
    yyresult = 1;
    goto yyreturn;


  /*-----------------------------------------------------.
  | yyreturn -- parsing is finished, return the result.  |
  `-----------------------------------------------------*/
  yyreturn:
    if (!yyla.empty ())
      yy_destroy_ ("Cleanup: discarding lookahead", yyla);

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYABORT or YYACCEPT.  */
    yypop_ (yylen);
    YY_STACK_PRINT ();
    while (1 < yystack_.size ())
      {
        yy_destroy_ ("Cleanup: popping", yystack_[0]);
        yypop_ ();
      }

    return yyresult;
  }
#if YY_EXCEPTIONS
    catch (...)
      {
        YYCDEBUG << "Exception caught: cleaning lookahead and stack\n";
        // Do not try to display the values of the reclaimed symbols,
        // as their printers might throw an exception.
        if (!yyla.empty ())
          yy_destroy_ (YY_NULLPTR, yyla);

        while (1 < yystack_.size ())
          {
            yy_destroy_ (YY_NULLPTR, yystack_[0]);
            yypop_ ();
          }
        throw;
      }
#endif // YY_EXCEPTIONS
  }

  void
  QueryBisonParser::error (const syntax_error& yyexc)
  {
    error (yyexc.location, yyexc.what ());
  }

  const char *
  QueryBisonParser::symbol_name (symbol_kind_type yysymbol)
  {
    static const char *const yy_sname[] =
    {
    "END_OF_INPUT", "error", "invalid token", "IDENTIFIER",
  "STRING_LITERAL", "DURATION_LITERAL", "NUMBER_LITERAL", "SELECT", "FROM",
  "WHERE", "IS", "AND", "OR", "NOT", "NULL_LITERAL", "IN", "LIKE",
  "BETWEEN", "LIMIT", "PAGE", "TOKEN", "SHOW", "TABLES", "FUNCTIONS",
  "OPERATORS", "DESCRIBE", "EXPLAIN", "AS", "INNER", "LEFT", "OUTER",
  "JOIN", "ON", "NOW", "PREFIX", "CONTAINS", "ORDER", "BY", "ASC", "DESC",
  "TRUE", "FALSE", "TIMESTAMP_NS", "DURATION_NS", "STAR", "SLASH", "COMMA",
  "SEMICOLON", "DOT", "LPAREN", "RPAREN", "PLUS", "MINUS", "EQ", "NEQ",
  "LT", "LTE", "GT", "GTE", "NOW_LITERAL", "$accept", "input", "statement",
  "select_stmt", "order_by_opt", "order_by_list", "order_by_item",
  "select_list", "select_item_list", "select_item", "table_ref",
  "alias_opt", "join_clauses", "join_clause", "where_opt",
  "predicate_list", "predicate", "window_option_list", "window_option",
  "expression_list", "expression", "legacy_expression", "or_expression",
  "and_expression", "comparison_expression", "additive_expression",
  "multiplicative_expression", "unary_expression", "primary_expression",
  "literal", "signed_integer", "now_literal", "signed_duration",
  "column_ref", "identifier_path", "limit_opt", "page_opt", YY_NULLPTR
    };
    return yy_sname[yysymbol];
  }



  // QueryBisonParser::context.
  QueryBisonParser::context::context (const QueryBisonParser& yyparser, const symbol_type& yyla)
    : yyparser_ (yyparser)
    , yyla_ (yyla)
  {}

  int
  QueryBisonParser::context::expected_tokens (symbol_kind_type yyarg[], int yyargn) const
  {
    // Actual number of expected tokens
    int yycount = 0;

    int yyn = yypact_[+yyparser_.yystack_[0].state];
    if (!yy_pact_value_is_default_ (yyn))
      {
        /* Start YYX at -YYN if negative to avoid negative indexes in
           YYCHECK.  In other words, skip the first -YYN actions for
           this state because they are default actions.  */
        int yyxbegin = yyn < 0 ? -yyn : 0;
        // Stay within bounds of both yycheck and yytname.
        int yychecklim = yylast_ - yyn + 1;
        int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
        for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
          if (yycheck_[yyx + yyn] == yyx && yyx != symbol_kind::S_YYerror
              && !yy_table_value_is_error_ (yytable_[yyx + yyn]))
            {
              if (!yyarg)
                ++yycount;
              else if (yycount == yyargn)
                return 0;
              else
                yyarg[yycount++] = YY_CAST (symbol_kind_type, yyx);
            }
      }

    if (yyarg && yycount == 0 && 0 < yyargn)
      yyarg[0] = symbol_kind::S_YYEMPTY;
    return yycount;
  }



  int
  QueryBisonParser::yy_syntax_error_arguments_ (const context& yyctx,
                                                 symbol_kind_type yyarg[], int yyargn) const
  {
    /* There are many possibilities here to consider:
       - If this state is a consistent state with a default action, then
         the only way this function was invoked is if the default action
         is an error action.  In that case, don't check for expected
         tokens because there are none.
       - The only way there can be no lookahead present (in yyla) is
         if this state is a consistent state with a default action.
         Thus, detecting the absence of a lookahead is sufficient to
         determine that there is no unexpected or expected token to
         report.  In that case, just report a simple "syntax error".
       - Don't assume there isn't a lookahead just because this state is
         a consistent state with a default action.  There might have
         been a previous inconsistent state, consistent state with a
         non-default action, or user semantic action that manipulated
         yyla.  (However, yyla is currently not documented for users.)
       - Of course, the expected token list depends on states to have
         correct lookahead information, and it depends on the parser not
         to perform extra reductions after fetching a lookahead from the
         scanner and before detecting a syntax error.  Thus, state merging
         (from LALR or IELR) and default reductions corrupt the expected
         token list.  However, the list is correct for canonical LR with
         one exception: it will still contain any token that will not be
         accepted due to an error action in a later state.
    */

    if (!yyctx.lookahead ().empty ())
      {
        if (yyarg)
          yyarg[0] = yyctx.token ();
        int yyn = yyctx.expected_tokens (yyarg ? yyarg + 1 : yyarg, yyargn - 1);
        return yyn + 1;
      }
    return 0;
  }

  // Generate an error message.
  std::string
  QueryBisonParser::yysyntax_error_ (const context& yyctx) const
  {
    // Its maximum.
    enum { YYARGS_MAX = 5 };
    // Arguments of yyformat.
    symbol_kind_type yyarg[YYARGS_MAX];
    int yycount = yy_syntax_error_arguments_ (yyctx, yyarg, YYARGS_MAX);

    char const* yyformat = YY_NULLPTR;
    switch (yycount)
      {
#define YYCASE_(N, S)                         \
        case N:                               \
          yyformat = S;                       \
        break
      default: // Avoid compiler warnings.
        YYCASE_ (0, YY_("syntax error"));
        YYCASE_ (1, YY_("syntax error, unexpected %s"));
        YYCASE_ (2, YY_("syntax error, unexpected %s, expecting %s"));
        YYCASE_ (3, YY_("syntax error, unexpected %s, expecting %s or %s"));
        YYCASE_ (4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
        YYCASE_ (5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
      }

    std::string yyres;
    // Argument number.
    std::ptrdiff_t yyi = 0;
    for (char const* yyp = yyformat; *yyp; ++yyp)
      if (yyp[0] == '%' && yyp[1] == 's' && yyi < yycount)
        {
          yyres += symbol_name (yyarg[yyi++]);
          ++yyp;
        }
      else
        yyres += *yyp;
    return yyres;
  }


  const signed char QueryBisonParser::yypact_ninf_ = -109;

  const signed char QueryBisonParser::yytable_ninf_ = -1;

  const short
  QueryBisonParser::yypact_[] =
  {
       9,    45,    52,    14,    46,    27,    67,  -109,    41,  -109,
    -109,  -109,   110,   -20,  -109,  -109,    43,    55,  -109,   110,
     110,   110,    98,    62,  -109,    68,    99,   108,  -109,   138,
      -4,  -109,  -109,  -109,  -109,  -109,    73,  -109,  -109,  -109,
    -109,    73,  -109,  -109,  -109,   110,  -109,   119,   129,  -109,
       4,     4,    85,  -109,  -109,    17,   110,   136,   110,   110,
     110,   110,   110,   110,   110,   110,   110,   110,   110,   110,
     139,   -22,  -109,  -109,  -109,  -109,   141,    91,    94,  -109,
      46,  -109,    20,  -109,  -109,   108,  -109,    -4,    -4,    29,
      29,    29,    29,    29,    29,  -109,  -109,  -109,   110,  -109,
    -109,  -109,  -109,    95,    74,  -109,   145,  -109,  -109,    11,
      14,   118,    87,    17,  -109,   122,  -109,  -109,   143,  -109,
     121,    17,   126,    17,   171,   167,   155,    14,   192,   157,
       3,     3,     3,     3,     3,     3,     3,     3,     3,     3,
     176,    17,   177,    14,   110,   204,   193,  -109,   197,    58,
    -109,  -109,   202,  -109,  -109,  -109,  -109,  -109,  -109,  -109,
    -109,    14,   182,    14,   162,   170,  -109,    88,  -109,   198,
    -109,  -109,    -8,   -28,     3,   164,    14,   166,    14,   110,
    -109,  -109,   216,   218,  -109,   218,  -109,  -109,    14,   169,
      14,  -109,  -109,  -109,   123,   -21,  -109,    23,  -109,    14,
    -109,  -109,  -109,   218,  -109,  -109,  -109,  -109
  };

  const signed char
  QueryBisonParser::yydefact_[] =
  {
       0,     0,     0,     0,     0,     0,     0,     3,   101,    86,
      92,    87,     0,    96,    88,    89,     0,     0,    17,     0,
       0,     0,     0,    18,    19,    21,    59,    61,    63,    65,
      72,    75,    78,    82,    93,    83,   100,     4,     5,     6,
     101,     7,     8,     1,     2,     0,    81,     0,     0,    97,
       0,     0,     0,    79,    80,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    57,    98,    99,    94,     0,     0,     0,    85,
       0,    28,    25,    20,    22,    62,    64,    73,    74,    66,
      67,    68,    69,    70,    71,    76,    77,   102,     0,    84,
      95,    90,    91,     0,    34,    27,     0,    23,    58,    25,
       0,     0,     0,     0,    29,    10,    26,    24,    35,    36,
       0,     0,     0,     0,     0,     0,   103,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   105,    37,     0,     0,
      44,    60,     0,    46,    45,    47,    48,    49,    50,    51,
      52,     0,     0,     0,     0,    11,    12,    14,   104,     0,
       9,    42,     0,     0,     0,     0,     0,     0,     0,     0,
      15,    16,     0,     0,    39,     0,    38,    43,     0,     0,
       0,    30,    13,   106,     0,     0,    53,     0,    31,     0,
      32,    55,    56,     0,    41,    40,    33,    54
  };

  const short
  QueryBisonParser::yypgoto_[] =
  {
    -109,  -109,  -109,    -3,  -109,  -109,    47,  -109,  -109,   168,
    -108,   116,  -109,  -109,  -109,  -109,   100,    44,    25,    81,
     -19,    49,  -109,   173,   174,   135,    72,    -9,    33,  -109,
     181,  -109,  -109,  -106,    -1,  -109,  -109
  };

  const short
  QueryBisonParser::yydefgoto_[] =
  {
      -1,     5,     6,     7,   126,   165,   166,    22,    23,    24,
      81,   107,   104,   114,   115,   118,   119,   195,   196,    71,
      25,   150,    26,    27,    28,    29,    30,    31,    32,    33,
      77,    34,    49,    35,    36,   146,   170
  };

  const unsigned char
  QueryBisonParser::yytable_[] =
  {
      52,    42,    41,    46,   120,   124,     8,     9,    10,    11,
      75,    53,    54,   140,   105,   142,     1,    40,    98,   185,
      40,   120,   186,   105,    98,   203,    72,    43,    99,   204,
       2,    47,    48,   162,     3,     4,    13,   164,   106,   183,
      68,    69,   184,    14,    15,    16,    17,   106,     8,     9,
      10,    11,    19,     1,    82,   175,    76,   177,    12,    95,
      96,     8,     9,    10,    11,     1,    80,    44,    70,   203,
     189,    12,   191,   205,    37,    38,    39,   103,    13,   108,
      60,    61,   198,   110,   200,    14,    15,    16,    17,    18,
      45,    13,    50,   206,    19,    57,    20,    21,    14,    15,
      16,    17,   111,   112,    51,   113,    55,    19,    56,    20,
      21,    58,    82,     8,     9,    10,    11,   122,   123,    59,
      82,    70,    82,    12,    73,   167,   180,   181,   201,   202,
      72,   128,    87,    88,    74,    79,   129,   130,   131,    84,
      82,   101,    97,    13,   102,   109,   172,   100,   116,   121,
      14,    15,    16,    17,   127,   132,   133,   141,   125,    19,
     167,    20,    21,   151,   151,   151,   151,   151,   151,   151,
     151,   151,   151,   145,   134,   135,   136,   137,   138,   139,
     152,   153,   154,   155,   156,   157,   158,   159,   160,    60,
      61,    62,    63,    64,    65,    66,    67,    89,    90,    91,
      92,    93,    94,   143,   144,   148,   149,   151,   161,   163,
     168,   171,   169,   174,   176,   178,   179,   188,   182,   190,
     193,   194,   199,   187,    83,   117,   192,   147,   207,   197,
     173,    85,    78,    86
  };

  const unsigned char
  QueryBisonParser::yycheck_[] =
  {
      19,     4,     3,    12,   110,   113,     3,     4,     5,     6,
       6,    20,    21,   121,     3,   123,     7,     3,    46,    47,
       3,   127,    50,     3,    46,    46,    45,     0,    50,    50,
      21,    51,    52,   141,    25,    26,    33,   143,    27,    47,
      44,    45,    50,    40,    41,    42,    43,    27,     3,     4,
       5,     6,    49,     7,    55,   161,    52,   163,    13,    68,
      69,     3,     4,     5,     6,     7,    49,     0,    48,    46,
     176,    13,   178,    50,    22,    23,    24,    80,    33,    98,
      51,    52,   188,     9,   190,    40,    41,    42,    43,    44,
      49,    33,    49,   199,    49,    27,    51,    52,    40,    41,
      42,    43,    28,    29,    49,    31,     8,    49,    46,    51,
      52,    12,   113,     3,     4,     5,     6,    30,    31,    11,
     121,    48,   123,    13,     5,   144,    38,    39,     5,     6,
     149,    10,    60,    61,     5,    50,    15,    16,    17,     3,
     141,    50,     3,    33,    50,    50,   149,     6,     3,    31,
      40,    41,    42,    43,    11,    34,    35,    31,    36,    49,
     179,    51,    52,   130,   131,   132,   133,   134,   135,   136,
     137,   138,   139,    18,    53,    54,    55,    56,    57,    58,
     131,   132,   133,   134,   135,   136,   137,   138,   139,    51,
      52,    53,    54,    55,    56,    57,    58,    62,    63,    64,
      65,    66,    67,    32,    37,    13,    49,   174,    32,    32,
       6,    14,    19,    11,    32,    53,    46,    53,    20,    53,
       4,     3,    53,   174,    56,   109,   179,   127,   203,   185,
     149,    58,    51,    59
  };

  const signed char
  QueryBisonParser::yystos_[] =
  {
       0,     7,    21,    25,    26,    61,    62,    63,     3,     4,
       5,     6,    13,    33,    40,    41,    42,    43,    44,    49,
      51,    52,    67,    68,    69,    80,    82,    83,    84,    85,
      86,    87,    88,    89,    91,    93,    94,    22,    23,    24,
       3,    94,    63,     0,     0,    49,    87,    51,    52,    92,
      49,    49,    80,    87,    87,     8,    46,    27,    12,    11,
      51,    52,    53,    54,    55,    56,    57,    58,    44,    45,
      48,    79,    80,     5,     5,     6,    52,    90,    90,    50,
      49,    70,    94,    69,     3,    83,    84,    86,    86,    85,
      85,    85,    85,    85,    85,    87,    87,     3,    46,    50,
       6,    50,    50,    63,    72,     3,    27,    71,    80,    50,
       9,    28,    29,    31,    73,    74,     3,    71,    75,    76,
      93,    31,    30,    31,    70,    36,    64,    11,    10,    15,
      16,    17,    34,    35,    53,    54,    55,    56,    57,    58,
      70,    31,    70,    32,    37,    18,    95,    76,    13,    49,
      81,    88,    81,    81,    81,    81,    81,    81,    81,    81,
      81,    32,    70,    32,    93,    65,    66,    80,     6,    19,
      96,    14,    63,    79,    11,    93,    32,    93,    53,    46,
      38,    39,    20,    47,    50,    47,    50,    81,    53,    93,
      53,    93,    66,     4,     3,    77,    78,    77,    93,    53,
      93,     5,     6,    46,    50,    50,    93,    78
  };

  const signed char
  QueryBisonParser::yyr1_[] =
  {
       0,    60,    61,    62,    62,    62,    62,    62,    62,    63,
      64,    64,    65,    65,    66,    66,    66,    67,    67,    68,
      68,    69,    69,    70,    70,    71,    71,    71,    72,    72,
      73,    73,    73,    73,    74,    74,    75,    75,    76,    76,
      76,    76,    76,    76,    76,    76,    76,    76,    76,    76,
      76,    76,    76,    77,    77,    78,    78,    79,    79,    80,
      81,    82,    82,    83,    83,    84,    84,    84,    84,    84,
      84,    84,    85,    85,    85,    86,    86,    86,    87,    87,
      87,    87,    88,    88,    88,    88,    89,    89,    89,    89,
      89,    89,    89,    89,    90,    90,    91,    91,    92,    92,
      93,    94,    94,    95,    95,    96,    96
  };

  const signed char
  QueryBisonParser::yyr2_[] =
  {
       0,     2,     2,     1,     2,     2,     2,     2,     2,     9,
       0,     3,     1,     3,     1,     2,     2,     1,     1,     1,
       3,     1,     3,     2,     4,     0,     2,     1,     0,     2,
       6,     7,     7,     8,     0,     2,     1,     3,     5,     5,
       7,     7,     4,     5,     3,     3,     3,     3,     3,     3,
       3,     3,     3,     1,     3,     2,     2,     1,     3,     1,
       1,     1,     3,     1,     3,     1,     3,     3,     3,     3,
       3,     3,     1,     3,     3,     1,     3,     3,     1,     2,
       2,     2,     1,     1,     4,     3,     1,     1,     1,     1,
       4,     4,     1,     1,     1,     2,     1,     2,     2,     2,
       1,     1,     3,     0,     2,     0,     3
  };




#if YYDEBUG
  const short
  QueryBisonParser::yyrline_[] =
  {
       0,   288,   288,   295,   297,   299,   301,   303,   307,   314,
     336,   337,   342,   344,   349,   355,   361,   370,   377,   382,
     384,   389,   391,   396,   403,   414,   415,   417,   423,   424,
     432,   443,   454,   465,   480,   481,   486,   488,   496,   504,
     511,   520,   528,   532,   541,   550,   559,   568,   576,   585,
     594,   603,   612,   624,   626,   631,   633,   638,   642,   650,
     655,   660,   662,   667,   669,   674,   676,   678,   680,   682,
     684,   686,   691,   693,   695,   700,   702,   704,   709,   711,
     713,   715,   720,   722,   724,   726,   731,   733,   735,   737,
     739,   741,   743,   745,   750,   752,   757,   759,   764,   766,
     771,   778,   780,   789,   790,   802,   803
  };

  void
  QueryBisonParser::yy_stack_print_ () const
  {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator
           i = yystack_.begin (),
           i_end = yystack_.end ();
         i != i_end; ++i)
      *yycdebug_ << ' ' << int (i->state);
    *yycdebug_ << '\n';
  }

  void
  QueryBisonParser::yy_reduce_print_ (int yyrule) const
  {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1
               << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
      YY_SYMBOL_PRINT ("   $" << yyi + 1 << " =",
                       yystack_[(yynrhs) - (yyi + 1)]);
  }
#endif // YYDEBUG


#line 5 "src/query/parser/grammar/QueryBisonParser.y"
} } } // mldp_pvxs_driver::query::generated
#line 2685 "src/query/parser/generated/QueryBisonParser.cpp"

#line 807 "src/query/parser/grammar/QueryBisonParser.y"

