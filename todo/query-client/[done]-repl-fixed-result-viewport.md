# Retired: Fixed Result Viewport Query REPL

The full-screen fixed result viewport was retired because it is not reliable
for the supported SSH and container-attached terminal workflows. A terminal UI
cannot safely own result scrolling, line editing, resize handling, and control
keys when the user expects the terminal emulator's own scrollback and input
model.

## Replacement behavior

- The interactive query client always uses the `replxx` line editor.
- Table and expanded results write directly to normal terminal scrollback.
- `Ctrl-C` retains the existing query-cancellation and editor-abort behavior.
- `Ctrl-Q`, `.quit`, and `.exit` leave the interactive REPL.
- `.pager on` sends real-TTY table output to `$PAGER`; if unset, it uses
  `less -FRSX`. Paging is disabled by default and is not used for redirected,
  one-shot, JSON, CSV, or Arrow output.
- Completed result history is terminal scrollback only. The removed workspace
  does not retain per-session Arrow IPC copies or independent result offsets.

## Verification

- Query command, formatter, cancellation, continuation, and completion tests
  cover the line-oriented REPL behavior and pager configuration.
- Manual acceptance is performed through the actual SSH/container-attached
  terminal: submit a large table result, scroll with the terminal emulator,
  verify `Ctrl-C`, `Ctrl-Q`, resize, `.pager on`, pager return, and terminal
  restoration.
