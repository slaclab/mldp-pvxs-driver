#!/usr/bin/env python3
"""Regression tests for scripts/show-arrow-table.py."""

from __future__ import annotations

import importlib.util
import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "show-arrow-table.py"
SCRIPT_SPEC = importlib.util.spec_from_file_location("show_arrow_table", SCRIPT_PATH)
assert SCRIPT_SPEC is not None
assert SCRIPT_SPEC.loader is not None
show_arrow_table = importlib.util.module_from_spec(SCRIPT_SPEC)
SCRIPT_SPEC.loader.exec_module(show_arrow_table)


class ShowArrowTableTest(unittest.TestCase):
    def test_converts_dense_union_to_active_python_values(self) -> None:
        import pyarrow as pa

        values = pa.UnionArray.from_dense(
            pa.array([0, 1, 0], type=pa.int8()),
            pa.array([0, 0, 1], type=pa.int32()),
            [pa.array(["one", "three"]), pa.array([2.5])],
            field_names=["string", "double"],
            type_codes=[0, 1],
        )
        table = pa.table({"pv": ["PV:ONE", "PV:TWO", "PV:THREE"], "value": values})

        compatible = show_arrow_table.dataframe_compatible_frame(table)

        self.assertEqual(compatible["value"].tolist(), ["one", 2.5, "three"])

    def test_reads_and_prints_arrow_file_with_dense_union(self) -> None:
        import pyarrow as pa
        import pyarrow.ipc as ipc

        values = pa.UnionArray.from_dense(
            pa.array([0], type=pa.int8()),
            pa.array([0], type=pa.int32()),
            [pa.array([42.0])],
            field_names=["double"],
            type_codes=[0],
        )
        table = pa.table({"value": values})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "native-values.arrow"
            with path.open("wb") as target:
                with ipc.new_file(target, table.schema) as writer:
                    writer.write_table(table)
            output = io.StringIO()
            with redirect_stdout(output):
                show_arrow_table.print_table(path, None, False)
            self.assertIn("42.0", output.getvalue())


if __name__ == "__main__":
    unittest.main()
