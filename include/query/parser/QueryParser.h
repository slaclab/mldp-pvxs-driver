//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/parser/QueryAST.h>
#include <query/parser/Token.h>

#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

class QueryParser
{
public:
    explicit QueryParser(std::vector<Token> tokens);

    QueryStatement parse();
    static QueryStatement parse(std::string_view sql);

private:
    std::vector<Token> tokens_;
    std::size_t        index_{0};

    [[nodiscard]] const Token& current() const;
    [[nodiscard]] const Token& previous() const;
    [[nodiscard]] bool atEnd() const;
    bool               match(TokenType type);
    const Token&       consume(TokenType type, std::string_view expected);
    [[nodiscard]] bool isStartOfValue(TokenType type) const;
    [[nodiscard]] bool canStartIdentifierPath(TokenType type) const;
    [[nodiscard]] bool isAliasCandidate(TokenType type) const;

    QueryStatement      parseStatement();
    ShowTablesStatement parseShowTables();
    DescribeStatement   parseDescribe();
    ExplainStatement    parseExplain();
    SelectStatement     parseSelect();

    std::vector<QualifiedColumn> parseSelectList();
    TableRef                     parseTableRef();
    std::vector<JoinClause>      parseJoinClauses();
    JoinClause                   parseJoinClause();
    JoinCondition                parseJoinCondition();
    std::vector<WherePredicate>  parseWhereClause();
    WherePredicate               parsePredicate();
    LiteralValue                 parseValue();
    QualifiedColumn              parseColumnReference();
    std::vector<std::string>     parseIdentifierPath();
};

QueryStatement parseQuery(std::string_view sql);

} // namespace mldp_pvxs_driver::query
