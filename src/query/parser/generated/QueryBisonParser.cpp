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

        if (path.front() == "attr")
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

#line 219 "src/query/parser/generated/QueryBisonParser.cpp"


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
#line 312 "src/query/parser/generated/QueryBisonParser.cpp"

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
#line 235 "src/query/parser/grammar/QueryBisonParser.y"
      {
          ctx.result = std::move(yystack_[1].value.as < mldp_pvxs_driver::query::QueryStatement > ());
      }
#line 1058 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 3: // statement: select_stmt
#line 242 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::SelectStatement > ()); }
#line 1064 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 4: // statement: SHOW TABLES
#line 244 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::ShowTablesStatement{}; }
#line 1070 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 5: // statement: DESCRIBE identifier_path
#line 246 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::DescribeStatement{ .table_name = joinPath(yystack_[0].value.as < std::vector<std::string> > (), 0) };
      }
#line 1078 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 6: // statement: EXPLAIN select_stmt
#line 250 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QueryStatement > () = mldp_pvxs_driver::query::ExplainStatement{ .query = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::SelectStatement > ()) };
      }
#line 1086 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 7: // select_stmt: SELECT select_list FROM table_ref join_clauses where_opt limit_opt page_opt
#line 257 "src/query/parser/grammar/QueryBisonParser.y"
      {
          mldp_pvxs_driver::query::SelectStatement statement;
          statement.select_all = yystack_[6].value.as < mldp_pvxs_driver::query::generated::SelectListValue > ().select_all;
          statement.columns = std::move(yystack_[6].value.as < mldp_pvxs_driver::query::generated::SelectListValue > ().columns);
          statement.from = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::TableRef > ());
          statement.joins = std::move(yystack_[3].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ());
          statement.predicates = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ());
          statement.limit = std::move(yystack_[1].value.as < std::optional<uint64_t> > ());
          statement.page_token = std::move(yystack_[0].value.as < std::optional<std::string> > ());
          yylhs.value.as < mldp_pvxs_driver::query::SelectStatement > () = std::move(statement);
      }
#line 1102 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 8: // select_list: STAR
#line 272 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::generated::SelectListValue > () = mldp_pvxs_driver::query::generated::SelectListValue{
              .select_all = true,
              .columns = {}
          };
      }
#line 1113 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 9: // select_list: column_list
#line 279 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::generated::SelectListValue > () = mldp_pvxs_driver::query::generated::SelectListValue{
              .select_all = false,
              .columns = std::move(yystack_[0].value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > ())
          };
      }
#line 1124 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 10: // column_list: column_ref
#line 289 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > () = std::vector<mldp_pvxs_driver::query::QualifiedColumn>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ())};
      }
#line 1132 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 11: // column_list: column_list COMMA column_ref
#line 293 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::QualifiedColumn> > ());
      }
#line 1141 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 12: // table_ref: identifier_path alias_opt
#line 301 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::TableRef > () = mldp_pvxs_driver::query::TableRef{
              .table_name = joinPath(yystack_[1].value.as < std::vector<std::string> > (), 0),
              .alias = std::move(yystack_[0].value.as < std::optional<std::string> > ())
          };
      }
#line 1152 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 13: // alias_opt: %empty
#line 311 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::nullopt; }
#line 1158 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 14: // alias_opt: AS IDENTIFIER
#line 313 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1164 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 15: // alias_opt: IDENTIFIER
#line 315 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1170 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 16: // join_clauses: %empty
#line 320 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > () = {}; }
#line 1176 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 17: // join_clauses: join_clauses join_clause
#line 322 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::JoinClause > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > () = std::move(yystack_[1].value.as < std::vector<mldp_pvxs_driver::query::JoinClause> > ());
      }
#line 1185 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 18: // join_clause: JOIN table_ref ON column_ref EQ column_ref
#line 330 "src/query/parser/grammar/QueryBisonParser.y"
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
#line 1200 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 19: // join_clause: INNER JOIN table_ref ON column_ref EQ column_ref
#line 341 "src/query/parser/grammar/QueryBisonParser.y"
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
#line 1215 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 20: // join_clause: LEFT JOIN table_ref ON column_ref EQ column_ref
#line 352 "src/query/parser/grammar/QueryBisonParser.y"
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
#line 1230 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 21: // join_clause: LEFT OUTER JOIN table_ref ON column_ref EQ column_ref
#line 363 "src/query/parser/grammar/QueryBisonParser.y"
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
#line 1245 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 22: // where_opt: %empty
#line 377 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = {}; }
#line 1251 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 23: // where_opt: WHERE predicate_list
#line 379 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::move(yystack_[0].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ()); }
#line 1257 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 24: // predicate_list: predicate
#line 384 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::vector<mldp_pvxs_driver::query::WherePredicate>{std::move(yystack_[0].value.as < mldp_pvxs_driver::query::WherePredicate > ())}; }
#line 1263 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 25: // predicate_list: predicate_list AND predicate
#line 386 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ().push_back(std::move(yystack_[0].value.as < mldp_pvxs_driver::query::WherePredicate > ()));
          yylhs.value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > () = std::move(yystack_[2].value.as < std::vector<mldp_pvxs_driver::query::WherePredicate> > ());
      }
#line 1272 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 26: // predicate: column_ref IN LPAREN literal RPAREN
#line 394 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::InPredicate{
              .column = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .values = std::vector<mldp_pvxs_driver::query::LiteralValue>{std::move(yystack_[1].value.as < mldp_pvxs_driver::query::LiteralValue > ())}
          };
      }
#line 1283 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 27: // predicate: column_ref IN LPAREN literal COMMA literal RPAREN
#line 401 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::InPredicate{
              .column = std::move(yystack_[6].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .values = std::vector<mldp_pvxs_driver::query::LiteralValue>{std::move(yystack_[3].value.as < mldp_pvxs_driver::query::LiteralValue > ()), std::move(yystack_[1].value.as < mldp_pvxs_driver::query::LiteralValue > ())}
          };
      }
#line 1294 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 28: // predicate: column_ref BETWEEN literal AND literal
#line 408 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::RangePredicate{
              .column = std::move(yystack_[4].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .lower = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::LiteralValue > ()),
              .upper = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1306 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 29: // predicate: column_ref LIKE literal
#line 416 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LIKE,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1318 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 30: // predicate: column_ref CONTAINS literal
#line 424 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::CONTAINS,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1330 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 31: // predicate: column_ref PREFIX literal
#line 432 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::PREFIX,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1342 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 32: // predicate: column_ref EQ literal
#line 440 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::EqPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1353 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 33: // predicate: column_ref NEQ literal
#line 447 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::NEQ,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1365 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 34: // predicate: column_ref LT literal
#line 455 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LT,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1377 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 35: // predicate: column_ref LTE literal
#line 463 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::LTE,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1389 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 36: // predicate: column_ref GT literal
#line 471 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::GT,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1401 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 37: // predicate: column_ref GTE literal
#line 479 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::WherePredicate > () = mldp_pvxs_driver::query::OpPredicate{
              .column = std::move(yystack_[2].value.as < mldp_pvxs_driver::query::QualifiedColumn > ()),
              .op = mldp_pvxs_driver::query::PredicateBinaryOp::GTE,
              .value = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ())
          };
      }
#line 1413 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 38: // literal: STRING_LITERAL
#line 490 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{yystack_[0].value.as < std::string > ()}; }
#line 1419 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 39: // literal: NUMBER_LITERAL
#line 492 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{yystack_[0].value.as < int64_t > ()}; }
#line 1425 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 40: // literal: now_literal
#line 494 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = std::move(yystack_[0].value.as < mldp_pvxs_driver::query::LiteralValue > ()); }
#line 1431 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 41: // now_literal: NOW
#line 499 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::NowLiteral{0}}; }
#line 1437 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 42: // now_literal: NOW signed_duration
#line 501 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < mldp_pvxs_driver::query::LiteralValue > () = mldp_pvxs_driver::query::LiteralValue{mldp_pvxs_driver::query::NowLiteral{yystack_[0].value.as < int64_t > ()}}; }
#line 1443 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 43: // signed_duration: PLUS DURATION_LITERAL
#line 506 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < int64_t > () = durationToSeconds(yystack_[0].value.as < std::string > (), yystack_[0].location); }
#line 1449 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 44: // signed_duration: MINUS DURATION_LITERAL
#line 508 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < int64_t > () = -durationToSeconds(yystack_[0].value.as < std::string > (), yystack_[0].location); }
#line 1455 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 45: // column_ref: identifier_path
#line 513 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yylhs.value.as < mldp_pvxs_driver::query::QualifiedColumn > () = makeColumn(std::move(yystack_[0].value.as < std::vector<std::string> > ()));
      }
#line 1463 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 46: // identifier_path: IDENTIFIER
#line 520 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::vector<std::string> > () = std::vector<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1469 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 47: // identifier_path: identifier_path DOT IDENTIFIER
#line 522 "src/query/parser/grammar/QueryBisonParser.y"
      {
          yystack_[2].value.as < std::vector<std::string> > ().push_back(yystack_[0].value.as < std::string > ());
          yylhs.value.as < std::vector<std::string> > () = std::move(yystack_[2].value.as < std::vector<std::string> > ());
      }
#line 1478 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 48: // limit_opt: %empty
#line 530 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<uint64_t> > () = std::nullopt; }
#line 1484 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 49: // limit_opt: LIMIT NUMBER_LITERAL
#line 532 "src/query/parser/grammar/QueryBisonParser.y"
      {
          if (yystack_[0].value.as < int64_t > () < 0)
          {
              throw ParseError("LIMIT must be non-negative", TokenPosition{0, static_cast<std::size_t>(yystack_[0].location.begin.line), static_cast<std::size_t>(yystack_[0].location.begin.column)});
          }
          yylhs.value.as < std::optional<uint64_t> > () = std::optional<uint64_t>{static_cast<uint64_t>(yystack_[0].value.as < int64_t > ())};
      }
#line 1496 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 50: // page_opt: %empty
#line 543 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::nullopt; }
#line 1502 "src/query/parser/generated/QueryBisonParser.cpp"
    break;

  case 51: // page_opt: PAGE TOKEN STRING_LITERAL
#line 545 "src/query/parser/grammar/QueryBisonParser.y"
      { yylhs.value.as < std::optional<std::string> > () = std::optional<std::string>{yystack_[0].value.as < std::string > ()}; }
#line 1508 "src/query/parser/generated/QueryBisonParser.cpp"
    break;


#line 1512 "src/query/parser/generated/QueryBisonParser.cpp"

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
  "JOIN", "ON", "NOW", "PREFIX", "CONTAINS", "STAR", "COMMA", "DOT",
  "LPAREN", "RPAREN", "PLUS", "MINUS", "EQ", "NEQ", "LT", "LTE", "GT",
  "GTE", "$accept", "input", "statement", "select_stmt", "select_list",
  "column_list", "table_ref", "alias_opt", "join_clauses", "join_clause",
  "where_opt", "predicate_list", "predicate", "literal", "now_literal",
  "signed_duration", "column_ref", "identifier_path", "limit_opt",
  "page_opt", YY_NULLPTR
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


  const signed char QueryBisonParser::yypact_ninf_ = -47;

  const signed char QueryBisonParser::yytable_ninf_ = -1;

  const signed char
  QueryBisonParser::yypact_[] =
  {
      16,    15,   -16,    12,    25,    13,    22,   -47,   -47,   -47,
      39,     3,   -47,    17,   -47,    17,   -47,   -47,   -47,    12,
      12,    56,   -47,     9,   -47,   -47,    49,   -47,    60,   -47,
      12,    41,     4,    12,   -47,    50,   -47,    57,   -47,    14,
      12,    43,    12,    44,    63,    58,    12,    42,    10,    10,
      10,    10,    10,    10,    10,    10,    10,    10,    51,    12,
      52,    12,   -47,    65,   -47,   -47,    10,   -47,   -47,    26,
     -47,   -47,    66,   -47,   -47,   -47,   -47,   -47,   -47,   -47,
     -47,    12,    53,    12,    46,    80,   -10,    81,    82,   -47,
      10,    48,    12,    55,    12,   -47,    10,   -47,   -47,   -47,
     -47,    12,    59,    12,   -47,    54,   -47,    12,   -47,   -47,
     -47
  };

  const signed char
  QueryBisonParser::yydefact_[] =
  {
       0,     0,     0,     0,     0,     0,     0,     3,    46,     8,
       0,     9,    10,    45,     4,     5,     6,     1,     2,     0,
       0,     0,    16,    13,    11,    47,    22,    15,     0,    12,
       0,     0,     0,     0,    17,    48,    14,    23,    24,     0,
       0,     0,     0,     0,     0,    50,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    49,     0,     7,    25,     0,    38,    39,    41,
      29,    40,     0,    31,    30,    32,    33,    34,    35,    36,
      37,     0,     0,     0,     0,     0,     0,     0,     0,    42,
       0,     0,     0,     0,     0,    51,     0,    26,    43,    44,
      28,     0,     0,     0,    18,     0,    19,     0,    20,    27,
      21
  };

  const signed char
  QueryBisonParser::yypgoto_[] =
  {
     -47,   -47,   -47,    85,   -47,   -47,     6,   -47,   -47,   -47,
     -47,   -47,    61,   -46,   -47,   -47,    -1,    -2,   -47,   -47
  };

  const signed char
  QueryBisonParser::yydefgoto_[] =
  {
      -1,     5,     6,     7,    10,    11,    22,    29,    26,    34,
      35,    37,    38,    70,    71,    89,    39,    13,    45,    64
  };

  const signed char
  QueryBisonParser::yytable_[] =
  {
      12,    15,    14,    72,    73,    74,    75,    76,    77,    78,
      79,    80,    27,    17,    67,     8,    68,    23,     8,    24,
      86,    96,    18,     1,    97,    47,    48,    49,    41,    42,
      28,    23,     1,     2,    20,     3,     4,    69,    23,    43,
      23,    21,    50,    51,   100,     9,    58,    19,    60,    21,
     105,    52,    53,    54,    55,    56,    57,    23,    30,    25,
      84,    87,    88,    36,    44,    82,    40,    46,    59,    62,
      61,    31,    32,    63,    33,    66,    90,    81,    83,    92,
      91,    85,    93,    94,    95,   101,    98,    99,   109,    16,
       0,   102,   103,   104,     0,     0,   107,     0,     0,     0,
     106,     0,   108,     0,     0,     0,   110,    65
  };

  const signed char
  QueryBisonParser::yycheck_[] =
  {
       1,     3,    18,    49,    50,    51,    52,    53,    54,    55,
      56,    57,     3,     0,     4,     3,     6,    19,     3,    20,
      66,    31,     0,     7,    34,    11,    12,    13,    24,    25,
      21,    33,     7,    17,    31,    19,    20,    27,    40,    33,
      42,    32,    28,    29,    90,    30,    40,     8,    42,    32,
      96,    37,    38,    39,    40,    41,    42,    59,     9,     3,
      61,    35,    36,     3,    14,    59,    25,    10,    25,     6,
      26,    22,    23,    15,    25,    33,    10,    26,    26,    26,
      81,    16,    83,    37,     4,    37,     5,     5,    34,     4,
      -1,    92,    37,    94,    -1,    -1,    37,    -1,    -1,    -1,
     101,    -1,   103,    -1,    -1,    -1,   107,    46
  };

  const signed char
  QueryBisonParser::yystos_[] =
  {
       0,     7,    17,    19,    20,    44,    45,    46,     3,    30,
      47,    48,    59,    60,    18,    60,    46,     0,     0,     8,
      31,    32,    49,    60,    59,     3,    51,     3,    21,    50,
       9,    22,    23,    25,    52,    53,     3,    54,    55,    59,
      25,    24,    25,    49,    14,    61,    10,    11,    12,    13,
      28,    29,    37,    38,    39,    40,    41,    42,    49,    25,
      49,    26,     6,    15,    62,    55,    33,     4,     6,    27,
      56,    57,    56,    56,    56,    56,    56,    56,    56,    56,
      56,    26,    49,    26,    59,    16,    56,    35,    36,    58,
      10,    59,    26,    59,    37,     4,    31,    34,     5,     5,
      56,    37,    59,    37,    59,    56,    59,    37,    59,    34,
      59
  };

  const signed char
  QueryBisonParser::yyr1_[] =
  {
       0,    43,    44,    45,    45,    45,    45,    46,    47,    47,
      48,    48,    49,    50,    50,    50,    51,    51,    52,    52,
      52,    52,    53,    53,    54,    54,    55,    55,    55,    55,
      55,    55,    55,    55,    55,    55,    55,    55,    56,    56,
      56,    57,    57,    58,    58,    59,    60,    60,    61,    61,
      62,    62
  };

  const signed char
  QueryBisonParser::yyr2_[] =
  {
       0,     2,     2,     1,     2,     2,     2,     8,     1,     1,
       1,     3,     2,     0,     2,     1,     0,     2,     6,     7,
       7,     8,     0,     2,     1,     3,     5,     7,     5,     3,
       3,     3,     3,     3,     3,     3,     3,     3,     1,     1,
       1,     1,     2,     2,     2,     1,     1,     3,     0,     2,
       0,     3
  };




#if YYDEBUG
  const short
  QueryBisonParser::yyrline_[] =
  {
       0,   234,   234,   241,   243,   245,   249,   256,   271,   278,
     288,   292,   300,   311,   312,   314,   320,   321,   329,   340,
     351,   362,   377,   378,   383,   385,   393,   400,   407,   415,
     423,   431,   439,   446,   454,   462,   470,   478,   489,   491,
     493,   498,   500,   505,   507,   512,   519,   521,   530,   531,
     543,   544
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
#line 2003 "src/query/parser/generated/QueryBisonParser.cpp"

#line 548 "src/query/parser/grammar/QueryBisonParser.y"
