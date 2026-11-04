# Phase 1 design: SQL window functions (`OVER`)

Status: Phase 1 implemented. Differences from this draft:
- `WINDOW`, `ROWS`, `RANGE`, `ROW`, `CURRENT`, `PARTITION`, `PRECEDING` and `FOLLOWING` are still accepted as
  column names, so `window IN (...)` keeps working. `OVER` and `UNBOUNDED` are reserved.
- Frame aggregates use their own sliding add/remove state (a monotonic deque for MIN/MAX) instead of
  `IAggregateAccumulator`, because that interface has no remove.
- `sorted_input=true` is a hint, not a guarantee: the executor checks the per-PV time order and does a full
  sort if the rows are out of order. No sortedness metadata exists on the backend.
- Window `SUM` returns int64 for integer arguments and double otherwise. The native `value` column always
  gives double.

## Goal
Let ML engineers compute per-row values over neighbouring rows (previous/next value,
running and moving aggregates, ranks) in the query REPL, as standard SQL:

```sql
SELECT pv, time, value,
       value - LAG(value) OVER w                                        AS delta,
       AVG(value) OVER (PARTITION BY pv ORDER BY time
                        RANGE BETWEEN 5m PRECEDING AND CURRENT ROW)     AS avg_5m,
       ROW_NUMBER() OVER (PARTITION BY pv ORDER BY time DESC)           AS rn
FROM mldp.time_series
WHERE pv PREFIX 'ltu:' AND time >= NOW - 1h
WINDOW w AS (PARTITION BY pv ORDER BY time);
```

## 1. Syntax (`src/query/parser/grammar/QueryBisonParser.y`)
New tokens: `OVER PARTITION WINDOW ROWS RANGE UNBOUNDED PRECEDING FOLLOWING CURRENT ROW`.

```
window_call   : IDENTIFIER LPAREN args_opt RPAREN OVER window_ref
window_ref    : IDENTIFIER                        -- named, from WINDOW clause
              | LPAREN window_spec RPAREN
window_spec   : [base_name] [PARTITION BY expr_list] [ORDER BY sort_list] [frame]
frame         : (ROWS | RANGE) BETWEEN bound AND bound
              | (ROWS | RANGE) bound              -- shorthand: bound AND CURRENT ROW
bound         : UNBOUNDED PRECEDING | UNBOUNDED FOLLOWING | CURRENT ROW
              | expr PRECEDING | expr FOLLOWING   -- int for ROWS; int or duration for RANGE
select_stmt   : ... having_opt window_clause_opt order_by_opt limit_opt
window_clause : WINDOW IDENTIFIER AS LPAREN window_spec RPAREN [, ...]
```

Window calls are allowed only in the select list and `ORDER BY` (as in standard SQL);
in `WHERE`, `GROUP BY` or `HAVING` they are a bind error. Filtering on a window result
uses a derived table: `SELECT * FROM (SELECT ..., ROW_NUMBER() OVER (...) rn FROM t) x WHERE rn = 1`.

AST (`include/query/parser/QueryAST.h`): new `WindowCall` alternative in `ExpressionValue`
holding function name, arguments and a `WindowSpec { optional base; partition_by; order_by; optional Frame }`;
`SelectStatement` gets `named_windows`.

Default frame (standard SQL): with `ORDER BY`, `RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW`
(peers included); without `ORDER BY`, the whole partition.

## 2. Functions
Registered in a new `WindowFunctionRegistry` next to `AggregateRegistry`, so `SHOW FUNCTIONS` lists them.

| Kind | Functions | Frame used |
|---|---|---|
| Ranking | `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()` | ignored |
| Offset | `LAG(x [, n [, default]])`, `LEAD(x [, n [, default]])` | ignored |
| Value | `FIRST_VALUE(x)`, `LAST_VALUE(x)` | yes |
| Aggregate | `SUM`, `AVG`, `MIN`, `MAX`, `COUNT(*\|x)` | yes; reuse existing `IAggregateFunction` accumulators |

`COUNT(DISTINCT x) OVER` and `FIRST`/`LAST` aggregates over frames are out of scope (bind error).

## 3. Planning
- **Binder** (`src/query/planner/Binder.cpp`): resolve named windows; bind partition, order and
  argument expressions against the input (or aggregate output when the query has `GROUP BY`);
  validate frame (`RANGE` with offset needs exactly one `ORDER BY` key, numeric or timestamp;
  duration offset only for timestamp keys). Each window call is replaced by a column reference
  `__win_<n>`, exactly as `AggregateRewriter` does for aggregates.
- **Logical plan** (`include/query/plan/LogicalPlan.h`): new `LogicalWindow { input; std::vector<WindowGroup> }`.
  Calls with identical `PARTITION BY` + `ORDER BY` share one `WindowGroup` (one sort, many outputs).
- **Placement** (`LogicalPlanner.cpp`): `scan → filter → join → post-join conditions → aggregate → WINDOW → sort → project → limit`.
- **Column pruning / pushdown:** window inputs are added to required columns; predicates are never
  pushed past a window node. `LIMIT` above a window is not pushed below it.
- **Physical plan:** `PhysicalWindow`; `EXPLAIN` prints `PhysicalWindow(groups=…, partition=…, order=…, sorted_input=…)`.

## 4. Execution (`src/query/executor/relational/WindowExecutionState.cpp`, `WindowRecordBatchStream.cpp`)
Per window group:
1. **Sort** by partition keys then order keys, reusing `applySort`. Skipped when input is already
   ordered (see 5).
2. **Partition scan:** walk rows; at each partition boundary compute outputs for that partition.
3. **Evaluation:**
   - ranking/offset: single pass, O(n).
   - frame aggregates: two-pointer sliding frame; `SUM/COUNT/AVG` add/remove incrementally, `MIN/MAX`
     use a monotonic deque. O(n) per partition for `ROWS` and `RANGE`.
   - `RANGE` peers (equal order key) share the same frame end, per standard.
4. Append `__win_<n>` columns with Arrow builders; output keeps input row order of the sorted stream.

**Streaming:** output is produced one partition at a time, so memory is bounded by the largest
partition, not the whole result.
**Spill:** when sorted input exceeds `memory_limit_bytes`, sort runs spill through `SpillManager`
(same mechanism as `GroupedAggregator`) and are merged; a partition that alone exceeds the limit
fails with a clear error in v1.
**Cancellation:** `throwIfCancelled()` per batch and per partition. Progress activity `"window"`.

## 5. MLDP-specific optimisations
- `mldp.time_series` returns rows ordered by `time` per PV shard. When the window is exactly
  `PARTITION BY pv ORDER BY time [ASC]`, the planner marks `sorted_input=true` and only a k-way
  merge by `pv` is needed (no full sort).
- `mldp.time_series_table` (wide, pivoted): window functions run on the wide rows ordered by `time`
  (`LAG("BPM:X") OVER (ORDER BY time)` works per column). Per-PV pre-pivot evaluation is Phase 4.
- Look-back pre-fetch widening (`RANGE 5m PRECEDING` → fetch 5 min earlier) is Phase 4; v1 frames
  at the start of the window simply see fewer rows.

## 6. REPL / docs
- Keywords and functions in highlighter and completion (`replHighlight`, `replCompletions`).
- New `.help timeseries` topic with examples above.
- `docs/guides/query-cli.md`: window functions section (syntax, frames, defaults, limits).

## 7. Tests
- **Parser** (`query_parser_test.cpp`): every syntax form, named windows + override, shorthand
  frames, errors (window call in `WHERE`, unknown window name, `RANGE` offset with two order keys).
- **Executor** (`query_planner_executor_test.cpp`, new `query_window_test.cpp`): each function vs
  hand-computed values; ties for `RANK`/`DENSE_RANK`/`RANGE` peers; NULLs in order and value
  columns; empty and single-row partitions; `LAG` default; duration `RANGE` frames;
  window over `GROUP BY` output; multiple groups in one query.
- **Spill:** large input with tiny memory budget gives identical results.
- **Integration** (`queryable_mldp_integration_test.cpp`): `LAG`/`AVG OVER` on seeded series;
  `sorted_input=true` path verified via `EXPLAIN`.
- **Benchmark (disabled):** 10M rows, 100 PVs, `LAG` + `AVG` 5m frame.

## 8. Out of scope for Phase 1
`GROUPS` frames, `EXCLUDE`, `NTILE`/`PERCENT_RANK`/`CUME_DIST`, `NTH_VALUE`, `IGNORE NULLS`,
window calls inside `WHERE`/`HAVING`, fill/interpolation.

## Next phases (already agreed)
2. `UNION ALL`/`UNION`, `WITH` (CTEs).
3. `free_intervals(start, end, config_name)` (same `config_name` never counts as overlap) and `time_bucket(time, dur)` (no fill).
4. Look-back pre-fetch widening, per-PV windows on `time_series_table`, `ASOF JOIN`, `merge_intervals`.
