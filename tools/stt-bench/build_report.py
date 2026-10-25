#!/usr/bin/env python3
"""Build a private, self-contained Italian HTML report from measured JSONL."""
import argparse
import base64
import datetime
import json
import os
from pathlib import Path


MODEL_INFO = {
    "whistle": ("Whistle · attuale", "C API / core C++", "CPU", "Riferimento attuale; word timestamps attivi"),
    "whistle-with-embeddings": ("Whistle + embedding", "C API / core C++", "CPU", "ASR più estrazione embedding; escluso clustering speaker"),
    "whisper-turbo-f16": ("Whisper turbo F16", "C API / core C++", "Metal", "Greedy, 8 thread, italiano, contesto azzerato"),
    "whisper-turbo-q5": ("Whisper turbo Q5", "C API / core C++", "Metal", "Quantizzazione Q5_0; stesse opzioni F16"),
    "whisper-large-v3-f16": ("Whisper large-v3 F16", "C API / core C++", "Metal", "Greedy, 8 thread, italiano"),
    "qwen3-asr-0.6b": ("Qwen3-ASR 0.6B", "C", "CPU / Accelerate", "Greedy, italiano, intero turno"),
    "qwen3-asr-1.7b": ("Qwen3-ASR 1.7B", "C", "CPU / Accelerate", "Greedy, italiano, intero turno"),
    "nemotron-3.5": ("Nemotron 3.5 0.6B", "C API / core C++", "Metal", "NeMo-Speech.cpp; input completo"),
    "nemotron-streaming": ("Nemotron 3.5 streaming", "C API / core C++", "Metal", "Blocchi da 20 ms senza pacing; contesto destro 320 ms"),
    "parakeet-v3": ("Parakeet TDT v3", "C API / core C++", "Metal", "Q8; parakeet.cpp"),
    "parakeet-v3-cpu": ("Parakeet v3 CPU", "C API / core C++", "CPU", "Q8; prima esecuzione con fallback CPU osservato"),
    "parakeet-redux": ("Parakeet Redux", "C API / core C++", "CPU", "Ternario packed; parakeet.cpp"),
    "parakeet-ultra": ("Parakeet Ultra", "C API / core C++", "Metal", "Q8; parakeet.cpp"),
    "parakeet-ultra-cpu": ("Parakeet Ultra CPU", "C API / core C++", "CPU", "Q8; prima esecuzione con fallback CPU osservato"),
    "voxtral": ("Voxtral · feed intero", "C / Obj-C / Metal", "Metal", "Tentativo diagnostico: limite KV encoder sul clip 30 s, errore non propagato dall’API; escluso dai confronti che includono quel clip"),
    "voxtral-stream": ("Voxtral streaming 2 s", "C / Obj-C / Metal", "Metal", "Blocchi da 2 s senza pacing; ritardo modello 480 ms"),
    "cohere": ("Cohere Transcribe 2B", "C API / core C++", "CPU / ONNX", "sherpa-onnx INT8; italiano"),
}
ALIASES = {
    "nemo-nemotron-3.5-q8_0-metal": "nemotron-3.5",
    "nemo-nemotron-3.5-q8_0-metal-streaming": "nemotron-streaming",
    "parakeet-v3-q8_0-metal": "parakeet-v3",
    "parakeet-ultra-q8_0-metal": "parakeet-ultra",
    "parakeet-redux-packed-cpu": "parakeet-redux",
    "voxtral-mini-4b-realtime": "voxtral",
    "cohere-transcribe-2b-int8": "cohere",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("build/stt-bench"))
    parser.add_argument("--output", type=Path, help="Optional durable private report directory")
    args = parser.parse_args()
    root = args.root
    rows, metadata, runs, rejected = [], {}, [], []
    for path in sorted((root / "results").glob("*.jsonl")):
        for line in path.read_text().splitlines():
            try:
                row = json.loads(line)
                if not all(k in row for k in ("model", "clip", "total_ms", "audio_s", "repeat")):
                    continue
                # Needle returns its original result JSON as the text field.
                if row["model"].startswith("whistle"):
                    try:
                        needle = json.loads(row["text"])
                        row["text"] = needle.get("text", "")
                        row["decode_tps"] = needle.get("decode_tps")
                    except (ValueError, TypeError):
                        pass
                row["source_file"] = path.name
                row["original_model"] = row["model"]
                row["model"] = ALIASES.get(row["model"], row["model"])
                if path.name == "voxtral-stream.jsonl":
                    row["model"] = "voxtral-stream"
                if path.name in ("parakeet-v3-q8_0-cpu.jsonl", "parakeet-ultra-q8_0-cpu.jsonl"):
                    row["model"] = "parakeet-v3-cpu" if "-v3-" in path.name else "parakeet-ultra-cpu"
                    row["backend"] = "parakeet.cpp CPU (fallback observed)"
                row["first_event_ms"] = row.get("first_token_ms")
                row["first_event_kind"] = row.get("first_kind", "unavailable")
                if row["model"] == "voxtral" and row["clip"] == "call01-sys-30s":
                    row["api_error"] = row["error"]
                    row["validation_error"] = "Encoder shared KV capacity exceeded; audio discarded without API error. See runtime stderr."
                    row["error"] = -100
                rows.append(row)
            except (ValueError, TypeError):
                rejected.append(path.name)
    for path in sorted((root / "results").glob("*.json")):
        try:
            value = json.loads(path.read_text())
            if path.name.endswith("-run.json"):
                runs.append(value)
            else:
                metadata[path.name] = value
        except ValueError:
            rejected.append(path.name)
    # A polled partial is useful, but it is not a token-generation callback.
    for sidecar in sorted((root / "results").glob("voxtral*extra.jsonl")):
        model = "voxtral-stream" if "stream" in sidecar.name else "voxtral"
        for line in sidecar.read_text().splitlines():
            event = json.loads(line)
            if event.get("event") != "inference":
                continue
            for row in rows:
                if row["model"] == model and row["clip"] == event.get("clip") and row["repeat"] == event.get("repeat"):
                    row["gpu_allocated_bytes"] = event.get("gpu_allocated_bytes")
                    if event.get("chunk_samples"):
                        row["first_event_ms"] = event.get("first_observed_result_ms")
                        row["first_event_kind"] = "incremental_text_poll"
    manifest = json.loads((root / "corpus" / "manifest.json").read_text())
    # Keep private source paths in the local manifest, not in the portable HTML.
    clips = [{k: v for k, v in c.items() if k not in ("source", "raw", "wav")} for c in manifest["clips"]]
    for clip, original in zip(clips, manifest["clips"]):
        clip["audio_data"] = "data:audio/wav;base64," + base64.b64encode(Path(original["wav"]).read_bytes()).decode("ascii")
    payload = dict(created=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                   hardware=dict(chip="Apple M3 Max", cpu="14 core · 10 performance + 4 efficiency", gpu="30 core Metal", memory="36 GB unificata"),
                   rows=rows, clips=clips, models=MODEL_INFO, metadata=metadata, runs=runs,
                   rejected=rejected)
    comparison = root / "cloud-reference/comparison.json"
    payload["cloud"] = json.loads(comparison.read_text()) if comparison.exists() else None
    output = args.output or root
    output.mkdir(parents=True, exist_ok=True)
    os.chmod(output, 0o700)
    (output / "report-data.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2))
    template = Path(__file__).with_name("report_template.html").read_text()
    # Escape script-closing text and other markup in transcript/model metadata.
    data = json.dumps(payload, ensure_ascii=False).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    (output / "report.html").write_text(template.replace("__BENCHMARK_DATA__", data))
    for name in ("report.html", "report-data.json"):
        os.chmod(output / name, 0o600)
    print(json.dumps(dict(rows=len(rows), measured_models=len({r['model'] for r in rows}), output=str((output / "report.html").resolve()))))


if __name__ == "__main__":
    main()
