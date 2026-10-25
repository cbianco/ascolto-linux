#!/usr/bin/env python3
"""Use the configured OpenRouter credential in memory for an explicit STT trial."""
import argparse
import base64
import datetime
import hashlib
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from ai_providers import ProviderManager, ProviderError

MODEL = "openai/whisper-large-v3-turbo"
ENDPOINT = "https://openrouter.ai/api/v1/audio/transcriptions"


def private_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2))
    os.chmod(path, 0o600)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path("build/stt-bench"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    output = args.root / "cloud-reference"
    output.mkdir(parents=True, exist_ok=True)
    os.chmod(output, 0o700)
    manager = ProviderManager(args.config)
    # The OpenRouter account identifier is fixed. Avoid loading/saving settings
    # or migrating the user's SQLite database for this isolated experiment.
    credential = manager.get_credential("openrouter", {"profiles": {"openrouter": {}}})
    token = credential["key"]
    headers = {"X-Title": "Ascolto", "User-Agent": "Ascolto/1.0 (user-authorized STT benchmark)"}
    catalog = manager.transport.request("https://openrouter.ai/api/v1/models?output_modalities=transcription", token, headers=headers)
    dto = next((m for m in catalog.get("data", []) if m.get("id") == MODEL), None)
    if not dto:
        raise ProviderError("model_unavailable", "The requested model is absent from the current transcription catalog.")
    private_json(output / "catalog.json", dto)
    print(json.dumps({"model": MODEL, "credential_loaded": True, "catalog_verified": True,
                      "pricing": dto.get("pricing"), "check_only": args.check}), flush=True)
    if args.check:
        return
    manifest = json.loads((args.root / "corpus/manifest.json").read_text())
    destination = output / "responses.jsonl"
    existing = {}
    if destination.exists():
        for line in destination.read_text().splitlines():
            item = json.loads(line)
            if item.get("status") == "completed":
                existing[item["clip"]] = item
    for clip in manifest["clips"]:
        if clip["id"] in existing:
            continue
        payload = Path(clip["wav"]).read_bytes()
        body = {"model": MODEL, "input_audio": {"data": base64.b64encode(payload).decode("ascii"), "format": "wav"},
                "language": "it", "temperature": 0, "response_format": "json"}
        started = time.monotonic()
        item = {"clip": clip["id"], "model": MODEL, "provider": "openrouter",
                "audio_s": clip["duration_s"], "audio_sha256": hashlib.sha256(payload).hexdigest(),
                "original_pcm_sha256": clip["sha256"], "language_requested": "it",
                "requested_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
        try:
            data = manager.transport.request(ENDPOINT, token, body=body, headers=headers)
            if not isinstance(data.get("text"), str):
                raise ProviderError("invalid_response", "The provider returned no transcription text.")
            # Store only the intended transcript and non-secret API result fields.
            item.update(status="completed", text=data["text"], response=data)
        except ProviderError as error:
            item.update(status="failed", error_code=error.code)
        item["request_wall_ms"] = (time.monotonic()-started)*1000
        with destination.open("a") as out:
            out.write(json.dumps(item, ensure_ascii=False) + "\n")
        os.chmod(destination, 0o600)
        print(json.dumps({k: item[k] for k in ("clip", "status", "request_wall_ms")}), flush=True)
        if item["status"] != "completed":
            # No silent retry, model substitution, or provider credential fallback.
            raise ProviderError(item["error_code"], "Cloud transcription stopped; completed clips have been retained.")
    print(json.dumps({"completed": True, "clips": len(manifest["clips"]),
                      "uploaded_audio_seconds": sum(c["duration_s"] for c in manifest["clips"])}), flush=True)


if __name__ == "__main__":
    try:
        main()
    except ProviderError as error:
        print(json.dumps({"status": "failed", "error_code": error.code, "message": str(error)}))
        raise SystemExit(1)
