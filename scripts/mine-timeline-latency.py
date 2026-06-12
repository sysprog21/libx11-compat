#!/usr/bin/env python3
"""Extract latency spans from a libx11-compat timeline JSONL.

Spans:
  menu_open_ms      ButtonPress -> first MapNotify after it
  scroll_settle_ms  ButtonPress -> first Present after it
  dialog_open_ms    ConfigureNotify -> first Present after it
  keyboard_settle_ms KeyPress -> first Present after it

Each span emits a list of all observations and the median / p95. Designed
to be CI-friendly: comparing one JSON output against a per-driver baseline
under tests/ui/baselines/ flags a regression only when both relative delta
> 25% and absolute delta > 50 ms (configurable via --rel / --abs / --floor).
"""

import argparse
import json
import math
import sys
from pathlib import Path

SPAN_DEFS = [
    ("menu_open_ms", "ButtonPress", "MapNotify"),
    ("scroll_settle_ms", "ButtonPress", "Present"),
    ("dialog_open_ms", "ConfigureNotify", "Present"),
    ("keyboard_settle_ms", "KeyPress", "Present"),
]


def load_records(path):
    records = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "t_ms" in record and "kind" in record:
                records.append(record)
    return records


def extract_spans(records, anchor_kind, target_kind, max_window_ms):
    """Match each anchor to the first target in (anchor, anchor+window].

    A target is consumed by at most one anchor so a bursty present after a
    single press doesn't inflate every later press's reported latency.
    """
    spans = []
    target_idx = 0
    targets = [r for r in records if r.get("kind") == target_kind]
    for record in records:
        if record.get("kind") != anchor_kind:
            continue
        anchor_t = record["t_ms"]
        deadline = anchor_t + max_window_ms
        while target_idx < len(targets) and targets[target_idx]["t_ms"] <= anchor_t:
            target_idx += 1
        if target_idx >= len(targets):
            break
        target = targets[target_idx]
        if target["t_ms"] > deadline:
            continue
        spans.append(target["t_ms"] - anchor_t)
        target_idx += 1
    return spans


def percentile(values, pct):
    if not values:
        return None
    s = sorted(values)
    if pct <= 0:
        return s[0]
    if pct >= 100:
        return s[-1]
    rank = (pct / 100) * (len(s) - 1)
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return s[lo]
    return s[lo] + (s[hi] - s[lo]) * (rank - lo)


def gate(metric, baseline_value, observed_value, rel, absolute, floor):
    """Return failure description, or None if within budget."""
    if observed_value is None:
        return None
    if baseline_value is None:
        return None
    if observed_value <= floor:
        return None
    delta = observed_value - baseline_value
    if delta <= absolute:
        return None
    if baseline_value <= 0:
        rel_ratio = float("inf")
    else:
        rel_ratio = delta / baseline_value
    if rel_ratio < rel:
        return None
    return (
        f"{metric}: observed {observed_value:.2f}ms vs baseline "
        f"{baseline_value:.2f}ms (delta {delta:.2f}ms, "
        f"rel {rel_ratio * 100:.1f}%)"
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--baseline",
        type=Path,
        default=None,
        help="JSON file with prior medians; missing keys ignored.",
    )
    parser.add_argument("--window-ms", type=int, default=2000)
    parser.add_argument(
        "--rel",
        type=float,
        default=0.25,
        help="relative regression budget (default 25%%).",
    )
    parser.add_argument(
        "--abs",
        dest="absolute",
        type=float,
        default=50.0,
        help="absolute regression budget in ms (default 50 ms).",
    )
    parser.add_argument(
        "--floor",
        type=float,
        default=5.0,
        help="ignore observations below this (ms).",
    )
    args = parser.parse_args(argv)

    records = load_records(args.timeline)
    summary = {}
    for name, anchor, target in SPAN_DEFS:
        spans = extract_spans(records, anchor, target, args.window_ms)
        if spans:
            summary[name] = {
                "count": len(spans),
                "median_ms": percentile(spans, 50),
                "p95_ms": percentile(spans, 95),
                "min_ms": min(spans),
                "max_ms": max(spans),
                "samples_ms": spans,
            }
        else:
            summary[name] = {
                "count": 0,
                "median_ms": None,
                "p95_ms": None,
                "min_ms": None,
                "max_ms": None,
                "samples_ms": [],
            }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, indent=2) + "\n")

    if args.baseline is None:
        return 0
    if not args.baseline.exists():
        print(
            f"baseline {args.baseline} not present; emitting summary only",
            file=sys.stderr,
        )
        return 0
    with args.baseline.open() as f:
        baseline = json.load(f)
    failures = []
    for name in summary:
        observed = summary[name].get("median_ms")
        if name not in baseline:
            continue
        baseline_value = baseline[name].get("median_ms")
        msg = gate(name, baseline_value, observed, args.rel, args.absolute, args.floor)
        if msg:
            failures.append(msg)
    if failures:
        for msg in failures:
            print(f"latency regression: {msg}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
