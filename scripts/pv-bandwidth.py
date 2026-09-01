#!/usr/bin/env python3
# Copyright (c) 2026 SLAC National Accelerator Laboratory
# Distributed subject to the EPICS Open License found in the LICENSE.txt file.
"""
pv_bandwidth.py - Monitor EPICS PV bandwidth and mean data size via PVA (p4p).

Connects to one or more PVs using PVAccess and periodically reports:
  - Bytes/second (sliding window)
  - Mean and median update size in bytes
  - Last observed update size
  - NTTable detection and column-aware sizing

Requires:
  pip install p4p numpy

Usage:
  python pv_bandwidth.py BSAS:SYS0:1:CUHXR_TBL BSAS:SYS0:1:CUSXR_TBL
  python pv_bandwidth.py MY:PV:1 MY:PV:2 --interval 2 --window 30
"""

from __future__ import annotations

import argparse
import signal
import statistics
import sys
import threading
import time
from collections import deque
from typing import Dict, List

try:
    import numpy as np
except ImportError:
    print("Error: numpy not found. Install with: pip install numpy", file=sys.stderr)
    sys.exit(1)

try:
    from p4p import Value
    from p4p.client.thread import Context
except ImportError:
    print("Error: p4p not found. Install with: pip install p4p", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# Byte-size estimation
# ---------------------------------------------------------------------------

def _scalar_size(v: object) -> int:
    """Return the byte footprint of a single scalar Python / numpy value."""
    if v is None:
        return 0
    if isinstance(v, np.ndarray):
        return int(v.nbytes)
    if isinstance(v, (bytes, bytearray)):
        return len(v)
    if isinstance(v, str):
        return len(v.encode("utf-8"))
    if isinstance(v, bool):
        return 1
    # numpy scalar types carry itemsize
    if hasattr(v, "itemsize"):
        return int(v.itemsize)
    if isinstance(v, float):
        return 8
    if isinstance(v, int):
        return 8
    if isinstance(v, list):
        return sum(_scalar_size(item) for item in v)
    # Unknown – fall back to string representation length
    try:
        return len(str(v).encode("utf-8"))
    except Exception:
        return 0


def estimate_value_bytes(value: Value) -> int:
    """
    Recursively estimate the byte footprint of a p4p Value.

    Walks every field in the type descriptor.  For NTTable the column
    arrays dominate, so this accurately captures the payload size.
    """
    total = 0
    try:
        spec = value.type()
        for field_name, _ in spec.items():
            try:
                fv = value[field_name]
            except Exception:
                continue
            if fv is None:
                continue
            # Nested p4p structure (e.g. alarm, timeStamp, or NTTable value struct)
            if hasattr(fv, "type") and callable(fv.type):
                total += estimate_value_bytes(fv)
            else:
                total += _scalar_size(fv)
    except Exception:
        # Last-resort fallback
        total = len(str(value).encode("utf-8"))
    return max(total, 1)


def detect_pv_type(value: Value) -> str:
    """Return a human-readable PV/NT type string."""
    try:
        type_id = value.getID()
        for known in ("NTTable", "NTScalarArray", "NTScalar", "NTNDArray", "NTEnum"):
            if known in type_id:
                return known
        return type_id or "unknown"
    except Exception:
        return "unknown"


# ---------------------------------------------------------------------------
# Per-PV statistics
# ---------------------------------------------------------------------------

class PVStats:
    """Thread-safe sliding-window statistics for one PV."""

    def __init__(self, name: str, window_seconds: float = 10.0) -> None:
        self.name = name
        self.window_seconds = window_seconds
        self._lock = threading.Lock()
        # Sliding window: deque of (monotonic_time, byte_size)
        self._window: deque = deque()
        # All-time sizes for mean/median
        self._all_sizes: List[int] = []
        self._update_count: int = 0
        self._error_count: int = 0
        self._last_type: str = "—"
        self._connected: bool = False

    def record(self, byte_size: int, pv_type: str = "unknown") -> None:
        now = time.monotonic()
        with self._lock:
            self._window.append((now, byte_size))
            self._all_sizes.append(byte_size)
            self._update_count += 1
            self._last_type = pv_type
            self._connected = True
            # Evict samples that have fallen outside the window
            cutoff = now - self.window_seconds
            while self._window and self._window[0][0] < cutoff:
                self._window.popleft()

    def record_error(self) -> None:
        with self._lock:
            self._error_count += 1
            self._connected = False

    def get_stats(self) -> dict:
        now = time.monotonic()
        with self._lock:
            cutoff = now - self.window_seconds
            in_window = [(t, b) for t, b in self._window if t >= cutoff]
            window_bytes = sum(b for _, b in in_window)
            times = [t for t, _ in in_window]
            if len(times) >= 2:
                duration = times[-1] - times[0]
            elif len(times) == 1:
                duration = now - times[0]
            else:
                duration = 0.0
            bps = window_bytes / duration if duration > 0.0 else 0.0
            mean_sz = statistics.mean(self._all_sizes) if self._all_sizes else 0.0
            median_sz = statistics.median(self._all_sizes) if self._all_sizes else 0.0
            return {
                "name": self.name,
                "type": self._last_type,
                "connected": self._connected,
                "update_count": self._update_count,
                "error_count": self._error_count,
                "bytes_per_sec": bps,
                "mean_size_bytes": mean_sz,
                "median_size_bytes": median_sz,
                "last_size_bytes": self._all_sizes[-1] if self._all_sizes else 0,
                "window_events": len(in_window),
            }


# ---------------------------------------------------------------------------
# Display helpers
# ---------------------------------------------------------------------------

def _fmt_bytes(n: float) -> str:
    if n < 1024:
        return f"{n:.0f} B"
    if n < 1024 ** 2:
        return f"{n / 1024:.2f} KB"
    if n < 1024 ** 3:
        return f"{n / 1024 ** 2:.2f} MB"
    return f"{n / 1024 ** 3:.2f} GB"


_COL_W = 120

def _print_stats(stats_map: Dict[str, PVStats], window: float, interval: float) -> None:
    now_str = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"\n{'─' * _COL_W}")
    print(
        f"  {'PV Name':<44} {'Type':<14} {'Conn':^4} "
        f"{'Bytes/s':>12} {'Mean Size':>12} {'Median':>10} {'Last':>10} "
        f"{'Updates':>8} {'Errors':>6}"
    )
    print(f"{'─' * _COL_W}")
    total_bps = 0.0
    for pv_name in sorted(stats_map):
        s = stats_map[pv_name].get_stats()
        conn = "YES" if s["connected"] else "NO"
        bps_str = _fmt_bytes(s["bytes_per_sec"]) + "/s"
        total_bps += s["bytes_per_sec"]
        print(
            f"  {pv_name:<44} {s['type']:<14} {conn:^4} "
            f"{bps_str:>12} {_fmt_bytes(s['mean_size_bytes']):>12} "
            f"{_fmt_bytes(s['median_size_bytes']):>10} {_fmt_bytes(s['last_size_bytes']):>10} "
            f"{s['update_count']:>8} {s['error_count']:>6}"
        )
    print(f"{'─' * _COL_W}")
    print(
        f"  {'TOTAL':<44} {'':14} {'':4} {_fmt_bytes(total_bps) + '/s':>12}"
        f"   window={window:.0f}s  interval={interval:.1f}s  [{now_str}]"
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Monitor EPICS PV bandwidth and mean data size via PVAccess (p4p). "
            "Supports scalar, array, and NTTable PVs."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "pvs",
        nargs="+",
        metavar="PV",
        help="One or more PV names to monitor (PVA protocol only).",
    )
    parser.add_argument(
        "--interval", "-i",
        type=float,
        default=5.0,
        metavar="SEC",
        help="Statistics display refresh interval in seconds.",
    )
    parser.add_argument(
        "--window", "-w",
        type=float,
        default=10.0,
        metavar="SEC",
        help="Sliding-window size used for bytes/sec calculation.",
    )
    args = parser.parse_args()

    if args.interval <= 0 or args.window <= 0:
        parser.error("--interval and --window must be positive numbers.")

    stats_map: Dict[str, PVStats] = {
        pv: PVStats(pv, args.window) for pv in args.pvs
    }

    stop_event = threading.Event()

    def _on_signal(sig, _frame):
        print("\nInterrupted – printing final statistics...")
        stop_event.set()

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    def _make_cb(pv_name: str):
        def _cb(value):
            if isinstance(value, Exception):
                stats_map[pv_name].record_error()
                return
            try:
                byte_size = estimate_value_bytes(value)
                pv_type = detect_pv_type(value)
                stats_map[pv_name].record(byte_size, pv_type)
            except Exception:
                stats_map[pv_name].record_error()
        return _cb

    ctx = Context("pva")
    subscriptions = []
    print(f"Subscribing to {len(args.pvs)} PV(s) via PVA …")
    for pv_name in args.pvs:
        try:
            sub = ctx.monitor(pv_name, _make_cb(pv_name))
            subscriptions.append(sub)
        except Exception as exc:
            print(f"  WARNING: could not subscribe to {pv_name!r}: {exc}", file=sys.stderr)

    print("Monitoring … Press Ctrl+C to stop.\n")
    try:
        while not stop_event.is_set():
            stop_event.wait(args.interval)
            _print_stats(stats_map, args.window, args.interval)
    finally:
        for sub in subscriptions:
            try:
                sub.close()
            except Exception:
                pass
        ctx.close()
        _print_stats(stats_map, args.window, args.interval)


if __name__ == "__main__":
    main()
