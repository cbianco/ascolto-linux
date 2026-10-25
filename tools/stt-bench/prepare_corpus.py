#!/usr/bin/env python3
"""Prepare private, matched PCM clips without changing source call archives."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import wave

import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    os.chmod(args.output, 0o700)
    inventory = []
    for path in sorted(args.source.rglob("recording.m4a")):
        proc = subprocess.run(["ffprobe", "-v", "quiet", "-show_format", "-show_streams", "-of", "json", str(path)], capture_output=True)
        try:
            data = json.loads(proc.stdout)
            duration = float(data["format"]["duration"])
            if data["streams"][0]["channels"] == 2:
                inventory.append((duration, path))
        except (KeyError, ValueError):
            continue
    selected = sorted(inventory, reverse=True)[:3]
    clips = []

    def save(identifier, pcm, metadata):
        raw = args.output / (identifier + ".f32")
        wav = args.output / (identifier + ".wav")
        pcm = np.asarray(pcm, dtype="<f4")
        raw.write_bytes(pcm.tobytes())
        with wave.open(str(wav), "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(16000)
            out.writeframes((np.clip(pcm, -1, 1) * 32767).astype("<i2").tobytes())
        for file in (raw, wav):
            os.chmod(file, 0o600)
        clips.append(dict(id=identifier, raw=str(raw.resolve()), wav=str(wav.resolve()),
                          duration_s=len(pcm) / 16000, sha256=hashlib.sha256(pcm.tobytes()).hexdigest(),
                          rms_db=float(20 * np.log10(np.sqrt(np.mean(pcm**2)) + 1e-12)), **metadata))

    for session, (duration, path) in enumerate(selected, 1):
        # Decode once. Search only after the introduction and before the ending.
        decoded = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(path), "-ar", "16000", "-ac", "2", "-f", "f32le", "-"])
        stereo = np.frombuffer(decoded, dtype="<f4").reshape(-1, 2)
        for channel, name in enumerate(("MIC", "SYS")):
            samples = stereo[:, channel]
            candidates = []
            for start in range(int(duration * .12), int(duration * .82) - 10, 5):
                window = samples[start*16000:(start+10)*16000]
                # Favor sustained activity rather than an isolated amplitude peak.
                bins = window.reshape(20, -1)
                powers = np.sqrt(np.mean(bins**2, axis=1))
                score = float(np.mean(powers > .004) + np.median(powers))
                candidates.append((score, start))
            _, start = max(candidates)
            save(f"call{session:02d}-{name.lower()}-10s", samples[start*16000:(start+10)*16000],
                 dict(channel=name, session=f"call{session:02d}", source=str(path), source_start_s=start, kind="real_call"))
            if session == 1 and name == "SYS":
                long_start = min(start, int(duration) - 30)
                save("call01-sys-30s", samples[long_start*16000:(long_start+30)*16000],
                     dict(channel=name, session="call01", source=str(path), source_start_s=long_start, kind="real_call"))
    save("silence-5s", np.zeros(80000, dtype="<f4"), dict(channel="NONE", session=None, source=None, source_start_s=0, kind="silence"))
    (args.output / "manifest.json").write_text(json.dumps(dict(sample_rate=16000, clips=clips), ensure_ascii=False, indent=2))
    (args.output / "manifest.tsv").write_text("".join(f"{c['id']}\t{c['raw']}\n" for c in clips))
    for name in ("manifest.json", "manifest.tsv"):
        os.chmod(args.output / name, 0o600)
    print(json.dumps(dict(readable_archives=len(inventory), clips=len(clips), seconds=sum(c["duration_s"] for c in clips), ids=[c["id"] for c in clips])))


if __name__ == "__main__":
    main()
