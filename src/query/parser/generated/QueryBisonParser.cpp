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
#line 32 "src/query/parser/grammar/QueryBisonParser.y"

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

#line 223 "src/query/parser/generated/QueryBisonParser.cpp"


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
#line 316 "src/query/parser/generated/QueryBisonParser.cpp"

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
      case symbol_kind::S_signed_duration: // signed_duration
        value.YY_MOVE_OR_COPY< int64_t > (YY_MOVE (that.value));
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

      case symbol_kind::S_join_clauses: // join_clauses
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::JoinClause> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_literal_list: // literal_list
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::LiteralValue> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::OrderByItem> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_column_list: // column_list
        value.YY_MOVE_OR_COPY< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > (YY_MOVE (that.value));
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
      case symbol_kind::S_signed_duration: // signed_duration
        value.move< int64_t > (YY_MOVE (that.value));
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

      case symbol_kind::S_join_clauses: // join_clauses
        value.move< std::vector<mldp_pvxs_driver::query::JoinClause> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_literal_list: // literal_list
        value.move< std::vector<mldp_pvxs_driver::query::LiteralValue> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.move< std::vector<mldp_pvxs_driver::query::OrderByItem> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_column_list: // column_list
        value.move< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > (YY_MOVE (that.value));
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
      case symbol_kind::S_signed_duration: // signed_duration
        value.copy< int64_t > (that.value);
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

      case symbol_kind::S_join_clauses: // join_clauses
        value.copy< std::vector<mldp_pvxs_driver::query::JoinClause> > (that.value);
        break;

      case symbol_kind::S_literal_list: // literal_list
        value.copy< std::vector<mldp_pvxs_driver::query::LiteralValue> > (that.value);
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.copy< std::vector<mldp_pvxs_driver::query::OrderByItem> > (that.value);
        break;

      case symbol_kind::S_column_list: // column_list
        value.copy< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > (that.value);
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
      case symbol_kind::S_signed_duration: // signed_duration
        value.move< int64_t > (that.value);
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

      case symbol_kind::S_join_clauses: // join_clauses
        value.move< std::vector<mldp_pvxs_driver::query::JoinClause> > (that.value);
        break;

      case symbol_kind::S_literal_list: // literal_list
        value.move< std::vector<mldp_pvxs_driver::query::LiteralValue> > (that.value);
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.move< std::vector<mldp_pvxs_driver::query::OrderByItem> > (that.value);
        break;

      case symbol_kind::S_column_list: // column_list
        value.move< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > (that.value);
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
      case symbol_kind::S_signed_duration: // signed_duration
        yylhs.value.emplace< int64_t > ();
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

      case symbol_kind::S_join_clauses: // join_clauses
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::JoinClause> > ();
        break;

      case symbol_kind::S_literal_list: // literal_list
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::LiteralValue> > ();
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::OrderByItem> > ();
        break;

      case symbol_kind::S_column_list: // column_list
        yylhs.value.emplace< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > ();
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
#line 242 "src/query/parser/grammar/QueryBisonParser.y"
      {
          ctx.result = std::move(yystack_[1].value.as < mldp_pvxs_driver::query::QueryStatement > ());
      }
#line 1127 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 3: // statement: select_stmt
#line 249 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::SelectStatement > ()); }
#line 1133 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 4: // statement: SHOW TABLES
#line 251 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::ShowTablesStatement{}; }
#line 1139 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 5: // statement: DESCRIBE identifier_path
#line 253 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::DescribeStatement{ .table_name = joinPath(yystack_[0].value.as < std::vector<std::string> > (), 0) };
      }
#line 1147 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 6: // statement: EXPLAIN select_stmt
#line 257 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::ExplainStatement{ .query = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::SelectStatement > ()) };
      }
#line 1155 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 7: // select_stmt: SELECT select_list FROM table_ref join_clauses where_opt order_by_opt limit_opt page_opt
#line 264 "src/query/parser/grammar/QueryBisonParser.y"
      {
          mldp_pvxs_driver::query::SelectStatement statement;
          statement.select_all = yystack_[7].value.as < mldp_pvxs_driver::query::generated::SelectListValue > ().select_all;
          statement.columns = std::move(yystack_[7].value.as < mldp_pvxs_driver::query::generated::SelectListValue > ().columns);
          statement.from = std::move(yystack_[5].value.as < mldp_pvxs_driver::query::TableRef > ());
          statement.joins = std::move(yystack_[4].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ());
          statement.predicates = std::move(yystack_[3].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ());
          statement.order_by = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > ());
          statement.limit = std::move(yystack_[1].value.as < std::optional<uint64_t> > ());
          statement.page_token = std::move(yystack_[0].value.as < std::optional<std::string> > ());
          yylhs.value.as < mldp_pvxs_driver::query::SelectStatement > () = std::move(statement);
      }
#line 1172 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 8: // order_by_opt: %empty
#line 280 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > () = {}; }
#line 1178 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 9: // order_by_opt: ORDER BY order_by_list
#line 282 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > () = std::move(yystack_[0].value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > ()); }
#line 1184 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 10: // order_by_list: order_by_item
#line 287 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > () = std::vector<mldp_pvxs_driver::query::OrderByItem>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::OrderByItem > ())}; }
#line 1190 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 11: // order_by_list: order_by_list COMMA order_by_item
#line 289 "src/query/parser/grammar/QueryBisonParser.y"
      { yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::OrderByItem > ())); yylhs.value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::OrderByItem> > ()); }
#line 1196 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 12: // order_by_item: column_ref
#line 294 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::OrderByItem > () = mldp_pvxs_driver::query::OrderByItem{.column = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())}; }
#line 1202 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 13: // order_by_item: column_ref ASC
#line 296 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::OrderByItem > () = mldp_pvxs_driver::query::OrderByItem{.column = std::move(yystack_[1].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()), .direction = mldp_pvxs_driver::query::SortDirection::ASCENDING}; }
#line 1208 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 14: // order_by_item: column_ref DESC
#line 298 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::OrderByItem > () = mldp_pvxs_driver::query::OrderByItem{.column = std::move(yystack_[1].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()), .direction = mldp_pvxs_driver::query::SortDirection::DESCENDING}; }
#line 1214 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 15: // select_list: STAR
#line 303 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::generated::SelectListValue > () = mldp_pvxs_driver::query::generated::SelectListValue{
              .select_all = true,
              .columns = {}
          };
      }
#line 1225 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 16: // select_list: column_list
#line 310 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::generated::SelectListValue > () = mldp_pvxs_driver::query::generated::SelectListValue{
              .select_all = false,
              .columns = std::move(yystack_[0].value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > ())
          };
      }
#line 1236 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 17: // column_list: column_ref
#line 320 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > () = std::vector<mldp_pvxs_driver::query::QualifiedColumn>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())};
      }
#line 1244 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 18: // column_list: column_list COMMA column_ref
#line 324 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > ());
      }
#line 1253 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 19: // table_ref: identifier_path alias_opt
#line 332 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::TableRef > () = mldp_pvxs_driver::query::TableRef{
              .table_name = joinPath(yystack_[1].value.as < std::vector<std::string> > (), 0),
              .alias = std::move(yystack_[0].value.as < std::optional<std::string> > ())
          };
      }
#line 1264 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 20: // alias_opt: %empty
#line 342 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::nullopt; }
#line 1270 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 21: // alias_opt: AS IDENTIFIER
#line 344 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1276 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 22: // alias_opt: IDENTIFIER
#line 346 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1282 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 23: // join_clauses: %empty
#line 351 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > () = {}; }
#line 1288 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 24: // join_clauses: join_clauses join_clause
#line 353 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::JoinClause > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > () = std::move(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ());
      }
#line 1297 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 25: // join_clause: JOIN table_ref ON column_ref EQ column_ref
#line 361 "src/query/parser/grammar/QueryBisonParser.y"
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
#line 1312 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 26: // join_clause: INNER JOIN table_ref ON column_ref EQ column_ref
#line 372 "src/query/parser/grammar/QueryBisonParser.y"
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
#line 1327 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 27: // join_clause: LEFT JOIN table_ref ON column_ref EQ column_ref
#line 383 "src/query/parser/grammar/QueryBisonParser.y"
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
#line 1342 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 28: // join_clause: LEFT OUTER JOIN table_ref ON column_ref EQ column_ref
#line 394 "src/query/parser/grammar/QueryBisonParser.y"
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
#line 1357 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 29: // where_opt: %empty
#line 408 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = {}; }
#line 1363 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 30: // where_opt: WHERE predicate_list
#line 410 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::move(yystack_[0].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ()); }
#line 1369 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 31: // predicate_list: predicate
#line 415 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::vector<mldp_pvxs_driver::query::WherePredicate>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::WherePredicate > ())}; }
#line 1375 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 32: // predicate_list: predicate_list AND predicate
#line 417 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::WherePredicate > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ());
      }
#line 1384 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 33: // predicate: column_ref IN LPAREN literal_list RPAREN
#line 425 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::InPredicate{
              .column = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .values = std::move(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::LiteralValue> > ())
          };
      }
#line 1395 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 34: // predicate: column_ref BETWEEN literal AND literal
#line 432 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::RangePredicate{
              .column = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .lower = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::LiteralValue > ()),
              .upper = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1407 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 35: // predicate: column_ref LIKE literal
#line 440 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LIKE,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1419 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 36: // predicate: column_ref CONTAINS literal
#line 448 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::CONTAINS,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1431 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 37: // predicate: column_ref PREFIX literal
#line 456 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::PREFIX,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1443 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 38: // predicate: column_ref EQ literal
#line 464 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::EqPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1454 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 39: // predicate: column_ref NEQ literal
#line 471 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::NEQ,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1466 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 40: // predicate: column_ref LT literal
#line 479 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LT,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1478 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 41: // predicate: column_ref LTE literal
#line 487 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LTE,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1490 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 42: // predicate: column_ref GT literal
#line 495 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::GT,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1502 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 43: // predicate: column_ref GTE literal
#line 503 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::GTE,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1514 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 44: // literal_list: literal
#line 514 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::LiteralValue> > () = std::vector<mldp_pvxs_driver::query::LiteralValue>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())};
      }
#line 1522 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 45: // literal_list: literal_list COMMA literal
#line 518 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::LiteralValue> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::LiteralValue> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::LiteralValue> > ());
      }
#line 1531 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 46: // literal: STRING_LITERAL
#line 526 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{yystack_[0].value.as < std::string > ()}; }
#line 1537 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 47: // literal: NUMBER_LITERAL
#line 528 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{yystack_[0].value.as < int64_t > ()}; }
#line 1543 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 48: // literal: now_literal
#line 530 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ()); }
#line 1549 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 49: // now_literal: NOW
#line 535 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::NowLiteral{0}}; }
#line 1555 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 50: // now_literal: NOW signed_duration
#line 537 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::NowLiteral{yystack_[0].value.as < int64_t > ()}}; }
#line 1561 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 51: // signed_duration: PLUS DURATION_LITERAL
#line 542 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < int64_t > () = durationToSeconds(yystack_[0].value.as < std::string > (), yystack_[0].location); }
#line 1567 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 52: // signed_duration: MINUS DURATION_LITERAL
#line 544 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < int64_t > () = -durationToSeconds(yystack_[0].value.as < std::string > (), yystack_[0].location); }
#line 1573 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 53: // column_ref: identifier_path
#line 549 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QualifiedColumn > () = makeColumn(std::move(yystack_[0].value.as < std::vector<std::string> > ()));
      }
#line 1581 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 54: // identifier_path: IDENTIFIER
#line 556 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<std::string> > () = std::vector<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1587 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 55: // identifier_path: identifier_path DOT IDENTIFIER
#line 558 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<std::string> > ().push_back(yystack_[0].value.as < std::string > ());
          yylhs.value.as < std::vector<std::string> > () = std::move(yystack_[2].value.as < std::vector<std::string> > ());
      }
#line 1596 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 56: // limit_opt: %empty
#line 566 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<uint64_t> > () = std::nullopt; }
#line 1602 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 57: // limit_opt: LIMIT NUMBER_LITERAL
#line 568 "src/query/parser/grammar/QueryBisonParser.y"
      {
          if (yystack_[0].value.as < int64_t > () < 0)
          {
              throw ParseError("LIMIT must be non-negative", TokenPosition{0, static_cast<std::size_t>(yystack_[0].location.begin.line), static_cast<std::size_t>(yystack_[0].location.begin.column)});
          }
          yylhs.value.as < std::optional<uint64_t> > () = std::optional<uint64_t>{static_cast<uint64_t>(yystack_[0].value.as < int64_t > ())};
      }
#line 1614 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 58: // page_opt: %empty
#line 579 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::nullopt; }
#line 1620 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 59: // page_opt: PAGE TOKEN STRING_LITERAL
#line 581 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1626 "src/query/parser/generated/QueryBisonParser.cpp"
    break;


#line 1630 "src/query/parser/generated/QueryBisonParser.cpp"

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
  "WHERE", "AND", "IN", "LIKE", "BETWEEN", "LIMIT", "PAGE", "TOKEN",
  "SHOW", "TABLES", "DESCRIBE", "EXPLAIN", "AS", "INNER", "LEFT", "OUTER",
  "JOIN", "ON", "NOW", "PREFIX", "CONTAINS", "ORDER", "BY", "ASC", "DESC",
  "STAR", "COMMA", "DOT", "LPAREN", "RPAREN", "PLUS", "MINUS", "EQ", "NEQ",
  "LT", "LTE", "GT", "GTE", "$accept", "input", "statement", "select_stmt",
  "order_by_opt", "order_by_list", "order_by_item", "select_list",
  "column_list", "table_ref", "alias_opt", "join_clauses", "join_clause",
  "where_opt", "predicate_list", "predicate", "literal_list", "literal",
  "now_literal", "signed_duration", "column_ref", "identifier_path",
  "limit_opt", "page_opt", YY_NULLPTR
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


  const signed char QueryBisonParser::yypact_ninf_ = -49;

  const signed char QueryBisonParser::yytable_ninf_ = -1;

  const signed char
  QueryBisonParser::yypact_[] =
  {
      52,    17,    -3,    22,    25,    33,    55,   -49,   -49,   -49,
      44,    21,   -49,    26,   -49,    26,   -49,   -49,   -49,    22,
      22,    60,   -49,    14,   -49,   -49,    45,   -49,    61,   -49,
      22,    40,    -2,    22,   -49,    28,   -49,    63,   -49,     0,
      22,    49,    22,    50,    46,    64,    22,    38,    10,    10,
      10,    10,    10,    10,    10,    10,    10,    10,    53,    22,
      57,    22,    22,    75,    69,   -49,    10,   -49,   -49,    -9,
     -49,   -49,    76,   -49,   -49,   -49,   -49,   -49,   -49,   -49,
     -49,    22,    59,    22,    47,    54,   -49,     6,   -49,    71,
     -49,   -14,   -49,    85,    86,   -49,    10,    51,    22,    62,
      22,    22,   -49,   -49,    89,    10,   -49,   -49,   -49,   -49,
      22,    65,    22,   -49,   -49,   -49,   -49,   -49,    22,   -49,
     -49
  };

  const signed char
  QueryBisonParser::yydefact_[] =
  {
       0,     0,     0,     0,     0,     0,     0,     3,    54,    15,
       0,    16,    17,    53,     4,     5,     6,     1,     2,     0,
       0,     0,    23,    20,    18,    55,    29,    22,     0,    19,
       0,     0,     0,     0,    24,     8,    21,    30,    31,     0,
       0,     0,     0,     0,     0,    56,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    58,    32,     0,    46,    47,    49,
      35,    48,     0,    37,    36,    38,    39,    40,    41,    42,
      43,     0,     0,     0,     0,     9,    10,    12,    57,     0,
       7,     0,    44,     0,     0,    50,     0,     0,     0,     0,
       0,     0,    13,    14,     0,     0,    33,    51,    52,    34,
       0,     0,     0,    25,    11,    59,    45,    26,     0,    27,
      28
  };

  const signed char
  QueryBisonParser::yypgoto_[] =
  {
     -49,   -49,   -49,    90,   -49,   -49,    -5,   -49,   -49,    -6,
     -49,   -49,   -49,   -49,   -49,    56,   -49,   -48,   -49,   -49,
      -1,     7,   -49,   -49
  };

  const signed char
  QueryBisonParser::yydefgoto_[] =
  {
      -1,     5,     6,     7,    45,    85,    86,    10,    11,    22,
      29,    26,    34,    35,    37,    38,    91,    70,    71,    95,
      39,    13,    64,    90
  };

  const signed char
  QueryBisonParser::yytable_[] =
  {
      12,    72,    73,    74,    75,    76,    77,    78,    79,    80,
      15,    47,    48,    49,    67,    14,    68,    27,    92,    24,
       8,   105,    41,    42,   106,     8,    23,    43,    50,    51,
      93,    94,     1,    17,    58,    28,    60,    69,   102,   103,
      23,    52,    53,    54,    55,    56,    57,    23,   109,    23,
      21,     9,    19,    82,    30,    18,    20,   116,    44,     1,
      84,    87,    21,    25,    36,    40,    23,    31,    32,     2,
      33,     3,     4,    46,    59,    66,    61,    62,    63,    81,
      97,    88,    99,    83,    89,    98,    96,   104,   100,   101,
     107,   108,   110,   115,    16,     0,   114,   111,     0,   113,
      87,     0,    65,   112,     0,     0,   118,     0,     0,   117,
       0,   119,     0,     0,     0,     0,     0,   120
  };

  const signed char
  QueryBisonParser::yycheck_[] =
  {
       1,    49,    50,    51,    52,    53,    54,    55,    56,    57,
       3,    11,    12,    13,     4,    18,     6,     3,    66,    20,
       3,    35,    24,    25,    38,     3,    19,    33,    28,    29,
      39,    40,     7,     0,    40,    21,    42,    27,    32,    33,
      33,    41,    42,    43,    44,    45,    46,    40,    96,    42,
      36,    34,     8,    59,     9,     0,    35,   105,    30,     7,
      61,    62,    36,     3,     3,    25,    59,    22,    23,    17,
      25,    19,    20,    10,    25,    37,    26,    31,    14,    26,
      81,     6,    83,    26,    15,    26,    10,    16,    41,    35,
       5,     5,    41,     4,     4,    -1,   101,    98,    -1,   100,
     101,    -1,    46,    41,    -1,    -1,    41,    -1,    -1,   110,
      -1,   112,    -1,    -1,    -1,    -1,    -1,   118
  };

  const signed char
  QueryBisonParser::yystos_[] =
  {
       0,     7,    17,    19,    20,    48,    49,    50,     3,    34,
      54,    55,    67,    68,    18,    68,    50,     0,     0,     8,
      35,    36,    56,    68,    67,     3,    58,     3,    21,    57,
       9,    22,    23,    25,    59,    60,     3,    61,    62,    67,
      25,    24,    25,    56,    30,    51,    10,    11,    12,    13,
      28,    29,    41,    42,    43,    44,    45,    46,    56,    25,
      56,    26,    31,    14,    69,    62,    37,     4,     6,    27,
      64,    65,    64,    64,    64,    64,    64,    64,    64,    64,
      64,    26,    56,    26,    67,    52,    53,    67,     6,    15,
      70,    63,    64,    39,    40,    66,    10,    67,    26,    67,
      41,    35,    32,    33,    16,    35,    38,     5,     5,    64,
      41,    67,    41,    67,    53,     4,    64,    67,    41,    67,
      67
  };

  const signed char
  QueryBisonParser::yyr1_[] =
  {
       0,    47,    48,    49,    49,    49,    49,    50,    51,    51,
      52,    52,    53,    53,    53,    54,    54,    55,    55,    56,
      57,    57,    57,    58,    58,    59,    59,    59,    59,    60,
      60,    61,    61,    62,    62,    62,    62,    62,    62,    62,
      62,    62,    62,    62,    63,    63,    64,    64,    64,    65,
      65,    66,    66,    67,    68,    68,    69,    69,    70,    70
  };

  const signed char
  QueryBisonParser::yyr2_[] =
  {
       0,     2,     2,     1,     2,     2,     2,     9,     0,     3,
       1,     3,     1,     2,     2,     1,     1,     1,     3,     2,
       0,     2,     1,     0,     2,     6,     7,     7,     8,     0,
       2,     1,     3,     5,     5,     3,     3,     3,     3,     3,
       3,     3,     3,     3,     1,     3,     1,     1,     1,     1,
       2,     2,     2,     1,     1,     3,     0,     2,     0,     3
  };




#if YYDEBUG
  const short
  QueryBisonParser::yyrline_[] =
  {
       0,   241,   241,   248,   250,   252,   256,   263,   280,   281,
     286,   288,   293,   295,   297,   302,   309,   319,   323,   331,
     342,   343,   345,   351,   352,   360,   371,   382,   393,   408,
     409,   414,   416,   424,   431,   439,   447,   455,   463,   470,
     478,   486,   494,   502,   513,   517,   525,   527,   529,   534,
     536,   541,   543,   548,   555,   557,   566,   567,   579,   580
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
#line 2129 "src/query/parser/generated/QueryBisonParser.cpp"

#line 584 "src/query/parser/grammar/QueryBisonParser.y"

