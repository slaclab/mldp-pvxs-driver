# Fixed Result Viewport Query REPL

## Goal

Provide a real-TTY query workspace in which SQL results occupy a fixed,
independently scrollable viewport and the SQL editor remains visible at all
times. A large result table must never consume the terminal scrollback or move
the next command prompt out of view.

```text
+- Results #12  rows 1-18 / 4,832  Alt-Left/Right: result  Tab: editor -+
| time                      pv                         value             |
| 2026-07-31T17:03:...      BPMS:IN20:...              1.024             |
| 2026-07-31T17:03:...      BPMS:IN20:...              1.027             |
|                         Up/Down, PgUp/PgDn: scroll                    |
+- SQL editor  Tab: results  F5: run  Ctrl-C: cancel --------------------+
| SELECT time, pv, value                                                  |
| FROM mldp.time_series                                                    |
| WHERE ...                                                               |
+- Idle | 4,832 rows | 180 ms | result history: 36 MiB | Ctrl-Q: quit ---+
```

## Current problem and design boundary

`QuerySubcommand` is and remains the top-level query command, result-lifecycle,
and interactive-mode entry point. It currently uses `replxx` for interactive
line editing and `TerminalLayout` reserves a footer row using a DECSTBM terminal
scroll region.
`replxx` still owns the full terminal prompt layout, so independently moving
the physical cursor or reserving a scrolling region can desynchronize its
internal prompt origin. Previous attempts caused lost result visibility,
stalled scrolling, invisible Ctrl-C feedback, or a missing caret.

Do not add another ANSI terminal layer around `replxx`. The workspace must use
one terminal UI owner for the result viewport, SQL editor, status bar, resize,
and cursor placement. FTXUI is already fetched for the configuration wizard
and is the selected UI owner for real-TTY query sessions.

## Required ownership split

`QuerySubcommand` and the TUI client have deliberately separate jobs:

```text
QuerySubcommand (src/query/)
  owns: CLI options, query preparation, QueryRunner, continuations,
        execution/cancellation/statistics/error lifecycle, TTY selection
  calls: query-tui client only for an interactive TTY session

query-tui/ (new root-level client module)
  owns: FTXUI screen, editor, pane focus, key handling, viewport offsets,
        result selection, render state, and terminal restoration
  never owns: SQL parsing/planning/execution, gRPC clients, queryables,
              continuation registry, or the query command lifecycle
```

The TUI sends a SQL-submit intent to `QuerySubcommand` and receives immutable
progress, result, and error updates through a narrow interface. It does not
call `QueryRunner` directly. `QuerySubcommand` remains the sole authority that
starts, cancels, completes, or discards a query result.

## Scope

- Replace the interactive TTY `replxx` layout with an FTXUI query workspace.
- Create a dedicated root-level `query-tui/` client module. It owns only FTXUI
  presentation and interaction; it is not placed under `src/query` and it does
  not become a second query-command entry point.
- Keep the result area at roughly two-thirds of the terminal height and the
  editor at one-third, with minimum viable heights for each pane. Recompute
  the layout on terminal resize.
- Store every completed table or expanded result in a session-owned Arrow IPC
  snapshot, so every result has its own scroll position without retaining all
  rows in memory.
- Retain every completed result for the current session. Track aggregate
  snapshot bytes and show a non-destructive warning once storage exceeds
  1 GiB. Delete the complete session directory on normal exit and attempt
  cleanup on errors or cancellation.
- Preserve table and expanded rendering in the result pane. JSON, CSV, and
  Arrow output keep their current formatter behavior and do not enter the
  workspace result history.
- Preserve redirected input/output, one-shot SQL, and machine-readable output
  exactly as ANSI-free, non-workspace paths.

## Non-goals

- Do not create a terminal scroll region, a persistent ANSI footer, or any
  cursor-positioning wrapper around `replxx`.
- Do not persist result history across REPL restarts.
- Do not impose an eviction limit on result count or alter query semantics,
  result pagination, continuation tokens, query statistics, cancellation, or
  backend protocols.
- Do not add JSON/CSV browsing in this phase.

## User interaction contract

### Focus and keys

| Key | Behavior |
|---|---|
| `Tab` | Toggle focus between the result viewport and SQL editor. |
| `Up` / `Down`, `PageUp` / `PageDown`, `Home` / `End` | Scroll the focused result viewport; editor navigation retains normal editor semantics when it has focus. |
| `Alt-Left` / `Alt-Right` | Select previous or next completed result without changing its saved scroll position. |
| `F5` | Submit the complete SQL editor buffer. |
| `Ctrl-C` | Cancel a running query; when idle, clear the active editor buffer. |
| `Ctrl-Q` | Exit after restoring the terminal and cleaning session snapshots. |

The result header always identifies the selected result, visible row range,
total row count, and navigation hints. The status line always remains visible:
while running it shows progress and cancellation state; once complete it shows
the selected query's stats, errors, and total session snapshot usage.

### SQL editor compatibility

The FTXUI editor must be feature-compatible with the current REPL before it
becomes the normal TTY path:

- multi-line SQL editing and semicolon-based statement completion;
- persistent SQL history and history navigation;
- completion from the current query table catalog;
- familiar cursor/editing shortcuts and clear error recovery for incomplete
  statements;
- existing `.help`, `.clear`, `.format`, `.table-fit`, `.history`,
  `\\expanded`/`\\x`, `.quit`, and `.exit` behavior, adapted to workspace UI
  messages where necessary.

## Implementation plan

### 1. Separate interactive execution from presentation

1. Keep `QuerySubcommand::run()` as the sole `query` command entry point. It
   parses options, prepares queryables, detects simultaneous stdin/stdout TTYs,
   owns `QueryRunner` and `QueryContinuationRegistry`, and creates the TUI
   client only for the TTY branch. It remains responsible for query submission,
   result completion, cancellation, statistics, errors, and cleanup; it must
   not move query orchestration into the TUI module.
2. Define a narrow `query-tui` interaction contract:
   - TUI-to-`QuerySubcommand`: submit SQL, request cancellation, request exit,
     and UI-only result navigation state.
   - `QuerySubcommand`-to-TUI: immutable progress snapshots, completed result
     descriptors, query statistics, and query errors.
   - The contract contains no parser/planner/executor/gRPC types and does not
     expose `QueryRunner` or `QueryContinuationRegistry` to the TUI.
3. Retain `QueryRunner` as the execution, cancellation, statistics, and
   continuation implementation behind `QuerySubcommand`. It continues to own
   actual query work; the TUI only renders updates that `QuerySubcommand`
   publishes.
4. Keep the current `runRepl()` plain-stream path for non-TTY test streams and
   redirected sessions. Route only simultaneous stdin/stdout TTY sessions to
   the FTXUI workspace.
5. Remove `TerminalLayout` from the workspace path. Replace its footer
   responsibilities with a workspace-owned status component; do not emit
   DECSTBM or other terminal layout escape sequences from query UI code.

### 2. Create the root-level TUI client module and make it available

1. Fetch and link FTXUI for the main executable independently of the
   `MLDP_WIZARD` option. `MLDP_WIZARD` continues to control only wizard
   features.
2. Create this root-level module structure:

   ```text
   query-tui/
   ├── include/query-tui/  # public client-facing TUI contracts
   ├── src/                # FTXUI client, pane components, result store
   └── test/               # isolated TUI state and result-store tests
   ```

   `QuerySubcommand.cpp` remains in `src/query/` and includes only the small
   `query-tui` launch/controller contract. The TUI client must not include
   parser, planner, executor, gRPC implementation headers, `QueryRunner`, or
   `QueryContinuationRegistry` directly.
3. Give every handwritten TUI class a paired header/source tuple inside this
   module and list its implementation files explicitly in CMake. Keep FTXUI
   types confined to `query-tui`; keep query execution and formatter interfaces
   FTXUI-free.
4. Preserve existing query test target linkage so workspace unit tests can use
   an off-screen FTXUI screen or presentation-independent state tests.

### 3. Implement disk-backed result history

1. Add a result-store contract that creates one unique directory beneath the
   query temporary directory at workspace startup. It owns result IDs, Arrow
   IPC paths, schema, total rows, byte size, title/SQL metadata, and an
   independent viewport offset per result.
2. For table and expanded output, drain the completed pull stream into an
   Arrow IPC snapshot while preserving formatter backpressure, cancellation,
   final query statistics, and errors. Do not collect an unbounded result in
   RAM merely to draw it.
3. Open a selected snapshot through Arrow IPC for visible-row rendering. Read
   only the batches required for the current viewport, format those rows with
   the existing table/expanded formatting rules, and clamp all offsets to the
   result's row bounds.
4. On successful result completion, publish its record to history. On query
   failure or cancellation, retain no partial result as a selectable completed
   table and remove its partial snapshot.
5. Sum completed snapshot bytes. At more than 1 GiB, present a visible warning
   but continue accepting and retaining results. On workspace shutdown remove
   the entire unique session directory using best-effort cleanup.

### 4. Build the workspace UI and event loop

1. Compose FTXUI components for the result header, scrollable result body,
   multiline SQL editor, and fixed status bar. The root component owns focus,
   event routing, resize redraw, and terminal restoration.
2. Implement the two-thirds results / one-third editor split, reducing each
   pane only to its defined minimum when a terminal is short. The header and
   status bar remain visible; display an actionable minimum-size message when
   the terminal cannot show both panes safely.
3. Route key events by focus according to the interaction contract. Result
   scrolling and result selection must never alter the editor contents;
   editor operations must never reset a stored result's scroll position.
4. Run queries asynchronously through the existing `QueryRunner`, publish
   `QueryProgressTracker` updates on the FTXUI event loop, and render the
   existing `QueryStats`/cancellation state in the status line. Ctrl-C must
   request the existing cancellation object and keep the workspace usable.
5. Preserve the active editor text after a query completes, except when normal
   successful submit behavior explicitly clears it. Surface parse, plan, and
   execution errors in the status area without terminating the session.

### 5. Documentation and obsolete layout cleanup

1. Update `docs/guides/query-cli.md` with the fixed-viewport model, diagram,
   real-TTY activation condition, keymap, result-format boundary, temporary
   history lifecycle, unlimited retention, and 1 GiB warning.
2. Describe non-TTY and one-shot compatibility explicitly: no workspace or
   terminal control sequences are emitted in these modes.
3. Remove or retire obsolete `TerminalLayout`/footer documentation and code
   only after the workspace passes the complete real-TTY acceptance matrix.
   Do not declare the replacement complete from unit tests alone.

## Tests and acceptance criteria

### Automated tests

- Result-store tests: unique session directory, Arrow IPC write/read, schema
  preservation, row and byte accounting, independent offsets, end-boundary
  clamping, selection order, over-1-GiB warning state, partial-result cleanup,
  and session cleanup.
- Workspace-state tests: focus switching, key dispatch, F5 submission,
  multiline statement readiness, history/completion integration, result
  scrolling, previous/next selection, preserved editor text, errors, resize
  layout calculations, and minimum-terminal-size behavior.
- Execution tests: running progress, Ctrl-C cancellation, complete statistics,
  no partial completed entry after error/cancellation, continuation behavior,
  and unchanged table/expanded formatter output for visible rows.
- Regression tests: current `QueryFormatterTest`, `QuerySubcommandTest`,
  cancellation tests, stream/continuation tests, and non-TTY REPL tests remain
  green. JSON, CSV, and Arrow retain their existing output contracts.

### Devcontainer verification

Run authoritative builds in the Linux devcontainer:

```sh
docker compose -f docker-compose.yml -f .devcontainer/docker-compose.devcontainer.yml \
  exec devcontainer bash -lc "cmake --build /workspace/build --target mldp_pvxs_driver mldp_pvxs_driver_test --parallel && ctest --test-dir /workspace/build -R '(QueryFormatterTest|QuerySubcommandTest|QueryCancellationTest|QueryContinuationRegistryTest|QueryRunnerTest|ConsoleFooterTest)' --output-on-failure"
git diff --check
```

### Manual real-TTY acceptance

Verify in an actual terminal, not redirected output:

1. Start with an empty workspace; editor, result header, and status bar are
   visible and the caret is usable.
2. Run a result larger than the viewport; scroll from first to last row while
   the SQL editor stays fixed and editable.
3. Run multiple queries; switch results with Alt-Left/Right and confirm each
   retains its own scroll position.
4. Verify multiline SQL, history, catalog completion, `.clear`, format and
   expanded display commands, F5, Ctrl-C, Ctrl-L, resize, an execution error,
   and Ctrl-Q cleanup.
5. Run JSON, CSV, Arrow, redirected REPL, and one-shot SQL; confirm their
   output remains unchanged and contains no workspace control sequences.

The work is complete only when the real-TTY matrix passes and a large result
cannot displace, overwrite, or hide the SQL editor, result rows, Ctrl-C
feedback, or cursor.
