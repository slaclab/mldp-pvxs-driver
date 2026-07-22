# Phase 2 — Parser

← [Back to main plan](query-client-impl.md)

## Tasks

- [ ] `src/cli/query/Token.h` — token types: `SELECT`, `FROM`, `WHERE`, `AND`, `IN`, `LIKE`, `BETWEEN`, `LIMIT`, `PAGE`, `TOKEN`, `SHOW`, `DESCRIBE`, identifiers, literals, operators
- [ ] `src/cli/query/Lexer.h/.cpp` — hand-written tokeniser
- [ ] `src/cli/query/QueryAST.h` — AST nodes: `SelectStatement`, `ShowTablesStatement`, `DescribeStatement`, `WherePredicate` variants (`EqPredicate`, `InPredicate`, `RangePredicate`, `OpPredicate`)
- [ ] `src/cli/query/QueryParser.h/.cpp` — recursive-descent parser; produces AST or `ParseError`

## Notes

- No external SQL parser dependency — grammar is simple enough for a hand-written recursive descent
- `ParseError` must carry position (line/col or char offset) for actionable user messages
- Grammar supports only `AND` at top level; `OR` expressed via `IN (...)`
- `NOW` and `NOW±duration` are parsed as literals; epoch folding happens in TypeChecker (Phase 3)
