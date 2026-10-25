#!/usr/bin/env python3
"""Check completeness and timing consistency of the measured report dataset."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("data", type=Path)
    args = parser.parse_args()
    data = json.loads(args.data.read_text())
    groups = defaultdict(list)
    errors = []
    expected_clips = {clip["id"] for clip in data["clips"]}
    for row in data["rows"]:
        groups[row["model"]].append(row)
        for key in ("audio_s", "load_ms", "total_ms", "cpu_ms", "peak_rss_mb"):
            if not math.isfinite(row[key]) or row[key] < 0:
                errors.append(f"Invalid numeric field {row['model']}/{row['clip']}/{key}")
        for key in ("first_token_ms", "first_event_ms"):
            first = row.get(key)
            if first is not None and (not math.isfinite(first) or first < 0 or first > row["total_ms"] * 1.05):
                errors.append(f"Invalid first event {row['model']}/{row['clip']}/{key}")
    for model, rows in groups.items():
        if len(rows) != 25 or sum(r["warmup"] for r in rows) != 1:
            errors.append(f"Wrong warmup/row count: {model}")
        for repeat in range(3):
            measured = [r for r in rows if r["repeat"] == repeat]
            if len(measured) != len(expected_clips) or {r["clip"] for r in measured} != expected_clips:
                errors.append(f"Missing/duplicate clip: {model}/{repeat}")
        if len({r["pid"] for r in rows}) != 1:
            errors.append(f"Model not persistent: {model}")
    invalid = [r for r in data["rows"] if r["error"]]
    if len(invalid) != 3 or any(r["model"] != "voxtral" or r["clip"] != "call01-sys-30s" or r.get("api_error") != 0 for r in invalid):
        errors.append("Unexpected error pattern; inspect all API/validation failures")
    if set(groups) != set(data["models"]):
        errors.append("Candidate coverage does not match report model inventory")
    summary = dict(configurations=len(groups), records=len(data["rows"]),
                   measured_records=sum(not r["warmup"] for r in data["rows"]),
                   validation_failures=len(invalid), errors=errors)
    print(json.dumps(summary))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
