#!/usr/bin/env python3
"""Summarize midi-ble-rt latency.tsv v2 captures.

The tool accepts one or more files or directories. Directories are scanned
recursively for *.tsv files. Each parent directory is treated as the scenario
name, which matches the recommended baseline layout:

    measurements/pre-rtkit/short-notes/latency-1.tsv
    measurements/pre-rtkit/chords/latency-1.tsv

The output is a compact TSV summary grouped by scenario, direction and metric.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path
from typing import Iterable


KEYS = (
    "count",
    "avg_ms",
    "p95_ms",
    "p99_ms",
    "max_ms",
    "jitter_count",
    "jitter_avg_ms",
    "jitter_p95_ms",
    "jitter_p99_ms",
    "jitter_max_ms",
)

OUT_HEADER = (
    "scenario",
    "dir",
    "metric",
    "samples",
    "events_sum",
    "lat_avg_max_ms",
    "lat_p95_max_ms",
    "lat_p99_max_ms",
    "lat_max_ms",
    "jitter_events_sum",
    "jitter_avg_max_ms",
    "jitter_p95_max_ms",
    "jitter_p99_max_ms",
    "jitter_max_ms",
)


def iter_tsv_paths(paths: Iterable[Path]) -> Iterable[Path]:
    for path in paths:
        if path.is_dir():
            yield from sorted(p for p in path.rglob("*.tsv") if p.is_file())
        elif path.is_file():
            yield path
        else:
            print(f"warning: skipping missing path: {path}", file=sys.stderr)


def scenario_name(path: Path) -> str:
    parent = path.parent.name
    if parent:
        return parent
    return "."


def parse_latency_tsv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        version = f.readline().strip()
        if version != "v2":
            raise ValueError(f"unsupported latency format {version!r}; expected v2")

        reader = csv.DictReader(f, delimiter="\t")
        rows = list(reader)

    required = {"dir", "metric", *KEYS}
    missing = required - set(reader.fieldnames or [])
    if missing:
        raise ValueError(f"missing columns: {', '.join(sorted(missing))}")

    return rows


def as_float(row: dict[str, str], key: str) -> float:
    value = row.get(key, "")
    if value == "":
        return 0.0
    return float(value)


def as_int(row: dict[str, str], key: str) -> int:
    value = row.get(key, "")
    if value == "":
        return 0
    return int(float(value))


def summarize(paths: Iterable[Path]) -> list[dict[str, object]]:
    groups: dict[tuple[str, str, str], dict[str, object]] = defaultdict(
        lambda: {
            "samples": 0,
            "events_sum": 0,
            "lat_avg_max_ms": 0.0,
            "lat_p95_max_ms": 0.0,
            "lat_p99_max_ms": 0.0,
            "lat_max_ms": 0.0,
            "jitter_events_sum": 0,
            "jitter_avg_max_ms": 0.0,
            "jitter_p95_max_ms": 0.0,
            "jitter_p99_max_ms": 0.0,
            "jitter_max_ms": 0.0,
        }
    )

    parsed = 0
    for path in iter_tsv_paths(paths):
        try:
            rows = parse_latency_tsv(path)
        except Exception as exc:  # noqa: BLE001 - CLI should continue across files.
            print(f"warning: skipping {path}: {exc}", file=sys.stderr)
            continue

        parsed += 1
        scenario = scenario_name(path)
        for row in rows:
            direction = row.get("dir", "-") or "-"
            metric = row.get("metric", "-") or "-"
            g = groups[(scenario, direction, metric)]

            g["samples"] = int(g["samples"]) + 1
            g["events_sum"] = int(g["events_sum"]) + as_int(row, "count")
            g["lat_avg_max_ms"] = max(float(g["lat_avg_max_ms"]), as_float(row, "avg_ms"))
            g["lat_p95_max_ms"] = max(float(g["lat_p95_max_ms"]), as_float(row, "p95_ms"))
            g["lat_p99_max_ms"] = max(float(g["lat_p99_max_ms"]), as_float(row, "p99_ms"))
            g["lat_max_ms"] = max(float(g["lat_max_ms"]), as_float(row, "max_ms"))

            g["jitter_events_sum"] = int(g["jitter_events_sum"]) + as_int(row, "jitter_count")
            g["jitter_avg_max_ms"] = max(float(g["jitter_avg_max_ms"]), as_float(row, "jitter_avg_ms"))
            g["jitter_p95_max_ms"] = max(float(g["jitter_p95_max_ms"]), as_float(row, "jitter_p95_ms"))
            g["jitter_p99_max_ms"] = max(float(g["jitter_p99_max_ms"]), as_float(row, "jitter_p99_ms"))
            g["jitter_max_ms"] = max(float(g["jitter_max_ms"]), as_float(row, "jitter_max_ms"))

    if parsed == 0:
        raise SystemExit("no valid latency.tsv v2 files found")

    out: list[dict[str, object]] = []
    for (scenario, direction, metric), values in sorted(groups.items()):
        item = {"scenario": scenario, "dir": direction, "metric": metric}
        item.update(values)
        out.append(item)
    return out


def fmt(value: object) -> str:
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Summarize midi-ble-rt latency.tsv v2 captures")
    parser.add_argument("paths", nargs="+", type=Path, help="latency.tsv files or directories to scan")
    args = parser.parse_args(argv)

    rows = summarize(args.paths)

    writer = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
    writer.writerow(OUT_HEADER)
    for row in rows:
        writer.writerow(fmt(row.get(key, "")) for key in OUT_HEADER)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
