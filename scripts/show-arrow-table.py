#!/usr/bin/env python3
# Copyright (c) 2026 SLAC National Accelerator Laboratory
# Distributed subject to the EPICS Open License found in the LICENSE.txt file.
"""Print all content from an Arrow IPC file as a pandas table.

Examples:
  python3 scripts/show-arrow-table.py query-dir/spill_wide-long_0.arrow
  python3 scripts/show-arrow-table.py --all query-dir
  python3 scripts/show-arrow-table.py --max-rows 100 query-dir/spill_wide-long_0.arrow

Requires ``pyarrow`` and ``pandas`` in the Python environment that runs it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable, Sequence


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", type=Path, help="Arrow IPC file, or directory when --all is used")
    parser.add_argument("--all", action="store_true", help="Read every *.arrow file recursively below PATH")
    parser.add_argument(
        "--max-rows",
        type=int,
        help="Limit printed rows for each file; omit to print every row",
    )
    parser.add_argument("--no-index", action="store_true", help="Do not print the pandas row index")
    return parser.parse_args(argv)


def arrow_files(path: Path, all_files: bool) -> Iterable[Path]:
    if all_files:
        if not path.is_dir():
            raise ValueError("--all requires PATH to be a directory")
        yield from sorted(path.rglob("*.arrow"))
        return
    if not path.is_file():
        raise FileNotFoundError(f"Arrow IPC file not found: {path}")
    yield path


def read_ipc_table(path: Path):
    import pyarrow.ipc as ipc

    with path.open("rb") as source:
        try:
            return ipc.open_file(source).read_all()
        except Exception as file_error:
            source.seek(0)
            try:
                return ipc.open_stream(source).read_all()
            except Exception as stream_error:
                raise RuntimeError(f"{path} is not a readable Arrow IPC file or stream: file reader: {file_error}; stream reader: {stream_error}") from stream_error


def print_table(path: Path, max_rows: int | None, show_index: bool) -> None:
    table = read_ipc_table(path)
    frame = table.to_pandas()
    print(f"\n{path}: {table.num_rows} row(s), {table.num_columns} column(s)")
    print(f"schema: {table.schema}")
    if frame.empty:
        print("<empty>")
        return
    displayed = frame if max_rows is None else frame.head(max_rows)
    with __import__("pandas").option_context(
        "display.max_columns",
        None,
        "display.max_colwidth",
        None,
        "display.max_rows",
        None,
        "display.max_seq_items",
        None,
        "display.width",
        0,
    ):
        print(displayed.to_string(index=show_index, max_rows=None, max_cols=None))
    if max_rows is not None and len(frame) > max_rows:
        print(f"... {len(frame) - max_rows} additional row(s) not shown")


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.max_rows is not None and args.max_rows <= 0:
        raise ValueError("--max-rows must be positive")
    try:
        import pandas  # noqa: F401
        import pyarrow  # noqa: F401
    except ImportError as error:
        raise RuntimeError("Install the required reader packages with: python3 -m pip install pandas pyarrow") from error

    files = list(arrow_files(args.path, args.all))
    if not files:
        print(f"No .arrow files found below {args.path}", file=sys.stderr)
        return 1
    for path in files:
        print_table(path, args.max_rows, not args.no_index)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(2)
