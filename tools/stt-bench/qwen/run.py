#!/usr/bin/env python3
"""Run the prepared C harness; invoke only in an assigned benchmark slot."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[3]
WORK = ROOT / "build/stt-bench"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", choices=["qwen3-asr-0.6b", "qwen3-asr-1.7b"], required=True)
    parser.add_argument("--manifest", type=pathlib.Path, default=WORK / "corpus/manifest.tsv")
    args = parser.parse_args()
    result_dir = WORK / "results"
    metadata_path = result_dir / "qwen-metadata.json"
    metadata = json.loads(metadata_path.read_text())
    manifest_lines = [line for line in args.manifest.read_text().splitlines() if line.strip()]
    if not manifest_lines or len(manifest_lines) > 256:
        raise RuntimeError("Manifest must contain between 1 and 256 clips")
    for line in manifest_lines:
        clip_id, path = line.split("\t")
        if not clip_id or not pathlib.Path(path).is_absolute() or pathlib.Path(path).stat().st_size % 4:
            raise RuntimeError("Manifest requires an identifier and absolute float32 PCM path")
    env = dict(os.environ, VECLIB_MAXIMUM_THREADS="8")
    output = result_dir / f"{args.model}.jsonl"
    log = result_dir / f"{args.model}.stderr.log"
    if output.exists():
        raise RuntimeError(f"Refusing to overwrite benchmark results: {output}")
    argv = [str(WORK / "runtimes/qwen-bench"), args.model,
            str(WORK / "models" / args.model), str(args.manifest.resolve())]
    run = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
           "command": argv, "output": str(output.relative_to(ROOT)),
           "log": str(log.relative_to(ROOT)), "status": "running",
           "manifest_sha256": hashlib.sha256(args.manifest.read_bytes()).hexdigest(),
           "clips": len(manifest_lines)}
    metadata.setdefault("runs", {})[args.model] = run
    metadata["status"] = "running"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    with output.open("w") as stdout, log.open("w") as stderr:
        result = subprocess.run(argv, cwd=ROOT, env=env, stdout=stdout, stderr=stderr)
    rows = []
    parse_error = None
    try:
        rows = [json.loads(line) for line in output.read_text().splitlines() if line.strip()]
    except (ValueError, UnicodeError) as error:
        parse_error = str(error)
    clip_count = len(manifest_lines)
    run.update({"finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "exit_code": result.returncode, "rows": len(rows), "expected_rows": 1 + 3 * clip_count,
                "error_rows": sum(row["error"] != 0 for row in rows),
                "status": "complete" if result.returncode == 0 and len(rows) == 1 + 3 * clip_count and not parse_error else "failed"})
    if all(metadata["runs"].get(model, {}).get("status") == "complete"
           for model in ("qwen3-asr-0.6b", "qwen3-asr-1.7b")):
        metadata["status"] = "complete"
    else:
        metadata["status"] = "partially_complete" if run["status"] == "complete" else "failed"
    if run["status"] == "failed":
        metadata["errors"].append({"model": args.model, "exit_code": result.returncode,
                                    "log": run["log"], "parse_error": parse_error})
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(run, indent=2))
    raise SystemExit(0 if run["status"] == "complete" else 1)


if __name__ == "__main__":
    main()
