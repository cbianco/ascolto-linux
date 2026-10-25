#!/usr/bin/env python3
"""Download pinned official Qwen weights and record reproducible provenance."""
import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[3]
WORK = ROOT / "build/stt-bench"
METADATA = WORK / "results/qwen-metadata.json"
RUNTIME_PIN = "924694251d9e0f18e5d86bbd06aa3ab5f870002d"
MODELS = {
    "qwen3-asr-0.6b": ("Qwen/Qwen3-ASR-0.6B", "5eb144179a02acc5e5ba31e748d22b0cf3e303b0"),
    "qwen3-asr-1.7b": ("Qwen/Qwen3-ASR-1.7B", "7278e1e70fe206f11671096ffdd38061171dd6e5"),
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(16 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def save(metadata):
    METADATA.parent.mkdir(parents=True, exist_ok=True)
    METADATA.write_text(json.dumps(metadata, indent=2) + "\n")


def prepare(download=True):
    previous = json.loads(METADATA.read_text()) if METADATA.exists() else {}
    metadata = {
        "updated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "runtime": {
            "repository": "https://github.com/antirez/qwen-asr",
            "commit": command("git", "-C", str(WORK / "runtimes/qwen-asr"), "rev-parse", "HEAD"),
            "license": "MIT",
            "compiler": command("clang", "--version"),
            "build_command": "sh tools/stt-bench/qwen/build.sh",
            "cflags": "-Wall -Wextra -O3 -march=native -ffast-math -DUSE_BLAS -DACCELERATE_NEW_LAPACK",
            "backend": "CPU Accelerate/NEON",
            "implementation": "C; Apple Accelerate BLAS; no Python inference",
        },
        "configuration": {
            "threads": 8,
            "VECLIB_MAXIMUM_THREADS": "8",
            "sample_rate": 16000,
            "channels": 1,
            "input": "float32 PCM loaded before model initialization",
            "mode": "completed clip, qwen_transcribe_audio",
            "language": "Italian",
            "decoder": "greedy, upstream maximum 2048 generated tokens per clip",
            "precision": "official BF16 safetensors; runtime FP32 encoder and BF16 decoder weights",
            "segment_sec": 0,
            "skip_silence": False,
            "past_text_conditioning": False,
            "prompt": None,
            "warmup": "first manifest clip once with repeat=-1",
            "measured_passes": 3,
            "model_loads_per_process": 1,
            "measurement": "monotonic full call wall time, process user+system CPU, getrusage peak RSS",
            "first_token": "first nonempty output token callback from complete-clip decode",
            "word_timestamps": False,
        },
        "models": [],
        "status": "preparing",
        "errors": previous.get("errors", []),
        "runs": previous.get("runs", {}),
    }
    if metadata["runtime"]["commit"] != RUNTIME_PIN:
        raise RuntimeError("Runtime commit does not match the fixed benchmark revision")
    save(metadata)
    for name, (repository, revision) in MODELS.items():
        model_dir = WORK / "models" / name
        model_dir.mkdir(parents=True, exist_ok=True)
        api = f"https://huggingface.co/api/models/{repository}/revision/{revision}?blobs=true"
        with urllib.request.urlopen(api) as response:
            upstream = json.load(response)
        (model_dir / "upstream-api.json").write_text(json.dumps(upstream, indent=2) + "\n")
        files = []
        for item in upstream["siblings"]:
            filename = item["rfilename"]
            if filename == ".gitattributes":
                continue
            destination = model_dir / filename
            expected_hash = item.get("lfs", {}).get("sha256")
            url = f"https://huggingface.co/{repository}/resolve/{revision}/{filename}"
            if download and (not destination.exists() or destination.stat().st_size != item["size"]):
                partial = destination.with_suffix(destination.suffix + ".part")
                print(f"Downloading {repository}/{filename}", flush=True)
                subprocess.run(["curl", "-fL", "--retry", "4", "--retry-all-errors", "--continue-at", "-",
                                "--output", str(partial), url], check=True)
                partial.replace(destination)
            if not destination.exists():
                raise RuntimeError(f"Missing model file: {destination}")
            digest = sha256(destination)
            if destination.stat().st_size != item["size"] or (expected_hash and digest != expected_hash):
                raise RuntimeError(f"Model file verification failed: {destination}")
            files.append({"path": str(destination.relative_to(ROOT)), "filename": filename,
                          "bytes": destination.stat().st_size, "sha256": digest,
                          "official_lfs_sha256": expected_hash, "source_url": url})
        metadata["models"].append({"id": name, "repository": f"https://huggingface.co/{repository}",
                                   "revision": revision, "license": "Apache-2.0",
                                   "weight_bytes": sum(f["bytes"] for f in files if f["filename"].endswith(".safetensors")),
                                   "files": files})
        save(metadata)
    binary = WORK / "runtimes/qwen-bench"
    if binary.exists():
        metadata["runtime"]["binary_sha256"] = sha256(binary)
        metadata["runtime"]["binary_bytes"] = binary.stat().st_size
        metadata["runtime"]["binary_libraries"] = command("otool", "-L", str(binary))
    metadata["runtime"]["harness_sha256"] = sha256(ROOT / "tools/stt-bench/qwen/bench.c")
    metadata["status"] = "ready" if binary.exists() else "weights_ready"
    save(metadata)
    print(f"Saved {METADATA}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    try:
        prepare(download=not args.verify_only)
    except Exception as error:
        metadata = json.loads(METADATA.read_text()) if METADATA.exists() else {}
        metadata["status"] = "preparation_failed"
        metadata.setdefault("errors", []).append({"stage": "preparation", "message": str(error)})
        save(metadata)
        print(str(error), file=sys.stderr)
        raise
