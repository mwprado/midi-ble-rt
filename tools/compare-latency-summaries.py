#!/usr/bin/env python3
"""Compare two summarize-latency.py TSV outputs.

The comparison is intentionally conservative for MIDI use: it highlights average
and worst-case latency/jitter. A candidate should not be accepted only because
its averages improved; max latency and max jitter must also be checked.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

KEY = ("scenario", "dir", "metric")
METRICS = (
    "events_sum",
    "lat_avg_max_ms",
    "lat_max_ms",
    "jitter_events_sum",
    "jitter_avg_max_ms",
    "jitter_max_ms",
)
OUT_HEADER = (
    *KEY,
    "baseline_events",
    "candidate_events",
    "lat_avg_delta_ms",
    "lat_avg_delta_pct",
    "lat_max_delta_ms",
    "lat_max_delta_pct",
    "jitter_avg_delta_ms",
    "jitter_avg_delta_pct",
    "jitter_max_delta_ms",
    "jitter_max_delta_pct",
)


def read_summary(path: Path) -> dict[tuple[str, str, str], dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        missing = set(KEY + METRICS) - set(reader.fieldnames or [])
        if missing:
            raise SystemExit(f"{path}: missing columns: {', '.join(sorted(missing))}")
        return {(row["scenario"], row["dir"], row["metric"]): row for row in reader}


def as_float(row: dict[str, str], key: str) -> float:
    value = row.get(key, "")
    if value == "":
        return 0.0
    return float(value)


def delta(candidate: float, baseline: float) -> tuple[float, str]:
    d = candidate - baseline
    if baseline == 0:
        return d, ""
    return d, f"{(d / baseline) * 100.0:.1f}"


def fmt_ms(value: float) -> str:
    return f"{value:.3f}"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Compare midi-ble-rt latency summary TSV files")
    parser.add_argument("baseline", type=Path, help="baseline summary.tsv")
    parser.add_argument("candidate", type=Path, help="candidate summary.tsv")
    args = parser.parse_args(argv)

    baseline = read_summary(args.baseline)
    candidate = read_summary(args.candidate)

    writer = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
    writer.writerow(OUT_HEADER)

    for key in sorted(set(baseline) & set(candidate)):
        b = baseline[key]
        c = candidate[key]

        lat_avg_d, lat_avg_pct = delta(as_float(c, "lat_avg_max_ms"), as_float(b, "lat_avg_max_ms"))
        lat_max_d, lat_max_pct = delta(as_float(c, "lat_max_ms"), as_float(b, "lat_max_ms"))
        jit_avg_d, jit_avg_pct = delta(as_float(c, "jitter_avg_max_ms"), as_float(b, "jitter_avg_max_ms"))
        jit_max_d, jit_max_pct = delta(as_float(c, "jitter_max_ms"), as_float(b, "jitter_max_ms"))

        writer.writerow((
            *key,
            b["events_sum"],
            c["events_sum"],
            fmt_ms(lat_avg_d),
            lat_avg_pct,
            fmt_ms(lat_max_d),
            lat_max_pct,
            fmt_ms(jit_avg_d),
            jit_avg_pct,
            fmt_ms(jit_max_d),
            jit_max_pct,
        ))

    missing = sorted(set(baseline) ^ set(candidate))
    for key in missing:
        print(f"warning: unmatched row: {'/'.join(key)}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
