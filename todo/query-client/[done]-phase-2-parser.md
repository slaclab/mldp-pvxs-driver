# Phase 2 — Parser

← [Back to main plan](query-client-impl.md)

## Goal

Implement a hand-written SQL subset parser that produces a typed AST for planner input, with precise errors and no external SQL parser dependency.

## Scope and Constraints

- Grammar is a strict subset of SQL `SELECT` with:
  - `SELECT ... FROM ...`
  - optional `JOIN` chain (`INNER`, `LEFT [OUTER]`, equi-join only)
  - optional `WHERE` (`AND` only at top-level)
  - optional `LIMIT`
  - optional `PAGE TOKEN`
- Meta statements:
  - `SHOW TABLES`
  - `DESCRIBE <table>`
  - `EXPLAIN <select_query>`
- No subqueries, DDL, `GROUP BY`, `ORDER BY`, `HAVING`, `UNION`, `RIGHT/FULL JOIN`.
- `NOW` and `NOW±duration` are parsed as literals; resolution/folding happens in TypeChecker (Phase 3).

## Inputs and Outputs

- **Input:** SQL text from `query` CLI positional argument or `--file`.
- **Output:** `QueryAST` statement variant (`SelectStatement`, `ShowTablesStatement`, `DescribeStatement`, `ExplainStatement`) or `ParseError`.
- **Error contract:** Parse errors always include position metadata (line/column or absolute offset) and expected token context.

## Tasks

- [ ] `src/cli/query/Token.h` — token types: `SELECT`, `FROM`, `WHERE`, `AND`, `IN`, `LIKE`, `BETWEEN`, `LIMIT`, `PAGE`, `TOKEN`, `SHOW`, `DESCRIBE`, identifiers, literals, operators
- [ ] `src/cli/query/Lexer.h/.cpp` — hand-written tokeniser
- [ ] `src/cli/query/QueryAST.h` — AST nodes: `SelectStatement`, `ShowTablesStatement`, `DescribeStatement`, `ExplainStatement`, `JoinClause`, `QualifiedColumn`, and predicate variants (`EqPredicate`, `InPredicate`, `RangePredicate`, `OpPredicate`)
- [ ] `src/cli/query/QueryParser.h/.cpp` — recursive-descent parser; produces AST or `ParseError`

## AST Requirements

- `table_ref` supports table aliases (`FROM mldp.time_series AS ts` and `FROM ... ts`).
- Join chain is represented as ordered `JoinClause` entries (left-deep planning in Phase 3/3b).
- Join condition supports only `qualified_column = qualified_column`.
- Qualified/unqualified column forms are both preserved in AST; binder resolves ambiguity later.
- Predicate literals support:
  - string
  - number
  - keyword literals (`NOW`)
  - duration offsets (`NOW-60s`, `NOW+5m`, `NOW-1h`)

## Tokenization Rules

- Keywords are case-insensitive (`select`, `SELECT`, `SeLeCt`).
- Identifiers support dotted table names (`mldp.time_series`) and `attr.<key>` column patterns.
- String literals support quoted values used in `IN (...)` and simple comparisons.
- Unknown characters and unterminated strings produce immediate `ParseError` with position.

## Acceptance Criteria

- Parses representative queries for:
  - table scan + filters
  - `SHOW TABLES`, `DESCRIBE`, `EXPLAIN`
  - `INNER JOIN`, `LEFT JOIN`, multi-join chains
  - `IN`, `BETWEEN`, `LIKE`, comparison operators, `LIMIT`, `PAGE TOKEN`
- Fails malformed SQL with actionable error location and expected grammar fragment.

## Notes

- No external SQL parser dependency — grammar is simple enough for a hand-written recursive descent
- `ParseError` must carry position (line/col or char offset) for actionable user messages
- Grammar supports only `AND` at top level; `OR` expressed via `IN (...)`
- `NOW` and `NOW±duration` are parsed as literals; epoch folding happens in TypeChecker (Phase 3)
