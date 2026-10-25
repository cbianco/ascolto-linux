#!/usr/bin/env python3
"""Run one native benchmark at a time, retain logs and an execution ledger."""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("build/stt-bench"))
    parser.add_argument("--id", required=True)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    results = args.root / "results"
    results.mkdir(parents=True, exist_ok=True)
    os.chmod(args.root, 0o700)
    os.chmod(results, 0o700)
    with (args.root / "inference.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        started = datetime.datetime.now(datetime.timezone.utc).isoformat()
        start = time.monotonic()
        status = "completed"
        env = dict(os.environ, VECLIB_MAXIMUM_THREADS="8", OMP_NUM_THREADS="8", OPENBLAS_NUM_THREADS="8")
        with (results / (args.id + ".jsonl")).open("w") as out, (results / (args.id + ".log")).open("w") as log:
            try:
                proc = subprocess.run(command, env=env, stdout=out, stderr=log, timeout=args.timeout)
                code = proc.returncode
                if code:
                    status = "failed"
            except subprocess.TimeoutExpired:
                status, code = "timeout", -1
            except OSError as error:
                status, code = "launch_failed", -2
                print(str(error), file=log)
        entry = dict(id=args.id, started_utc=started, elapsed_s=time.monotonic()-start,
                     command=command, status=status, returncode=code,
                     environment={k: env[k] for k in ("VECLIB_MAXIMUM_THREADS", "OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "GGML_BACKEND_PATH", "PARAKEET_DEVICE", "PARAKEET_REPACK_THREADS", "DYLD_LIBRARY_PATH") if k in env})
        (results / (args.id + "-run.json")).write_text(json.dumps(entry, indent=2))
        with (results / "execution-ledger.jsonl").open("a") as ledger:
            ledger.write(json.dumps(entry) + "\n")
        for path in results.glob(args.id + "*"):
            if path.is_file():
                os.chmod(path, 0o600)
        count = sum(1 for line in (results / (args.id + ".jsonl")).open() if line.startswith('{"model"'))
        print(json.dumps(dict(id=args.id, status=status, returncode=code, seconds=round(entry["elapsed_s"], 2), rows=count)))
        raise SystemExit(0 if status == "completed" else 1)


if __name__ == "__main__":
    main()
