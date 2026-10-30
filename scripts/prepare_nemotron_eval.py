#!/usr/bin/env python3
"""Prepare a fixed public LibriSpeech ASR evaluation without model inference.

Uses only Python's standard library and ffmpeg. Selection uses source metadata,
never recognizer outputs. Archives remain under the ignored build/ directory.
Run: python3 scripts/prepare_nemotron_eval.py
"""
import argparse
from array import array
import hashlib
import itertools
import json
import math
import platform
from pathlib import Path, PurePosixPath
import random
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import wave

SOURCE = "https://www.openslr.org/12/"
MIRROR = "https://openslr.trmal.net/resources/12/"
ARCHIVES = {"test-clean": "32fa31d27d2e1cad72775fee3f4849a9",
            "test-other": "fb5a50374b501bb3bac4815ee91d3135"}
SEED = "ascolto-nemotron-eval-v1"
RATE = 16000


def digest(path, algorithm="sha256"):
    h = hashlib.new(algorithm)
    with path.open("rb") as src:
        for block in iter(lambda: src.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def safe_member(member):
    path = PurePosixPath(member.name)
    if path.is_absolute() or ".." in path.parts or member.issym() or member.islnk():
        raise ValueError("Unsafe archive member: " + member.name)
    return member.isfile()


def flac_duration(header):
    """Read the mandatory FLAC STREAMINFO header without decoding audio."""
    if len(header) < 42 or header[:4] != b"fLaC" or header[4] & 127 != 0:
        raise ValueError("Missing FLAC STREAMINFO")
    value = int.from_bytes(header[18:26], "big")
    rate, samples = value >> 44, value & ((1 << 36) - 1)
    if not rate or not samples:
        raise ValueError("Invalid FLAC sample count")
    return samples / rate


def scan_archive(path, split):
    clips, transcripts = [], {}
    with tarfile.open(path, "r:gz") as archive:
        for member in archive:
            if not safe_member(member):
                continue
            if member.name.endswith(".trans.txt"):
                for line in archive.extractfile(member).read().decode("utf-8").splitlines():
                    identifier, text = line.split(" ", 1)
                    if "\t" in text:
                        raise ValueError("Transcript contains a tab")
                    transcripts[identifier] = text.strip()
            elif member.name.endswith(".flac"):
                identifier = PurePosixPath(member.name).stem
                duration = flac_duration(archive.extractfile(member).read(42))
                clips.append(dict(id=identifier, member=member.name, split=split,
                                  speaker=identifier.split("-")[0], duration_s=duration))
    for clip in clips:
        clip["expected_text"] = transcripts[clip["id"]]
    return clips


def select_clips(clips, count):
    """Half 3–8s, half 8–20s; one clip per speaker; seeded hash order."""
    ordered = sorted(clips, key=lambda clip: hashlib.sha256(
        (SEED + ":" + clip["id"]).encode()).hexdigest())
    selected, speakers = [], set()
    for minimum, maximum in ((3, 8), (8, 20)):
        for clip in ordered:
            if minimum <= clip["duration_s"] < maximum and clip["speaker"] not in speakers:
                selected.append(dict(clip))
                speakers.add(clip["speaker"])
                if len(selected) == (count // 2 if minimum == 3 else count):
                    break
        required = count // 2 if minimum == 3 else count
        if len(selected) != required:
            raise ValueError("Insufficient distinct speakers in duration bucket")
    if len(selected) != count:
        raise ValueError("Insufficient distinct speakers for fixed selection")
    return selected


def read_pcm(path):
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, RATE):
            raise ValueError("Expected mono PCM16 at 16 kHz")
        data = array("h", wav.readframes(wav.getnframes()))
    if sys.byteorder != "little":
        data.byteswap()
    return data


def write_pcm(path, samples):
    data = array("h", samples)
    if sys.byteorder != "little":
        data.byteswap()
    with wave.open(str(path), "wb") as wav:
        wav.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        wav.writeframes(data.tobytes())


def scaled_pcm(samples):
    peak = max((abs(x) for x in samples), default=0)
    gain = min(1.0, 32760 / peak) if peak else 1.0
    return array("h", (round(x * gain) for x in samples)), gain


def add_noise(samples, seed, snr_db=10):
    rng = random.Random(seed)
    noise = [rng.gauss(0, 1) for _ in samples]
    energy = sum(x * x for x in samples)
    noise_energy = sum(x * x for x in noise)
    scale = math.sqrt(energy / (10 ** (snr_db / 10) * noise_energy))
    return scaled_pcm([x + n * scale for x, n in zip(samples, noise)])


def write_manifest(path, clips):
    with path.open("w", encoding="utf-8", newline="\n") as out:
        out.write("id\tpath\tlanguage\texpected_text\n")
        for clip in clips:
            values = [clip["id"], clip["path"], "en", clip["expected_text"]]
            if any("\t" in x or "\n" in x or "\r" in x for x in values):
                raise ValueError("Manifest fields must not contain tabs or newlines")
            out.write("\t".join(values) + "\n")


def prepare(root, ffmpeg):
    root.mkdir(parents=True, exist_ok=True)
    archives = root / "archives"
    archives.mkdir(exist_ok=True)
    sources, selected = [], []
    for split, expected_md5 in ARCHIVES.items():
        path = archives / (split + ".tar.gz")
        url = MIRROR + path.name
        if not path.exists():
            temporary = path.with_suffix(path.suffix + ".part")
            print("Downloading " + url, flush=True)
            with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as out:
                shutil.copyfileobj(response, out, 1024 * 1024)
            temporary.replace(path)
        if digest(path, "md5") != expected_md5:
            raise ValueError("Official archive MD5 mismatch: " + str(path))
        sources.append(dict(split=split, url=url, bytes=path.stat().st_size,
                            md5=expected_md5, sha256=digest(path)))
        print("Verified " + path.name + "; selecting from source metadata", flush=True)
        selected.extend(select_clips(scan_archive(path, split), 12 if split == "test-clean" else 8))
    # Write the selection before decoding or any recognizer invocation.
    (root / "selection.json").write_text(json.dumps(selected, indent=2) + "\n")
    audio = root / "audio"
    audio.mkdir(exist_ok=True)
    for split in ARCHIVES:
        wanted = {clip["member"]: clip for clip in selected if clip["split"] == split}
        with tarfile.open(archives / (split + ".tar.gz"), "r:gz") as archive:
            for member in archive:
                if not safe_member(member) or member.name not in wanted:
                    continue
                clip = wanted[member.name]
                source = audio / (clip["id"] + ".flac")
                with source.open("wb") as out:
                    shutil.copyfileobj(archive.extractfile(member), out)
                target = audio / (clip["id"] + ".wav")
                subprocess.run([ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-i", str(source),
                                "-ar", str(RATE), "-ac", "1", "-c:a", "pcm_s16le", str(target)], check=True)
                clip.update(path=str(target.resolve()), condition=split, score=True,
                            source_sha256=digest(source), sha256=digest(target))
    clips = list(selected)
    # Known-text long turns retain whole source utterances, with 250ms silence.
    used = set()
    for index in range(2):
        choices = []
        for size in range(3, 7):
            for combination in itertools.combinations(range(len(selected)), size):
                duration = sum(selected[i]["duration_s"] for i in combination) + .25 * (size - 1)
                if 45 <= duration <= 55:
                    choices.append((len(used.intersection(combination)), abs(duration - 50), combination))
        _, _, chosen = min(choices)
        used.update(chosen)
        samples, segments = array("h"), []
        for i in chosen:
            if samples:
                samples.extend([0] * (RATE // 4))
            start = len(samples) / RATE
            samples.extend(read_pcm(Path(selected[i]["path"])))
            segments.append(dict(source_id=selected[i]["id"], start_s=start, end_s=len(samples)/RATE))
        target = audio / ("long-%d.wav" % (index + 1))
        write_pcm(target, samples)
        clips.append(dict(id=target.stem, path=str(target.resolve()), condition="long", score=True,
                          duration_s=len(samples)/RATE, expected_text=" ".join(selected[i]["expected_text"] for i in chosen),
                          segments=segments, sha256=digest(target)))
    for index, source in enumerate((selected[0], selected[12])):
        seed = 20261030 + index
        samples, gain = add_noise(read_pcm(Path(source["path"])), seed)
        target = audio / ("noise-10db-%d.wav" % (index + 1))
        write_pcm(target, samples)
        clips.append(dict(id=target.stem, path=str(target.resolve()), condition="noise-10db", score=True,
                          duration_s=len(samples)/RATE, expected_text=source["expected_text"], source_id=source["id"],
                          seed=seed, snr_db=10, gain=gain, sha256=digest(target)))
    for index, (left, right) in enumerate(((selected[6], selected[7]), (selected[16], selected[17]))):
        a, b = read_pcm(Path(left["path"])), read_pcm(Path(right["path"]))
        offset = RATE
        mixed = [0.0] * max(len(a), len(b) + offset)
        for i, value in enumerate(a):
            mixed[i] += value
        for i, value in enumerate(b):
            mixed[i + offset] += value
        samples, gain = scaled_pcm(mixed)
        target = audio / ("overlap-%d.wav" % (index + 1))
        write_pcm(target, samples)
        clips.append(dict(id=target.stem, path=str(target.resolve()), condition="overlap", score=False,
                          score_exclusion="Simultaneous speakers have no single canonical token order",
                          duration_s=len(samples)/RATE, expected_text="", gain=gain,
                          sources=[dict(id=left["id"], start_s=0, expected_text=left["expected_text"]),
                                   dict(id=right["id"], start_s=1, expected_text=right["expected_text"])],
                          sha256=digest(target)))
    write_manifest(root / "manifest.tsv", clips)
    playlist = selected + [clip for clip in clips if clip["condition"] in ("noise-10db", "long")]
    write_manifest(root / "playlist.tsv", playlist)
    ffmpeg_version = subprocess.run([ffmpeg, "-version"], check=True, capture_output=True, text=True).stdout.splitlines()[0]
    metadata = dict(schema_version=1, preparation_python=platform.python_version(),
                    preparation_ffmpeg=ffmpeg_version, corpus="LibriSpeech SLR12", source=SOURCE,
                    license="CC BY 4.0", license_url="https://creativecommons.org/licenses/by/4.0/",
                    attribution="Vassil Panayotov, Guoguo Chen, Daniel Povey and Sanjeev Khudanpur, LibriSpeech (2015)",
                    selection_seed=SEED, selection_policy="Hash order; distinct speakers per split; half 3–8s and half 8–20s; no inference-based selection",
                    archive_checksums_source=MIRROR + "md5sum.txt", archives=sources,
                    audio_format="16000 Hz mono signed PCM16 little-endian WAV", clips=clips,
                    playlist_ids=[clip["id"] for clip in playlist],
                    limits=["English read audiobooks are not conversational calls or an Italian accuracy benchmark",
                            "Two seeded white-noise mixtures are controlled robustness probes, not real noise coverage",
                            "Overlap probes are excluded from WER/CER because token order is ambiguous",
                            "The subset is small and not a full LibriSpeech test-set score"])
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(dict(manifest=str(root / "manifest.tsv"), clips=len(clips),
                          scored_clips=sum(clip["score"] for clip in clips),
                          seconds=sum(clip["duration_s"] for clip in clips), playlist_turns=len(playlist)), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/nemotron-eval"))
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg")
    args = parser.parse_args()
    prepare(args.output.resolve(), args.ffmpeg)


if __name__ == "__main__":
    main()
