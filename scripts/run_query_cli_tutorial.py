#!/usr/bin/env python3
# Copyright (c) 2026 SLAC National Accelerator Laboratory
# Distributed subject to the EPICS Open License found in the LICENSE.txt file.
"""Run the query CLI tutorial against a live MLDP stack.

The runner creates a dedicated sample-data namespace, launches the real
``mldp_pvxs_driver query`` REPL with its documented inline configuration, and
submits the tutorial statements through standard input.  It also exercises the
persistent-table walkthrough with separate client processes sharing one
temporary Arrow IPC catalog.

Run inside the devcontainer after building the driver:

  python3 scripts/run_query_cli_tutorial.py

The generated annotation records are removed on exit unless ``--keep-namespace``
is passed.  MLDP does not expose time-series bucket deletion, so those archived
samples remain after cleanup.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Sequence


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_namespace() -> str:
    return f"tutorial_{int(time.time())}_{os.getpid()}"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--driver", type=Path, default=repository_root() / "build/bin/mldp_pvxs_driver", help="Path to mldp_pvxs_driver")
    parser.add_argument("--namespace", default=default_namespace(), help="Dedicated sample-data namespace")
    parser.add_argument("--ingestion-url", default="dp-ingestion:50051", help="MLDP ingestion gRPC endpoint")
    parser.add_argument("--query-url", default="dp-query:50052", help="MLDP query gRPC endpoint")
    parser.add_argument("--annotation-url", default="dp-annotation:50053", help="MLDP annotation gRPC endpoint")
    parser.add_argument("--timeout-seconds", type=float, default=180.0, help="Timeout for each generator or CLI process")
    parser.add_argument("--verify", action="store_true", help="Ask the sample-data generator to read back annotation records")
    parser.add_argument("--keep-namespace", action="store_true", help="Keep generated annotation records after the run")
    return parser.parse_args(argv)


def run(command: list[str], *, input_text: str | None, timeout_seconds: float) -> None:
    completed = subprocess.run(
        command,
        cwd=repository_root(),
        input=input_text,
        text=True,
        timeout=timeout_seconds,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"Command failed with exit code {completed.returncode}: {' '.join(command)}")


def cli_command(args: argparse.Namespace, catalog_directory: Path) -> list[str]:
    return [
        str(args.driver),
        "-c", "queryable.mldp.mldp-pool.query-url=" + args.query_url,
        "-c", "queryable.mldp.mldp-pool.min-conn=1",
        "-c", "queryable.mldp.mldp-pool.max-conn=2",
        "-c", "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.annotation-url=" + args.annotation_url,
        "-c", "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.min-conn=1",
        "-c", "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.max-conn=2",
        "query",
        "--table-catalog-dir", str(catalog_directory),
    ]


def tutorial_sql(namespace: str) -> str:
    magnet = f"{namespace}:MAGNET:01:VALUE"
    return f"""SHOW TABLES;
DESC mldp.time_series;
DESCRIBE mldp.pv_metadata;

SELECT pv, time, value
FROM mldp.time_series
WHERE pv = '{magnet}'
  AND time >= NOW -10m
  AND time <= NOW;

SELECT pv, time, value
FROM mldp.time_series
WHERE pv IN ('{namespace}:MAGNET:01:VALUE', '{namespace}:RF:02:VALUE',
             '{namespace}:VACUUM:03:VALUE', '{namespace}:DIAGNOSTIC:04:VALUE')
  AND time >= NOW -1h
  AND time <= NOW;

SELECT pv, first_timestamp, last_timestamp, num_buckets
FROM mldp.pv_stats
WHERE pv IN ('{namespace}:MAGNET:01:VALUE', '{namespace}:RF:02:VALUE',
             '{namespace}:VACUUM:03:VALUE', '{namespace}:DIAGNOSTIC:04:VALUE');

SELECT pv, description, attributes.device_group, attributes.units, modified_by
FROM mldp.pv_metadata
WHERE pv PREFIX '{namespace}';

SELECT pv, alias, description, attributes.device_group, attributes.ordinal, tags
FROM mldp.pv_metadata
WHERE tag = 'magnet';

SELECT pv, attributes.device_group, attributes.ordinal
FROM mldp.pv_metadata
WHERE attributes.namespace = '{namespace}'
ORDER BY attributes.ordinal;

SELECT name, category, description
FROM mldp.configuration
WHERE name PREFIX '{namespace}';

SELECT name, category, description
FROM mldp.configuration
WHERE category = 'beam_mode';

SELECT time, end_time, config_name, activation_id
FROM mldp.configuration_activation
WHERE config_name IN ('{namespace}_injector_tuning', '{namespace}_user_delivery')
  AND end_time IS NOT NULL;

SELECT name, activation_id, time
FROM mldp.active_configurations
WHERE at = NOW -30m;

SELECT ts.pv, ts.time, ts.value, m.description, m.attributes.units
FROM mldp.time_series ts
JOIN mldp.pv_metadata m ON ts.pv = m.pv
WHERE ts.pv IN (
  SELECT pv FROM mldp.pv_metadata WHERE pv PREFIX '{namespace}:MAGNET'
)
  AND ts.time >= NOW -10m
  AND ts.time <= NOW;

EXPLAIN SELECT pv, time, value FROM mldp.time_series
WHERE pv = '{magnet}' AND time >= NOW -1h AND time <= NOW;

SELECT ts.pv, ts.time, ts.value, m.description
FROM mldp.time_series ts
JOIN mldp.pv_metadata m ON ts.pv = m.pv
WHERE ts.pv IN ('{namespace}:MAGNET:01:VALUE', '{namespace}:RF:02:VALUE',
                '{namespace}:VACUUM:03:VALUE', '{namespace}:DIAGNOSTIC:04:VALUE')
  AND ts.time >= NOW -1h AND ts.time <= NOW
  AND m.pv PREFIX '{namespace}'
LIMIT 200;

SELECT *
FROM mldp.time_series_table
WHERE pv IN (
  SELECT pv FROM mldp.pv_metadata
  WHERE attributes.namespace = '{namespace}' AND tag = 'magnet'
)
AND window IN (
  SELECT activation.time, activation.end_time
  FROM mldp.configuration_activation activation
  JOIN mldp.configuration configuration ON activation.config_name = configuration.name
  WHERE activation.attributes.namespace = '{namespace}'
    AND configuration.attributes.namespace = '{namespace}'
    AND configuration.category = 'beam_mode'
    AND activation.end_time IS NOT NULL
);

CREATE TEMP TABLE magnet_samples AS
SELECT pv, time, value FROM mldp.time_series
WHERE pv = '{magnet}' AND time >= NOW -10m AND time <= NOW;
SELECT * FROM magnet_samples;
DROP TABLE magnet_samples;
.quit
"""


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.timeout_seconds <= 0:
        raise ValueError("--timeout-seconds must be positive")
    if not args.driver.is_file():
        raise FileNotFoundError(f"Driver binary not found: {args.driver}")

    generator = [sys.executable, "scripts/generate_mldp_sample_data.py", "--namespace", args.namespace,
                 "--ingestion-url", args.ingestion_url, "--annotation-url", args.annotation_url,
                 "--timeout-seconds", str(args.timeout_seconds)]
    if args.verify:
        generator.append("--verify")

    generated = False
    try:
        run(generator, input_text=None, timeout_seconds=args.timeout_seconds)
        generated = True
        with tempfile.TemporaryDirectory(prefix="mldp-query-tutorial-") as catalog:
            catalog_directory = Path(catalog)
            command = cli_command(args, catalog_directory)
            run(command, input_text=tutorial_sql(args.namespace), timeout_seconds=args.timeout_seconds)

            persistent_create = f"""CREATE TABLE magnet_samples AS
SELECT pv, time, value FROM mldp.time_series
WHERE pv = '{args.namespace}:MAGNET:01:VALUE' AND time >= NOW -10m AND time <= NOW;
.quit
"""
            run(command, input_text=persistent_create, timeout_seconds=args.timeout_seconds)
            run(command, input_text="SHOW TABLES;\nDESC magnet_samples;\nSELECT * FROM magnet_samples;\n.quit\n", timeout_seconds=args.timeout_seconds)
            run(command, input_text="DROP TABLE magnet_samples;\nSHOW TABLES;\n.quit\n", timeout_seconds=args.timeout_seconds)
    finally:
        if generated and not args.keep_namespace:
            run(generator + ["--drop-namespace"], input_text=None, timeout_seconds=args.timeout_seconds)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, subprocess.TimeoutExpired, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1)
