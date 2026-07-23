// A Bison parser, made by GNU Bison 3.7.4.

// Skeleton interface for Bison LALR(1) parsers in C++

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


/**
 ** \file src/query/parser/generated/QueryBisonParser.hpp
 ** Define the mldp_pvxs_driver::query::generated::parser class.
 */

// C++ LALR(1) parser skeleton written by Akim Demaille.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.

#ifndef YY_YY_SRC_QUERY_PARSER_GENERATED_QUERYBISONPARSER_HPP_INCLUDED
# define YY_YY_SRC_QUERY_PARSER_GENERATED_QUERYBISONPARSER_HPP_INCLUDED
// "%code requires" blocks.
#line 12 "src/query/parser/grammar/QueryBisonParser.y"

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

#line 69 "src/query/parser/generated/QueryBisonParser.hpp"


# include <cstdlib> // std::abort
# include <iostream>
# include <stdexcept>
# include <string>
# include <vector>

#if defined __cplusplus
# define YY_CPLUSPLUS __cplusplus
#else
# define YY_CPLUSPLUS 199711L
#endif

// Support move semantics when possible.
#if 201103L <= YY_CPLUSPLUS
# define YY_MOVE           std::move
# define YY_MOVE_OR_COPY   move
# define YY_MOVE_REF(Type) Type&&
# define YY_RVREF(Type)    Type&&
# define YY_COPY(Type)     Type
#else
# define YY_MOVE
# define YY_MOVE_OR_COPY   copy
# define YY_MOVE_REF(Type) Type&
# define YY_RVREF(Type)    const Type&
# define YY_COPY(Type)     const Type&
#endif

// Support noexcept when possible.
#if 201103L <= YY_CPLUSPLUS
# define YY_NOEXCEPT noexcept
# define YY_NOTHROW
#else
# define YY_NOEXCEPT
# define YY_NOTHROW throw ()
#endif

// Support constexpr when possible.
#if 201703 <= YY_CPLUSPLUS
# define YY_CONSTEXPR constexpr
#else
# define YY_CONSTEXPR
#endif
# include "location.hh"


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && ! defined __ICC && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                            \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif

#line 5 "src/query/parser/grammar/QueryBisonParser.y"
namespace mldp_pvxs_driver { namespace query { namespace generated {
#line 199 "src/query/parser/generated/QueryBisonParser.hpp"




  /// A Bison parser.
  class QueryBisonParser
  {
  public:
#ifndef YYSTYPE
  /// A buffer to store and retrieve objects.
  ///
  /// Sort of a variant, but does not keep track of the nature
  /// of the stored data, since that knowledge is available
  /// via the current parser state.
  class semantic_type
  {
  public:
    /// Type of *this.
    typedef semantic_type self_type;

    /// Empty construction.
    semantic_type () YY_NOEXCEPT
      : yybuffer_ ()
    {}

    /// Construct and fill.
    template <typename T>
    semantic_type (YY_RVREF (T) t)
    {
      new (yyas_<T> ()) T (YY_MOVE (t));
    }

#if 201103L <= YY_CPLUSPLUS
    /// Non copyable.
    semantic_type (const self_type&) = delete;
    /// Non copyable.
    self_type& operator= (const self_type&) = delete;
#endif

    /// Destruction, allowed only if empty.
    ~semantic_type () YY_NOEXCEPT
    {}

# if 201103L <= YY_CPLUSPLUS
    /// Instantiate a \a T in here from \a t.
    template <typename T, typename... U>
    T&
    emplace (U&&... u)
    {
      return *new (yyas_<T> ()) T (std::forward <U>(u)...);
    }
# else
    /// Instantiate an empty \a T in here.
    template <typename T>
    T&
    emplace ()
    {
      return *new (yyas_<T> ()) T ();
    }

    /// Instantiate a \a T in here from \a t.
    template <typename T>
    T&
    emplace (const T& t)
    {
      return *new (yyas_<T> ()) T (t);
    }
# endif

    /// Instantiate an empty \a T in here.
    /// Obsolete, use emplace.
    template <typename T>
    T&
    build ()
    {
      return emplace<T> ();
    }

    /// Instantiate a \a T in here from \a t.
    /// Obsolete, use emplace.
    template <typename T>
    T&
    build (const T& t)
    {
      return emplace<T> (t);
    }

    /// Accessor to a built \a T.
    template <typename T>
    T&
    as () YY_NOEXCEPT
    {
      return *yyas_<T> ();
    }

    /// Const accessor to a built \a T (for %printer).
    template <typename T>
    const T&
    as () const YY_NOEXCEPT
    {
      return *yyas_<T> ();
    }

    /// Swap the content with \a that, of same type.
    ///
    /// Both variants must be built beforehand, because swapping the actual
    /// data requires reading it (with as()), and this is not possible on
    /// unconstructed variants: it would require some dynamic testing, which
    /// should not be the variant's responsibility.
    /// Swapping between built and (possibly) non-built is done with
    /// self_type::move ().
    template <typename T>
    void
    swap (self_type& that) YY_NOEXCEPT
    {
      std::swap (as<T> (), that.as<T> ());
    }

    /// Move the content of \a that to this.
    ///
    /// Destroys \a that.
    template <typename T>
    void
    move (self_type& that)
    {
# if 201103L <= YY_CPLUSPLUS
      emplace<T> (std::move (that.as<T> ()));
# else
      emplace<T> ();
      swap<T> (that);
# endif
      that.destroy<T> ();
    }

# if 201103L <= YY_CPLUSPLUS
    /// Move the content of \a that to this.
    template <typename T>
    void
    move (self_type&& that)
    {
      emplace<T> (std::move (that.as<T> ()));
      that.destroy<T> ();
    }
#endif

    /// Copy the content of \a that to this.
    template <typename T>
    void
    copy (const self_type& that)
    {
      emplace<T> (that.as<T> ());
    }

    /// Destroy the stored \a T.
    template <typename T>
    void
    destroy ()
    {
      as<T> ().~T ();
    }

  private:
#if YY_CPLUSPLUS < 201103L
    /// Non copyable.
    semantic_type (const self_type&);
    /// Non copyable.
    self_type& operator= (const self_type&);
#endif

    /// Accessor to raw memory as \a T.
    template <typename T>
    T*
    yyas_ () YY_NOEXCEPT
    {
      void *yyp = yybuffer_.yyraw;
      return static_cast<T*> (yyp);
     }

    /// Const accessor to raw memory as \a T.
    template <typename T>
    const T*
    yyas_ () const YY_NOEXCEPT
    {
      const void *yyp = yybuffer_.yyraw;
      return static_cast<const T*> (yyp);
     }

    /// An auxiliary type to compute the largest semantic type.
    union union_type
    {
      // NUMBER_LITERAL
      // signed_duration
      char dummy1[sizeof (int64_t)];

      // join_clause
      char dummy2[sizeof (mldp_pvxs_driver::query::JoinClause)];

      // literal
      // now_literal
      char dummy3[sizeof (mldp_pvxs_driver::query::LiteralValue)];

      // order_by_item
      char dummy4[sizeof (mldp_pvxs_driver::query::OrderByItem)];

      // column_ref
      char dummy5[sizeof (mldp_pvxs_driver::query::QualifiedColumn)];

      // statement
      char dummy6[sizeof (mldp_pvxs_driver::query::QueryStatement)];

      // select_stmt
      char dummy7[sizeof (mldp_pvxs_driver::query::SelectStatement)];

      // table_ref
      char dummy8[sizeof (mldp_pvxs_driver::query::TableRef)];

      // predicate
      char dummy9[sizeof (mldp_pvxs_driver::query::WherePredicate)];

      // select_list
      char dummy10[sizeof (mldp_pvxs_driver::query::generated::SelectListValue)];

      // alias_opt
      // page_opt
      char dummy11[sizeof (std::optional<std::string>)];

      // limit_opt
      char dummy12[sizeof (std::optional<uint64_t>)];

      // IDENTIFIER
      // STRING_LITERAL
      // DURATION_LITERAL
      char dummy13[sizeof (std::string)];

      // join_clauses
      char dummy14[sizeof (std::vector<mldp_pvxs_driver::query::JoinClause>)];

      // order_by_opt
      // order_by_list
      char dummy15[sizeof (std::vector<mldp_pvxs_driver::query::OrderByItem>)];

      // column_list
      char dummy16[sizeof (std::vector<mldp_pvxs_driver::query::QualifiedColumn>)];

      // where_opt
      // predicate_list
      char dummy17[sizeof (std::vector<mldp_pvxs_driver::query::WherePredicate>)];

      // identifier_path
      char dummy18[sizeof (std::vector<std::string>)];
    };

    /// The size of the largest semantic type.
    enum { size = sizeof (union_type) };

    /// A buffer to store semantic values.
    union
    {
      /// Strongest alignment constraints.
      long double yyalign_me;
      /// A buffer large enough to store any of the semantic values.
      char yyraw[size];
    } yybuffer_;
  };

#else
    typedef YYSTYPE semantic_type;
#endif
    /// Symbol locations.
    typedef location location_type;

    /// Syntax errors thrown from user actions.
    struct syntax_error : std::runtime_error
    {
      syntax_error (const location_type& l, const std::string& m)
        : std::runtime_error (m)
        , location (l)
      {}

      syntax_error (const syntax_error& s)
        : std::runtime_error (s.what ())
        , location (s.location)
      {}

      ~syntax_error () YY_NOEXCEPT YY_NOTHROW;

      location_type location;
    };

    /// Token kinds.
    struct token
    {
      enum token_kind_type
      {
        YYEMPTY = -2,
    END_OF_INPUT = 0,              // END_OF_INPUT
    YYerror = 256,                 // error
    YYUNDEF = 257,                 // "invalid token"
    IDENTIFIER = 258,              // IDENTIFIER
    STRING_LITERAL = 259,          // STRING_LITERAL
    DURATION_LITERAL = 260,        // DURATION_LITERAL
    NUMBER_LITERAL = 261,          // NUMBER_LITERAL
    SELECT = 262,                  // SELECT
    FROM = 263,                    // FROM
    WHERE = 264,                   // WHERE
    AND = 265,                     // AND
    IN = 266,                      // IN
    LIKE = 267,                    // LIKE
    BETWEEN = 268,                 // BETWEEN
    LIMIT = 269,                   // LIMIT
    PAGE = 270,                    // PAGE
    TOKEN = 271,                   // TOKEN
    SHOW = 272,                    // SHOW
    TABLES = 273,                  // TABLES
    DESCRIBE = 274,                // DESCRIBE
    EXPLAIN = 275,                 // EXPLAIN
    AS = 276,                      // AS
    INNER = 277,                   // INNER
    LEFT = 278,                    // LEFT
    OUTER = 279,                   // OUTER
    JOIN = 280,                    // JOIN
    ON = 281,                      // ON
    NOW = 282,                     // NOW
    PREFIX = 283,                  // PREFIX
    CONTAINS = 284,                // CONTAINS
    ORDER = 285,                   // ORDER
    BY = 286,                      // BY
    ASC = 287,                     // ASC
    DESC = 288,                    // DESC
    STAR = 289,                    // STAR
    COMMA = 290,                   // COMMA
    DOT = 291,                     // DOT
    LPAREN = 292,                  // LPAREN
    RPAREN = 293,                  // RPAREN
    PLUS = 294,                    // PLUS
    MINUS = 295,                   // MINUS
    EQ = 296,                      // EQ
    NEQ = 297,                     // NEQ
    LT = 298,                      // LT
    LTE = 299,                     // LTE
    GT = 300,                      // GT
    GTE = 301                      // GTE
      };
      /// Backward compatibility alias (Bison 3.6).
      typedef token_kind_type yytokentype;
    };

    /// Token kind, as returned by yylex.
    typedef token::yytokentype token_kind_type;

    /// Backward compatibility alias (Bison 3.6).
    typedef token_kind_type token_type;

    /// Symbol kinds.
    struct symbol_kind
    {
      enum symbol_kind_type
      {
        YYNTOKENS = 47, ///< Number of tokens.
        S_YYEMPTY = -2,
        S_YYEOF = 0,                             // END_OF_INPUT
        S_YYerror = 1,                           // error
        S_YYUNDEF = 2,                           // "invalid token"
        S_IDENTIFIER = 3,                        // IDENTIFIER
        S_STRING_LITERAL = 4,                    // STRING_LITERAL
        S_DURATION_LITERAL = 5,                  // DURATION_LITERAL
        S_NUMBER_LITERAL = 6,                    // NUMBER_LITERAL
        S_SELECT = 7,                            // SELECT
        S_FROM = 8,                              // FROM
        S_WHERE = 9,                             // WHERE
        S_AND = 10,                              // AND
        S_IN = 11,                               // IN
        S_LIKE = 12,                             // LIKE
        S_BETWEEN = 13,                          // BETWEEN
        S_LIMIT = 14,                            // LIMIT
        S_PAGE = 15,                             // PAGE
        S_TOKEN = 16,                            // TOKEN
        S_SHOW = 17,                             // SHOW
        S_TABLES = 18,                           // TABLES
        S_DESCRIBE = 19,                         // DESCRIBE
        S_EXPLAIN = 20,                          // EXPLAIN
        S_AS = 21,                               // AS
        S_INNER = 22,                            // INNER
        S_LEFT = 23,                             // LEFT
        S_OUTER = 24,                            // OUTER
        S_JOIN = 25,                             // JOIN
        S_ON = 26,                               // ON
        S_NOW = 27,                              // NOW
        S_PREFIX = 28,                           // PREFIX
        S_CONTAINS = 29,                         // CONTAINS
        S_ORDER = 30,                            // ORDER
        S_BY = 31,                               // BY
        S_ASC = 32,                              // ASC
        S_DESC = 33,                             // DESC
        S_STAR = 34,                             // STAR
        S_COMMA = 35,                            // COMMA
        S_DOT = 36,                              // DOT
        S_LPAREN = 37,                           // LPAREN
        S_RPAREN = 38,                           // RPAREN
        S_PLUS = 39,                             // PLUS
        S_MINUS = 40,                            // MINUS
        S_EQ = 41,                               // EQ
        S_NEQ = 42,                              // NEQ
        S_LT = 43,                               // LT
        S_LTE = 44,                              // LTE
        S_GT = 45,                               // GT
        S_GTE = 46,                              // GTE
        S_YYACCEPT = 47,                         // $accept
        S_input = 48,                            // input
        S_statement = 49,                        // statement
        S_select_stmt = 50,                      // select_stmt
        S_order_by_opt = 51,                     // order_by_opt
        S_order_by_list = 52,                    // order_by_list
        S_order_by_item = 53,                    // order_by_item
        S_select_list = 54,                      // select_list
        S_column_list = 55,                      // column_list
        S_table_ref = 56,                        // table_ref
        S_alias_opt = 57,                        // alias_opt
        S_join_clauses = 58,                     // join_clauses
        S_join_clause = 59,                      // join_clause
        S_where_opt = 60,                        // where_opt
        S_predicate_list = 61,                   // predicate_list
        S_predicate = 62,                        // predicate
        S_literal = 63,                          // literal
        S_now_literal = 64,                      // now_literal
        S_signed_duration = 65,                  // signed_duration
        S_column_ref = 66,                       // column_ref
        S_identifier_path = 67,                  // identifier_path
        S_limit_opt = 68,                        // limit_opt
        S_page_opt = 69                          // page_opt
      };
    };

    /// (Internal) symbol kind.
    typedef symbol_kind::symbol_kind_type symbol_kind_type;

    /// The number of tokens.
    static const symbol_kind_type YYNTOKENS = symbol_kind::YYNTOKENS;

    /// A complete symbol.
    ///
    /// Expects its Base type to provide access to the symbol kind
    /// via kind ().
    ///
    /// Provide access to semantic value and location.
    template <typename Base>
    struct basic_symbol : Base
    {
      /// Alias to Base.
      typedef Base super_type;

      /// Default constructor.
      basic_symbol ()
        : value ()
        , location ()
      {}

#if 201103L <= YY_CPLUSPLUS
      /// Move constructor.
      basic_symbol (basic_symbol&& that)
        : Base (std::move (that))
        , value ()
        , location (std::move (that.location))
      {
        switch (this->kind ())
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_duration: // signed_duration
        value.move< int64_t > (std::move (that.value));
        break;

      case symbol_kind::S_join_clause: // join_clause
        value.move< mldp_pvxs_driver::query::JoinClause > (std::move (that.value));
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        value.move< mldp_pvxs_driver::query::LiteralValue > (std::move (that.value));
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        value.move< mldp_pvxs_driver::query::OrderByItem > (std::move (that.value));
        break;

      case symbol_kind::S_column_ref: // column_ref
        value.move< mldp_pvxs_driver::query::QualifiedColumn > (std::move (that.value));
        break;

      case symbol_kind::S_statement: // statement
        value.move< mldp_pvxs_driver::query::QueryStatement > (std::move (that.value));
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        value.move< mldp_pvxs_driver::query::SelectStatement > (std::move (that.value));
        break;

      case symbol_kind::S_table_ref: // table_ref
        value.move< mldp_pvxs_driver::query::TableRef > (std::move (that.value));
        break;

      case symbol_kind::S_predicate: // predicate
        value.move< mldp_pvxs_driver::query::WherePredicate > (std::move (that.value));
        break;

      case symbol_kind::S_select_list: // select_list
        value.move< mldp_pvxs_driver::query::generated::SelectListValue > (std::move (that.value));
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        value.move< std::optional<std::string> > (std::move (that.value));
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        value.move< std::optional<uint64_t> > (std::move (that.value));
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        value.move< std::string > (std::move (that.value));
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        value.move< std::vector<mldp_pvxs_driver::query::JoinClause> > (std::move (that.value));
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.move< std::vector<mldp_pvxs_driver::query::OrderByItem> > (std::move (that.value));
        break;

      case symbol_kind::S_column_list: // column_list
        value.move< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > (std::move (that.value));
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        value.move< std::vector<mldp_pvxs_driver::query::WherePredicate> > (std::move (that.value));
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        value.move< std::vector<std::string> > (std::move (that.value));
        break;

      default:
        break;
    }

      }
#endif

      /// Copy constructor.
      basic_symbol (const basic_symbol& that);

      /// Constructors for typed symbols.
#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, location_type&& l)
        : Base (t)
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const location_type& l)
        : Base (t)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, int64_t&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const int64_t& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::JoinClause&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::JoinClause& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::LiteralValue&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::LiteralValue& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::OrderByItem&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::OrderByItem& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::QualifiedColumn&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::QualifiedColumn& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::QueryStatement&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::QueryStatement& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::SelectStatement&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::SelectStatement& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::TableRef&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::TableRef& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::WherePredicate&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::WherePredicate& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, mldp_pvxs_driver::query::generated::SelectListValue&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const mldp_pvxs_driver::query::generated::SelectListValue& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, std::optional<std::string>&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const std::optional<std::string>& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, std::optional<uint64_t>&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const std::optional<uint64_t>& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, std::string&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const std::string& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, std::vector<mldp_pvxs_driver::query::JoinClause>&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const std::vector<mldp_pvxs_driver::query::JoinClause>& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, std::vector<mldp_pvxs_driver::query::OrderByItem>&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const std::vector<mldp_pvxs_driver::query::OrderByItem>& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, std::vector<mldp_pvxs_driver::query::QualifiedColumn>&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const std::vector<mldp_pvxs_driver::query::QualifiedColumn>& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, std::vector<mldp_pvxs_driver::query::WherePredicate>&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const std::vector<mldp_pvxs_driver::query::WherePredicate>& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

#if 201103L <= YY_CPLUSPLUS
      basic_symbol (typename Base::kind_type t, std::vector<std::string>&& v, location_type&& l)
        : Base (t)
        , value (std::move (v))
        , location (std::move (l))
      {}
#else
      basic_symbol (typename Base::kind_type t, const std::vector<std::string>& v, const location_type& l)
        : Base (t)
        , value (v)
        , location (l)
      {}
#endif

      /// Destroy the symbol.
      ~basic_symbol ()
      {
        clear ();
      }

      /// Destroy contents, and record that is empty.
      void clear ()
      {
        // User destructor.
        symbol_kind_type yykind = this->kind ();
        basic_symbol<Base>& yysym = *this;
        (void) yysym;
        switch (yykind)
        {
       default:
          break;
        }

        // Value type destructor.
switch (yykind)
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_duration: // signed_duration
        value.template destroy< int64_t > ();
        break;

      case symbol_kind::S_join_clause: // join_clause
        value.template destroy< mldp_pvxs_driver::query::JoinClause > ();
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        value.template destroy< mldp_pvxs_driver::query::LiteralValue > ();
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        value.template destroy< mldp_pvxs_driver::query::OrderByItem > ();
        break;

      case symbol_kind::S_column_ref: // column_ref
        value.template destroy< mldp_pvxs_driver::query::QualifiedColumn > ();
        break;

      case symbol_kind::S_statement: // statement
        value.template destroy< mldp_pvxs_driver::query::QueryStatement > ();
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        value.template destroy< mldp_pvxs_driver::query::SelectStatement > ();
        break;

      case symbol_kind::S_table_ref: // table_ref
        value.template destroy< mldp_pvxs_driver::query::TableRef > ();
        break;

      case symbol_kind::S_predicate: // predicate
        value.template destroy< mldp_pvxs_driver::query::WherePredicate > ();
        break;

      case symbol_kind::S_select_list: // select_list
        value.template destroy< mldp_pvxs_driver::query::generated::SelectListValue > ();
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        value.template destroy< std::optional<std::string> > ();
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        value.template destroy< std::optional<uint64_t> > ();
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        value.template destroy< std::string > ();
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        value.template destroy< std::vector<mldp_pvxs_driver::query::JoinClause> > ();
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.template destroy< std::vector<mldp_pvxs_driver::query::OrderByItem> > ();
        break;

      case symbol_kind::S_column_list: // column_list
        value.template destroy< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > ();
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        value.template destroy< std::vector<mldp_pvxs_driver::query::WherePredicate> > ();
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        value.template destroy< std::vector<std::string> > ();
        break;

      default:
        break;
    }

        Base::clear ();
      }

      /// The user-facing name of this symbol.
      const char *name () const YY_NOEXCEPT
      {
        return QueryBisonParser::symbol_name (this->kind ());
      }

      /// Backward compatibility (Bison 3.6).
      symbol_kind_type type_get () const YY_NOEXCEPT;

      /// Whether empty.
      bool empty () const YY_NOEXCEPT;

      /// Destructive move, \a s is emptied into this.
      void move (basic_symbol& s);

      /// The semantic value.
      semantic_type value;

      /// The location.
      location_type location;

    private:
#if YY_CPLUSPLUS < 201103L
      /// Assignment operator.
      basic_symbol& operator= (const basic_symbol& that);
#endif
    };

    /// Type access provider for token (enum) based symbols.
    struct by_kind
    {
      /// Default constructor.
      by_kind ();

#if 201103L <= YY_CPLUSPLUS
      /// Move constructor.
      by_kind (by_kind&& that);
#endif

      /// Copy constructor.
      by_kind (const by_kind& that);

      /// The symbol kind as needed by the constructor.
      typedef token_kind_type kind_type;

      /// Constructor from (external) token numbers.
      by_kind (kind_type t);

      /// Record that this symbol is empty.
      void clear ();

      /// Steal the symbol kind from \a that.
      void move (by_kind& that);

      /// The (internal) type number (corresponding to \a type).
      /// \a empty when empty.
      symbol_kind_type kind () const YY_NOEXCEPT;

      /// Backward compatibility (Bison 3.6).
      symbol_kind_type type_get () const YY_NOEXCEPT;

      /// The symbol kind.
      /// \a S_YYEMPTY when empty.
      symbol_kind_type kind_;
    };

    /// Backward compatibility for a private implementation detail (Bison 3.6).
    typedef by_kind by_type;

    /// "External" symbols: returned by the scanner.
    struct symbol_type : basic_symbol<by_kind>
    {
      /// Superclass.
      typedef basic_symbol<by_kind> super_type;

      /// Empty symbol.
      symbol_type () {}

      /// Constructor for valueless symbols, and symbols from each type.
#if 201103L <= YY_CPLUSPLUS
      symbol_type (int tok, location_type l)
        : super_type(token_type (tok), std::move (l))
#else
      symbol_type (int tok, const location_type& l)
        : super_type(token_type (tok), l)
#endif
      {}
#if 201103L <= YY_CPLUSPLUS
      symbol_type (int tok, int64_t v, location_type l)
        : super_type(token_type (tok), std::move (v), std::move (l))
#else
      symbol_type (int tok, const int64_t& v, const location_type& l)
        : super_type(token_type (tok), v, l)
#endif
      {}
#if 201103L <= YY_CPLUSPLUS
      symbol_type (int tok, std::string v, location_type l)
        : super_type(token_type (tok), std::move (v), std::move (l))
#else
      symbol_type (int tok, const std::string& v, const location_type& l)
        : super_type(token_type (tok), v, l)
#endif
      {}
    };

    /// Build a parser object.
    QueryBisonParser (mldp_pvxs_driver::query::generated::ParseContext& ctx_yyarg);
    virtual ~QueryBisonParser ();

#if 201103L <= YY_CPLUSPLUS
    /// Non copyable.
    QueryBisonParser (const QueryBisonParser&) = delete;
    /// Non copyable.
    QueryBisonParser& operator= (const QueryBisonParser&) = delete;
#endif

    /// Parse.  An alias for parse ().
    /// \returns  0 iff parsing succeeded.
    int operator() ();

    /// Parse.
    /// \returns  0 iff parsing succeeded.
    virtual int parse ();

#if YYDEBUG
    /// The current debugging stream.
    std::ostream& debug_stream () const YY_ATTRIBUTE_PURE;
    /// Set the current debugging stream.
    void set_debug_stream (std::ostream &);

    /// Type for debugging levels.
    typedef int debug_level_type;
    /// The current debugging level.
    debug_level_type debug_level () const YY_ATTRIBUTE_PURE;
    /// Set the current debugging level.
    void set_debug_level (debug_level_type l);
#endif

    /// Report a syntax error.
    /// \param loc    where the syntax error is found.
    /// \param msg    a description of the syntax error.
    virtual void error (const location_type& loc, const std::string& msg);

    /// Report a syntax error.
    void error (const syntax_error& err);

    /// The user-facing name of the symbol whose (internal) number is
    /// YYSYMBOL.  No bounds checking.
    static const char *symbol_name (symbol_kind_type yysymbol);

    // Implementation of make_symbol for each symbol type.
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_END_OF_INPUT (location_type l)
      {
        return symbol_type (token::END_OF_INPUT, std::move (l));
      }
#else
      static
      symbol_type
      make_END_OF_INPUT (const location_type& l)
      {
        return symbol_type (token::END_OF_INPUT, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_YYerror (location_type l)
      {
        return symbol_type (token::YYerror, std::move (l));
      }
#else
      static
      symbol_type
      make_YYerror (const location_type& l)
      {
        return symbol_type (token::YYerror, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_YYUNDEF (location_type l)
      {
        return symbol_type (token::YYUNDEF, std::move (l));
      }
#else
      static
      symbol_type
      make_YYUNDEF (const location_type& l)
      {
        return symbol_type (token::YYUNDEF, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_IDENTIFIER (std::string v, location_type l)
      {
        return symbol_type (token::IDENTIFIER, std::move (v), std::move (l));
      }
#else
      static
      symbol_type
      make_IDENTIFIER (const std::string& v, const location_type& l)
      {
        return symbol_type (token::IDENTIFIER, v, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_STRING_LITERAL (std::string v, location_type l)
      {
        return symbol_type (token::STRING_LITERAL, std::move (v), std::move (l));
      }
#else
      static
      symbol_type
      make_STRING_LITERAL (const std::string& v, const location_type& l)
      {
        return symbol_type (token::STRING_LITERAL, v, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_DURATION_LITERAL (std::string v, location_type l)
      {
        return symbol_type (token::DURATION_LITERAL, std::move (v), std::move (l));
      }
#else
      static
      symbol_type
      make_DURATION_LITERAL (const std::string& v, const location_type& l)
      {
        return symbol_type (token::DURATION_LITERAL, v, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_NUMBER_LITERAL (int64_t v, location_type l)
      {
        return symbol_type (token::NUMBER_LITERAL, std::move (v), std::move (l));
      }
#else
      static
      symbol_type
      make_NUMBER_LITERAL (const int64_t& v, const location_type& l)
      {
        return symbol_type (token::NUMBER_LITERAL, v, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_SELECT (location_type l)
      {
        return symbol_type (token::SELECT, std::move (l));
      }
#else
      static
      symbol_type
      make_SELECT (const location_type& l)
      {
        return symbol_type (token::SELECT, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_FROM (location_type l)
      {
        return symbol_type (token::FROM, std::move (l));
      }
#else
      static
      symbol_type
      make_FROM (const location_type& l)
      {
        return symbol_type (token::FROM, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_WHERE (location_type l)
      {
        return symbol_type (token::WHERE, std::move (l));
      }
#else
      static
      symbol_type
      make_WHERE (const location_type& l)
      {
        return symbol_type (token::WHERE, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_AND (location_type l)
      {
        return symbol_type (token::AND, std::move (l));
      }
#else
      static
      symbol_type
      make_AND (const location_type& l)
      {
        return symbol_type (token::AND, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_IN (location_type l)
      {
        return symbol_type (token::IN, std::move (l));
      }
#else
      static
      symbol_type
      make_IN (const location_type& l)
      {
        return symbol_type (token::IN, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_LIKE (location_type l)
      {
        return symbol_type (token::LIKE, std::move (l));
      }
#else
      static
      symbol_type
      make_LIKE (const location_type& l)
      {
        return symbol_type (token::LIKE, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_BETWEEN (location_type l)
      {
        return symbol_type (token::BETWEEN, std::move (l));
      }
#else
      static
      symbol_type
      make_BETWEEN (const location_type& l)
      {
        return symbol_type (token::BETWEEN, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_LIMIT (location_type l)
      {
        return symbol_type (token::LIMIT, std::move (l));
      }
#else
      static
      symbol_type
      make_LIMIT (const location_type& l)
      {
        return symbol_type (token::LIMIT, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_PAGE (location_type l)
      {
        return symbol_type (token::PAGE, std::move (l));
      }
#else
      static
      symbol_type
      make_PAGE (const location_type& l)
      {
        return symbol_type (token::PAGE, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_TOKEN (location_type l)
      {
        return symbol_type (token::TOKEN, std::move (l));
      }
#else
      static
      symbol_type
      make_TOKEN (const location_type& l)
      {
        return symbol_type (token::TOKEN, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_SHOW (location_type l)
      {
        return symbol_type (token::SHOW, std::move (l));
      }
#else
      static
      symbol_type
      make_SHOW (const location_type& l)
      {
        return symbol_type (token::SHOW, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_TABLES (location_type l)
      {
        return symbol_type (token::TABLES, std::move (l));
      }
#else
      static
      symbol_type
      make_TABLES (const location_type& l)
      {
        return symbol_type (token::TABLES, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_DESCRIBE (location_type l)
      {
        return symbol_type (token::DESCRIBE, std::move (l));
      }
#else
      static
      symbol_type
      make_DESCRIBE (const location_type& l)
      {
        return symbol_type (token::DESCRIBE, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_EXPLAIN (location_type l)
      {
        return symbol_type (token::EXPLAIN, std::move (l));
      }
#else
      static
      symbol_type
      make_EXPLAIN (const location_type& l)
      {
        return symbol_type (token::EXPLAIN, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_AS (location_type l)
      {
        return symbol_type (token::AS, std::move (l));
      }
#else
      static
      symbol_type
      make_AS (const location_type& l)
      {
        return symbol_type (token::AS, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_INNER (location_type l)
      {
        return symbol_type (token::INNER, std::move (l));
      }
#else
      static
      symbol_type
      make_INNER (const location_type& l)
      {
        return symbol_type (token::INNER, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_LEFT (location_type l)
      {
        return symbol_type (token::LEFT, std::move (l));
      }
#else
      static
      symbol_type
      make_LEFT (const location_type& l)
      {
        return symbol_type (token::LEFT, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_OUTER (location_type l)
      {
        return symbol_type (token::OUTER, std::move (l));
      }
#else
      static
      symbol_type
      make_OUTER (const location_type& l)
      {
        return symbol_type (token::OUTER, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_JOIN (location_type l)
      {
        return symbol_type (token::JOIN, std::move (l));
      }
#else
      static
      symbol_type
      make_JOIN (const location_type& l)
      {
        return symbol_type (token::JOIN, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_ON (location_type l)
      {
        return symbol_type (token::ON, std::move (l));
      }
#else
      static
      symbol_type
      make_ON (const location_type& l)
      {
        return symbol_type (token::ON, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_NOW (location_type l)
      {
        return symbol_type (token::NOW, std::move (l));
      }
#else
      static
      symbol_type
      make_NOW (const location_type& l)
      {
        return symbol_type (token::NOW, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_PREFIX (location_type l)
      {
        return symbol_type (token::PREFIX, std::move (l));
      }
#else
      static
      symbol_type
      make_PREFIX (const location_type& l)
      {
        return symbol_type (token::PREFIX, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_CONTAINS (location_type l)
      {
        return symbol_type (token::CONTAINS, std::move (l));
      }
#else
      static
      symbol_type
      make_CONTAINS (const location_type& l)
      {
        return symbol_type (token::CONTAINS, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_ORDER (location_type l)
      {
        return symbol_type (token::ORDER, std::move (l));
      }
#else
      static
      symbol_type
      make_ORDER (const location_type& l)
      {
        return symbol_type (token::ORDER, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_BY (location_type l)
      {
        return symbol_type (token::BY, std::move (l));
      }
#else
      static
      symbol_type
      make_BY (const location_type& l)
      {
        return symbol_type (token::BY, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_ASC (location_type l)
      {
        return symbol_type (token::ASC, std::move (l));
      }
#else
      static
      symbol_type
      make_ASC (const location_type& l)
      {
        return symbol_type (token::ASC, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_DESC (location_type l)
      {
        return symbol_type (token::DESC, std::move (l));
      }
#else
      static
      symbol_type
      make_DESC (const location_type& l)
      {
        return symbol_type (token::DESC, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_STAR (location_type l)
      {
        return symbol_type (token::STAR, std::move (l));
      }
#else
      static
      symbol_type
      make_STAR (const location_type& l)
      {
        return symbol_type (token::STAR, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_COMMA (location_type l)
      {
        return symbol_type (token::COMMA, std::move (l));
      }
#else
      static
      symbol_type
      make_COMMA (const location_type& l)
      {
        return symbol_type (token::COMMA, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_DOT (location_type l)
      {
        return symbol_type (token::DOT, std::move (l));
      }
#else
      static
      symbol_type
      make_DOT (const location_type& l)
      {
        return symbol_type (token::DOT, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_LPAREN (location_type l)
      {
        return symbol_type (token::LPAREN, std::move (l));
      }
#else
      static
      symbol_type
      make_LPAREN (const location_type& l)
      {
        return symbol_type (token::LPAREN, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_RPAREN (location_type l)
      {
        return symbol_type (token::RPAREN, std::move (l));
      }
#else
      static
      symbol_type
      make_RPAREN (const location_type& l)
      {
        return symbol_type (token::RPAREN, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_PLUS (location_type l)
      {
        return symbol_type (token::PLUS, std::move (l));
      }
#else
      static
      symbol_type
      make_PLUS (const location_type& l)
      {
        return symbol_type (token::PLUS, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_MINUS (location_type l)
      {
        return symbol_type (token::MINUS, std::move (l));
      }
#else
      static
      symbol_type
      make_MINUS (const location_type& l)
      {
        return symbol_type (token::MINUS, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_EQ (location_type l)
      {
        return symbol_type (token::EQ, std::move (l));
      }
#else
      static
      symbol_type
      make_EQ (const location_type& l)
      {
        return symbol_type (token::EQ, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_NEQ (location_type l)
      {
        return symbol_type (token::NEQ, std::move (l));
      }
#else
      static
      symbol_type
      make_NEQ (const location_type& l)
      {
        return symbol_type (token::NEQ, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_LT (location_type l)
      {
        return symbol_type (token::LT, std::move (l));
      }
#else
      static
      symbol_type
      make_LT (const location_type& l)
      {
        return symbol_type (token::LT, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_LTE (location_type l)
      {
        return symbol_type (token::LTE, std::move (l));
      }
#else
      static
      symbol_type
      make_LTE (const location_type& l)
      {
        return symbol_type (token::LTE, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_GT (location_type l)
      {
        return symbol_type (token::GT, std::move (l));
      }
#else
      static
      symbol_type
      make_GT (const location_type& l)
      {
        return symbol_type (token::GT, l);
      }
#endif
#if 201103L <= YY_CPLUSPLUS
      static
      symbol_type
      make_GTE (location_type l)
      {
        return symbol_type (token::GTE, std::move (l));
      }
#else
      static
      symbol_type
      make_GTE (const location_type& l)
      {
        return symbol_type (token::GTE, l);
      }
#endif


    class context
    {
    public:
      context (const QueryBisonParser& yyparser, const symbol_type& yyla);
      const symbol_type& lookahead () const { return yyla_; }
      symbol_kind_type token () const { return yyla_.kind (); }
      const location_type& location () const { return yyla_.location; }

      /// Put in YYARG at most YYARGN of the expected tokens, and return the
      /// number of tokens stored in YYARG.  If YYARG is null, return the
      /// number of expected tokens (guaranteed to be less than YYNTOKENS).
      int expected_tokens (symbol_kind_type yyarg[], int yyargn) const;

    private:
      const QueryBisonParser& yyparser_;
      const symbol_type& yyla_;
    };

  private:
#if YY_CPLUSPLUS < 201103L
    /// Non copyable.
    QueryBisonParser (const QueryBisonParser&);
    /// Non copyable.
    QueryBisonParser& operator= (const QueryBisonParser&);
#endif


    /// Stored state numbers (used for stacks).
    typedef signed char state_type;

    /// The arguments of the error message.
    int yy_syntax_error_arguments_ (const context& yyctx,
                                    symbol_kind_type yyarg[], int yyargn) const;

    /// Generate an error message.
    /// \param yyctx     the context in which the error occurred.
    virtual std::string yysyntax_error_ (const context& yyctx) const;
    /// Compute post-reduction state.
    /// \param yystate   the current state
    /// \param yysym     the nonterminal to push on the stack
    static state_type yy_lr_goto_state_ (state_type yystate, int yysym);

    /// Whether the given \c yypact_ value indicates a defaulted state.
    /// \param yyvalue   the value to check
    static bool yy_pact_value_is_default_ (int yyvalue);

    /// Whether the given \c yytable_ value indicates a syntax error.
    /// \param yyvalue   the value to check
    static bool yy_table_value_is_error_ (int yyvalue);

    static const signed char yypact_ninf_;
    static const signed char yytable_ninf_;

    /// Convert a scanner token kind \a t to a symbol kind.
    /// In theory \a t should be a token_kind_type, but character literals
    /// are valid, yet not members of the token_type enum.
    static symbol_kind_type yytranslate_ (int t);



    // Tables.
    // YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
    // STATE-NUM.
    static const signed char yypact_[];

    // YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
    // Performed when YYTABLE does not specify something else to do.  Zero
    // means the default is an error.
    static const signed char yydefact_[];

    // YYPGOTO[NTERM-NUM].
    static const signed char yypgoto_[];

    // YYDEFGOTO[NTERM-NUM].
    static const signed char yydefgoto_[];

    // YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
    // positive, shift that token.  If negative, reduce the rule whose
    // number is the opposite.  If YYTABLE_NINF, syntax error.
    static const signed char yytable_[];

    static const signed char yycheck_[];

    // YYSTOS[STATE-NUM] -- The (internal number of the) accessing
    // symbol of state STATE-NUM.
    static const signed char yystos_[];

    // YYR1[YYN] -- Symbol number of symbol that rule YYN derives.
    static const signed char yyr1_[];

    // YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.
    static const signed char yyr2_[];


#if YYDEBUG
    // YYRLINE[YYN] -- Source line where rule number YYN was defined.
    static const short yyrline_[];
    /// Report on the debug stream that the rule \a r is going to be reduced.
    virtual void yy_reduce_print_ (int r) const;
    /// Print the state stack on the debug stream.
    virtual void yy_stack_print_ () const;

    /// Debugging level.
    int yydebug_;
    /// Debug stream.
    std::ostream* yycdebug_;

    /// \brief Display a symbol kind, value and location.
    /// \param yyo    The output stream.
    /// \param yysym  The symbol.
    template <typename Base>
    void yy_print_ (std::ostream& yyo, const basic_symbol<Base>& yysym) const;
#endif

    /// \brief Reclaim the memory associated to a symbol.
    /// \param yymsg     Why this token is reclaimed.
    ///                  If null, print nothing.
    /// \param yysym     The symbol.
    template <typename Base>
    void yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const;

  private:
    /// Type access provider for state based symbols.
    struct by_state
    {
      /// Default constructor.
      by_state () YY_NOEXCEPT;

      /// The symbol kind as needed by the constructor.
      typedef state_type kind_type;

      /// Constructor.
      by_state (kind_type s) YY_NOEXCEPT;

      /// Copy constructor.
      by_state (const by_state& that) YY_NOEXCEPT;

      /// Record that this symbol is empty.
      void clear () YY_NOEXCEPT;

      /// Steal the symbol kind from \a that.
      void move (by_state& that);

      /// The symbol kind (corresponding to \a state).
      /// \a symbol_kind::S_YYEMPTY when empty.
      symbol_kind_type kind () const YY_NOEXCEPT;

      /// The state number used to denote an empty symbol.
      /// We use the initial state, as it does not have a value.
      enum { empty_state = 0 };

      /// The state.
      /// \a empty when empty.
      state_type state;
    };

    /// "Internal" symbol: element of the stack.
    struct stack_symbol_type : basic_symbol<by_state>
    {
      /// Superclass.
      typedef basic_symbol<by_state> super_type;
      /// Construct an empty symbol.
      stack_symbol_type ();
      /// Move or copy construction.
      stack_symbol_type (YY_RVREF (stack_symbol_type) that);
      /// Steal the contents from \a sym to build this.
      stack_symbol_type (state_type s, YY_MOVE_REF (symbol_type) sym);
#if YY_CPLUSPLUS < 201103L
      /// Assignment, needed by push_back by some old implementations.
      /// Moves the contents of that.
      stack_symbol_type& operator= (stack_symbol_type& that);

      /// Assignment, needed by push_back by other implementations.
      /// Needed by some other old implementations.
      stack_symbol_type& operator= (const stack_symbol_type& that);
#endif
    };

    /// A stack with random access from its top.
    template <typename T, typename S = std::vector<T> >
    class stack
    {
    public:
      // Hide our reversed order.
      typedef typename S::iterator iterator;
      typedef typename S::const_iterator const_iterator;
      typedef typename S::size_type size_type;
      typedef typename std::ptrdiff_t index_type;

      stack (size_type n = 200)
        : seq_ (n)
      {}

#if 201103L <= YY_CPLUSPLUS
      /// Non copyable.
      stack (const stack&) = delete;
      /// Non copyable.
      stack& operator= (const stack&) = delete;
#endif

      /// Random access.
      ///
      /// Index 0 returns the topmost element.
      const T&
      operator[] (index_type i) const
      {
        return seq_[size_type (size () - 1 - i)];
      }

      /// Random access.
      ///
      /// Index 0 returns the topmost element.
      T&
      operator[] (index_type i)
      {
        return seq_[size_type (size () - 1 - i)];
      }

      /// Steal the contents of \a t.
      ///
      /// Close to move-semantics.
      void
      push (YY_MOVE_REF (T) t)
      {
        seq_.push_back (T ());
        operator[] (0).move (t);
      }

      /// Pop elements from the stack.
      void
      pop (std::ptrdiff_t n = 1) YY_NOEXCEPT
      {
        for (; 0 < n; --n)
          seq_.pop_back ();
      }

      /// Pop all elements from the stack.
      void
      clear () YY_NOEXCEPT
      {
        seq_.clear ();
      }

      /// Number of elements on the stack.
      index_type
      size () const YY_NOEXCEPT
      {
        return index_type (seq_.size ());
      }

      /// Iterator on top of the stack (going downwards).
      const_iterator
      begin () const YY_NOEXCEPT
      {
        return seq_.begin ();
      }

      /// Bottom of the stack.
      const_iterator
      end () const YY_NOEXCEPT
      {
        return seq_.end ();
      }

      /// Present a slice of the top of a stack.
      class slice
      {
      public:
        slice (const stack& stack, index_type range)
          : stack_ (stack)
          , range_ (range)
        {}

        const T&
        operator[] (index_type i) const
        {
          return stack_[range_ - i];
        }

      private:
        const stack& stack_;
        index_type range_;
      };

    private:
#if YY_CPLUSPLUS < 201103L
      /// Non copyable.
      stack (const stack&);
      /// Non copyable.
      stack& operator= (const stack&);
#endif
      /// The wrapped container.
      S seq_;
    };


    /// Stack type.
    typedef stack<stack_symbol_type> stack_type;

    /// The stack.
    stack_type yystack_;

    /// Push a new state on the stack.
    /// \param m    a debug message to display
    ///             if null, no trace is output.
    /// \param sym  the symbol
    /// \warning the contents of \a s.value is stolen.
    void yypush_ (const char* m, YY_MOVE_REF (stack_symbol_type) sym);

    /// Push a new look ahead token on the state on the stack.
    /// \param m    a debug message to display
    ///             if null, no trace is output.
    /// \param s    the state
    /// \param sym  the symbol (for its value and location).
    /// \warning the contents of \a sym.value is stolen.
    void yypush_ (const char* m, state_type s, YY_MOVE_REF (symbol_type) sym);

    /// Pop \a n symbols from the stack.
    void yypop_ (int n = 1);

    /// Constants.
    enum
    {
      yylast_ = 116,     ///< Last index in yytable_.
      yynnts_ = 23,  ///< Number of nonterminal symbols.
      yyfinal_ = 17 ///< Termination state number.
    };


    // User arguments.
    mldp_pvxs_driver::query::generated::ParseContext& ctx;

  };

  inline
  QueryBisonParser::symbol_kind_type
  QueryBisonParser::yytranslate_ (int t)
  {
    // YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to
    // TOKEN-NUM as returned by yylex.
    static
    const signed char
    translate_table[] =
    {
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46
    };
    // Last valid token kind.
    const int code_max = 301;

    if (t <= 0)
      return symbol_kind::S_YYEOF;
    else if (t <= code_max)
      return YY_CAST (symbol_kind_type, translate_table[t]);
    else
      return symbol_kind::S_YYUNDEF;
  }

  // basic_symbol.
  template <typename Base>
  QueryBisonParser::basic_symbol<Base>::basic_symbol (const basic_symbol& that)
    : Base (that)
    , value ()
    , location (that.location)
  {
    switch (this->kind ())
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_duration: // signed_duration
        value.copy< int64_t > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_join_clause: // join_clause
        value.copy< mldp_pvxs_driver::query::JoinClause > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        value.copy< mldp_pvxs_driver::query::LiteralValue > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        value.copy< mldp_pvxs_driver::query::OrderByItem > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_column_ref: // column_ref
        value.copy< mldp_pvxs_driver::query::QualifiedColumn > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_statement: // statement
        value.copy< mldp_pvxs_driver::query::QueryStatement > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        value.copy< mldp_pvxs_driver::query::SelectStatement > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_table_ref: // table_ref
        value.copy< mldp_pvxs_driver::query::TableRef > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_predicate: // predicate
        value.copy< mldp_pvxs_driver::query::WherePredicate > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_select_list: // select_list
        value.copy< mldp_pvxs_driver::query::generated::SelectListValue > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        value.copy< std::optional<std::string> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        value.copy< std::optional<uint64_t> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        value.copy< std::string > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        value.copy< std::vector<mldp_pvxs_driver::query::JoinClause> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.copy< std::vector<mldp_pvxs_driver::query::OrderByItem> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_column_list: // column_list
        value.copy< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        value.copy< std::vector<mldp_pvxs_driver::query::WherePredicate> > (YY_MOVE (that.value));
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        value.copy< std::vector<std::string> > (YY_MOVE (that.value));
        break;

      default:
        break;
    }

  }



  template <typename Base>
  QueryBisonParser::symbol_kind_type
  QueryBisonParser::basic_symbol<Base>::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }

  template <typename Base>
  bool
  QueryBisonParser::basic_symbol<Base>::empty () const YY_NOEXCEPT
  {
    return this->kind () == symbol_kind::S_YYEMPTY;
  }

  template <typename Base>
  void
  QueryBisonParser::basic_symbol<Base>::move (basic_symbol& s)
  {
    super_type::move (s);
    switch (this->kind ())
    {
      case symbol_kind::S_NUMBER_LITERAL: // NUMBER_LITERAL
      case symbol_kind::S_signed_duration: // signed_duration
        value.move< int64_t > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_join_clause: // join_clause
        value.move< mldp_pvxs_driver::query::JoinClause > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_literal: // literal
      case symbol_kind::S_now_literal: // now_literal
        value.move< mldp_pvxs_driver::query::LiteralValue > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_order_by_item: // order_by_item
        value.move< mldp_pvxs_driver::query::OrderByItem > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_column_ref: // column_ref
        value.move< mldp_pvxs_driver::query::QualifiedColumn > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_statement: // statement
        value.move< mldp_pvxs_driver::query::QueryStatement > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_select_stmt: // select_stmt
        value.move< mldp_pvxs_driver::query::SelectStatement > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_table_ref: // table_ref
        value.move< mldp_pvxs_driver::query::TableRef > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_predicate: // predicate
        value.move< mldp_pvxs_driver::query::WherePredicate > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_select_list: // select_list
        value.move< mldp_pvxs_driver::query::generated::SelectListValue > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_alias_opt: // alias_opt
      case symbol_kind::S_page_opt: // page_opt
        value.move< std::optional<std::string> > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_limit_opt: // limit_opt
        value.move< std::optional<uint64_t> > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_IDENTIFIER: // IDENTIFIER
      case symbol_kind::S_STRING_LITERAL: // STRING_LITERAL
      case symbol_kind::S_DURATION_LITERAL: // DURATION_LITERAL
        value.move< std::string > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_join_clauses: // join_clauses
        value.move< std::vector<mldp_pvxs_driver::query::JoinClause> > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_order_by_opt: // order_by_opt
      case symbol_kind::S_order_by_list: // order_by_list
        value.move< std::vector<mldp_pvxs_driver::query::OrderByItem> > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_column_list: // column_list
        value.move< std::vector<mldp_pvxs_driver::query::QualifiedColumn> > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_where_opt: // where_opt
      case symbol_kind::S_predicate_list: // predicate_list
        value.move< std::vector<mldp_pvxs_driver::query::WherePredicate> > (YY_MOVE (s.value));
        break;

      case symbol_kind::S_identifier_path: // identifier_path
        value.move< std::vector<std::string> > (YY_MOVE (s.value));
        break;

      default:
        break;
    }

    location = YY_MOVE (s.location);
  }

  // by_kind.
  inline
  QueryBisonParser::by_kind::by_kind ()
    : kind_ (symbol_kind::S_YYEMPTY)
  {}

#if 201103L <= YY_CPLUSPLUS
  inline
  QueryBisonParser::by_kind::by_kind (by_kind&& that)
    : kind_ (that.kind_)
  {
    that.clear ();
  }
#endif

  inline
  QueryBisonParser::by_kind::by_kind (const by_kind& that)
    : kind_ (that.kind_)
  {}

  inline
  QueryBisonParser::by_kind::by_kind (token_kind_type t)
    : kind_ (yytranslate_ (t))
  {}

  inline
  void
  QueryBisonParser::by_kind::clear ()
  {
    kind_ = symbol_kind::S_YYEMPTY;
  }

  inline
  void
  QueryBisonParser::by_kind::move (by_kind& that)
  {
    kind_ = that.kind_;
    that.clear ();
  }

  inline
  QueryBisonParser::symbol_kind_type
  QueryBisonParser::by_kind::kind () const YY_NOEXCEPT
  {
    return kind_;
  }

  inline
  QueryBisonParser::symbol_kind_type
  QueryBisonParser::by_kind::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }

#line 5 "src/query/parser/grammar/QueryBisonParser.y"
} } } // mldp_pvxs_driver::query::generated
#line 2632 "src/query/parser/generated/QueryBisonParser.hpp"




#endif // !YY_YY_SRC_QUERY_PARSER_GENERATED_QUERYBISONPARSER_HPP_INCLUDED
