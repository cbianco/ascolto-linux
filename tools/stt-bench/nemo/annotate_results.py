"""Annotate completed native benchmark metadata from retained local evidence."""
import hashlib
import json
from pathlib import Path
import re
import statistics

ROOT = Path(__file__).resolve().parents[3]
RESULTS = ROOT / "build/stt-bench/results"

for meta_path in sorted(RESULTS.glob("*.metadata.json")):
    if not meta_path.name.startswith(("nemo-", "parakeet-")):
        continue
    name = meta_path.name.removesuffix(".metadata.json")
    ledger_path = RESULTS / f"{name}-run.json"
    rows_path = RESULTS / f"{name}.jsonl"
    log_path = RESULTS / f"{name}.log"
    if not all(path.exists() for path in (ledger_path, rows_path, log_path)):
        continue
    metadata = json.loads(meta_path.read_text())
    ledger = json.loads(ledger_path.read_text())
    if (metadata.get("status") == "prepared_for_corrected_gpu_rerun" and
            ledger.get("environment", {}).get("PARAKEET_DEVICE") != "MTL0"):
        continue
    rows = [json.loads(line) for line in rows_path.read_text().splitlines() if line.startswith('{"model"')]
    if ledger["status"] != "completed" or not rows:
        continue
    log = log_path.read_text()
    match = re.search(r"BENCH_LOAD_JSON (\{[^\n]+\})", log)
    load = json.loads(match.group(1)) if match else None
    measured = [row for row in rows if not row["warmup"]]
    requested = metadata["backend_requested"]
    fallback = "falling back to CPU" in log
    observed_match = re.search(r"backend=(\S+)", log) or re.search(r"using device: (\S+)", log)
    if fallback:
        observed = "cpu"
    elif observed_match:
        observed = observed_match.group(1)
    elif ledger.get("environment", {}).get("PARAKEET_DEVICE") == "cpu":
        observed = "cpu"
    else:
        observed = None
    audio_s = sum(row["audio_s"] for row in measured)
    total_ms = sum(row["total_ms"] for row in measured)
    first = [row["first_token_ms"] for row in measured if row["first_token_ms"] is not None]
    metadata.update({
        "status": "completed",
        "backend_observed": observed,
        "measurement_matches_requested_backend": (observed == "cpu" if requested == "cpu" else
                                                   bool(observed and observed.startswith("MTL"))),
        "execution_environment": ledger.get("environment", {}),
        "execution_command_argv": ledger["command"],
        "execution_started_utc": ledger["started_utc"],
        "execution_elapsed_s": ledger["elapsed_s"],
        "load_measurement": load,
        "result_validation": {
            "rows": len(rows), "warmup_rows": sum(row["warmup"] for row in rows),
            "measured_rows": len(measured), "decode_errors": sum(bool(row["error"]) for row in rows),
            "text_stable_across_three_passes": all(
                len({row["text"] for row in measured if row["clip"] == clip}) == 1
                for clip in {row["clip"] for row in measured}),
            "silence_output_words": [len(row["text"].split()) for row in measured
                                     if row["clip"] == "silence-5s"],
        },
        "measured_aggregate": {
            "audio_s_including_repeats": audio_s, "decode_wall_s": total_ms / 1000,
            "weighted_rtf": total_ms / 1000 / audio_s,
            "decode_cpu_s": sum(row["cpu_ms"] for row in measured) / 1000,
            "peak_rss_mb": max(row["peak_rss_mb"] for row in rows),
            "first_returned_text_ms_median": statistics.median(first) if first else None,
            "first_returned_text_ms_min": min(first) if first else None,
            "first_returned_text_ms_max": max(first) if first else None,
        },
        "result_artifact": {"path": str(rows_path), "size_bytes": rows_path.stat().st_size,
                            "sha256": hashlib.sha256(rows_path.read_bytes()).hexdigest()},
        "backend_evidence": "explicit runtime fallback message" if fallback else
            (observed_match.group(0) if observed_match else "forced cpu environment and native source selection"),
    })
    notes = metadata.setdefault("measurement_notes", [])
    cache_note = ("Model load timing includes native backend/library initialization and possible driver "
                  "shader cache work. The runs do not use a uniformly cold process/driver/file cache. "
                  "Nemotron streaming loaded first in 17.857 s; offline followed in 0.073619 s. "
                  "Shader-cache contribution is a plausible inference, not directly instrumented.")
    if cache_note not in notes:
        notes.append(cache_note)
    if fallback:
        metadata["errors"] = ["PARAKEET_DEVICE=Metal did not match registry device MTL0; inference ran on CPU."]
        metadata["corrected_environment_for_gpu_rerun"] = {
            "PARAKEET_DEVICE": "MTL0", "PARAKEET_REPACK_THREADS": "8"}
    else:
        metadata["errors"] = []
        metadata.pop("corrected_environment_for_gpu_rerun", None)
    if name.startswith("nemo-") and not any(note.startswith("Streaming finalization") for note in notes):
        notes.append("Streaming finalization was checked in source: finish stores one final result; "
                     "next consumes it once, and the final transcript is cumulative. "
                     "The harness replaces earlier partials instead of concatenating cumulative text.")
    meta_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(name, observed, metadata["measurement_matches_requested_backend"])
