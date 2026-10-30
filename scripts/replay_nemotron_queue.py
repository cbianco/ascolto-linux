#!/usr/bin/env python3
"""Replay measured ASR delays through the real portable capture queue.

This accelerated, deterministic workload uses synthetic indexed 20 ms frames,
not microphones, physical capture, the VAD, or live ASR. A small temporary C
library compiles src/capture_buffer.c unchanged. No speech weights are loaded.

Example:
  python3 scripts/replay_nemotron_queue.py --manifest build/nemotron-eval/manifest.tsv \
    --results build/nemotron-eval/run.jsonl --duration-seconds 1800 --output build/queue.json
"""
import argparse
import csv
import ctypes
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import wave


ROOT = Path(__file__).resolve().parents[1]
RATE, FRAME, FRAME_NS = 16000, 320, 20_000_000
MISSING_NS = 300_000_000


def production_capacity_samples(root=ROOT):
    """Read both production call sites so the replay cannot silently drift."""
    audio_types = (root / "include/ascolto/audio_types.h").read_text()
    sample_rate = re.search(r"#define ASCOLTO_SAMPLE_RATE (\d+)", audio_types)
    frame_samples = re.search(r"#define ASCOLTO_FRAME_SAMPLES (\d+)", audio_types)
    if not sample_rate or not frame_samples or (int(sample_rate[1]), int(frame_samples[1])) != (RATE, FRAME):
        raise ValueError("Production capture sample clock changed")
    mac = (root / "src/capture_macos.c").read_text()
    linux = (root / "src/audio_backend.c").read_text()
    first = re.search(r"#define CAP_RING_CAPACITY \(ASCOLTO_SAMPLE_RATE \* (\d+)\)", mac)
    second = re.search(r"ascolto_audio_queue_start\(ASCOLTO_SAMPLE_RATE \* (\d+),", linux)
    if not first or not second or first[1] != second[1]:
        raise ValueError("Production capture queue capacities changed or differ")
    return RATE * int(first[1])


@dataclass(frozen=True)
class Timing:
    identifier: str
    audio_frames: int
    elapsed_ns: int
    failed: bool = False


def nonnegative_number(record, *keys):
    for key in keys:
        value = record.get(key)
        if isinstance(value, str):
            try:
                value = float(value)
            except ValueError:
                continue
        if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0:
            return float(value)
    return None


def load_timings(manifest, results, metadata=None):
    manifest, results = Path(manifest), Path(results)
    clips = {}
    with manifest.open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            identifier = row.get("id")
            if not identifier or identifier in clips or None in row:
                raise ValueError("Invalid or duplicate manifest identifier")
            clips[identifier] = row
    information = {}
    metadata = Path(metadata) if metadata else manifest.with_name("metadata.json")
    if metadata.exists():
        information = {clip["id"]: clip for clip in json.loads(metadata.read_text())["clips"]}
    timings = []
    with results.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            if not line.strip():
                continue
            record = json.loads(line)
            if not isinstance(record, dict):
                raise ValueError("Expected timing object at line %d" % line_number)
            identifier = record.get("id")
            if identifier is None:
                continue
            if identifier not in clips:
                raise ValueError("Timing identifier absent from manifest: " + str(identifier))
            elapsed = nonnegative_number(record, "elapsed_s", "latency_s", "decode_s")
            if elapsed is None:
                milliseconds = nonnegative_number(record, "elapsed_ms", "latency_ms", "decode_ms")
                elapsed = milliseconds / 1000 if milliseconds is not None else None
            duration = nonnegative_number(record, "audio_s", "duration_s")
            if duration is None:
                duration = nonnegative_number(information.get(identifier, {}), "duration_s")
            if duration is None:
                duration = nonnegative_number(clips[identifier], "duration_s")
            if duration is None and clips[identifier].get("path"):
                path = Path(clips[identifier]["path"])
                if not path.is_absolute():
                    path = manifest.parent / path
                with wave.open(str(path), "rb") as audio:
                    duration = audio.getnframes() / audio.getframerate()
            if elapsed is None or duration is None or not 0 < duration <= 60:
                raise ValueError("Missing/invalid duration or elapsed timing: " + str(identifier))
            timings.append(Timing(str(identifier), max(1, math.ceil(duration * RATE / FRAME)),
                                  round(elapsed * 1e9), record.get("ok") is False or bool(record.get("error"))))
    if not timings:
        raise ValueError("No measured clip timings")
    return timings


class NativeQueue:
    """Own only a bounded C ring and reusable one-frame input/output buffers."""
    def __init__(self, capacity, library):
        self.library = library
        self.pointer = library.ascolto_capture_buffer_create(capacity)
        if not self.pointer:
            raise ValueError("Native capture queue rejected capacity")
        self.producer = (ctypes.c_float * FRAME)()
        self.mic, self.sys = (ctypes.c_float * FRAME)(), (ctypes.c_float * FRAME)()

    def close(self):
        if self.pointer:
            self.library.ascolto_capture_buffer_free(self.pointer)
            self.pointer = None

    def push(self, channel, frame):
        payload = frame_bytes(frame)
        ctypes.memmove(self.producer, payload, len(payload))
        self.library.ascolto_capture_buffer_push(self.pointer, channel, self.producer, FRAME, frame * FRAME)

    def available(self, channel):
        return self.library.ascolto_capture_buffer_available(self.pointer, channel)

    def dropped(self):
        return self.library.ascolto_capture_buffer_dropped(self.pointer)

    def padded(self, channel):
        return self.library.ascolto_capture_buffer_padded(self.pointer, channel)

    def pop(self, pad):
        count = self.library.ascolto_capture_buffer_pop(self.pointer, self.mic, self.sys, FRAME, pad)
        if not count:
            return 0, None, None
        return count, ctypes.string_at(self.mic, count * 4), ctypes.string_at(self.sys, count * 4)


def compile_queue(directory, root=ROOT):
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler or not shutil.which(compiler[0]):
        raise RuntimeError("A C compiler is required for the native queue replay")
    target = Path(directory) / ("capture-buffer.dylib" if platform.system() == "Darwin" else "capture-buffer.so")
    mode = ["-dynamiclib"] if platform.system() == "Darwin" else ["-shared", "-fPIC"]
    completed = subprocess.run([*compiler, "-std=c11", "-O2", *mode, "-I", str(root / "include"),
                                str(root / "src/capture_buffer.c"), "-o", str(target)],
                               capture_output=True, text=True, timeout=30)
    if completed.returncode:
        raise RuntimeError("Native capture queue compilation failed:\n" + completed.stderr)
    library = ctypes.CDLL(str(target))
    signatures = {
        "create": ([ctypes.c_int], ctypes.c_void_p),
        "free": ([ctypes.c_void_p], None),
        "push": ([ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_float), ctypes.c_int, ctypes.c_int64], None),
        "pop": ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float), ctypes.c_int, ctypes.c_bool], ctypes.c_int),
        "available": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_int),
        "dropped": ([ctypes.c_void_p], ctypes.c_int64),
        "padded": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_int64),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(library, "ascolto_capture_buffer_" + name)
        function.argtypes, function.restype = arguments, result
    return library


@lru_cache(maxsize=64)
def frame_bytes(frame):
    # Frame indices stay exactly representable in float32 for the allowed 24h.
    return struct.pack("=f", frame + 1) * FRAME


def timing_at(timings, index, startup_turns):
    if index < len(timings):
        return timings[index]
    warm = min(startup_turns, len(timings) - 1)
    return timings[warm + (index - len(timings)) % (len(timings) - warm)]


def replay(timings, *, duration_seconds=1800, gap_seconds=0.4, startup_turns=1,
           mic_start_seconds=0, sys_start_seconds=0, capacity_samples=None, library=None):
    """Run an event-driven serialized consumer against unchanged C queue calls."""
    if not timings or not math.isfinite(duration_seconds) or not 0 < duration_seconds <= 86400:
        raise ValueError("Replay duration must be positive and at most 24 hours")
    for value in (gap_seconds, mic_start_seconds, sys_start_seconds):
        if not math.isfinite(value) or value < 0:
            raise ValueError("Gap/start offsets must be finite and nonnegative")
    if startup_turns < 0 or any(type(t.audio_frames) is not int or not 0 < t.audio_frames <= 60 * RATE // FRAME or
                                type(t.elapsed_ns) is not int or t.elapsed_ns < 0 for t in timings):
        raise ValueError("Invalid timing schedule")
    gap_frames = math.ceil(gap_seconds * RATE / FRAME)
    target_frames = math.ceil(duration_seconds * RATE / FRAME)
    total_frames, planned_turns = 0, 0
    while total_frames < target_frames:
        total_frames += timing_at(timings, planned_turns, startup_turns).audio_frames + gap_frames
        planned_turns += 1
    starts = [math.ceil(mic_start_seconds * RATE / FRAME), math.ceil(sys_start_seconds * RATE / FRAME)]
    if min(starts) >= total_frames:
        raise ValueError("At least one channel must start before capture ends")
    capacity = production_capacity_samples() if capacity_samples is None else capacity_samples
    if capacity < FRAME or capacity % FRAME:
        raise ValueError("Capacity must contain a positive whole number of frames")
    if library is None:
        with tempfile.TemporaryDirectory(prefix="ascolto-queue-replay-") as temporary:
            return replay(timings, duration_seconds=duration_seconds, gap_seconds=gap_seconds,
                          startup_turns=startup_turns, mic_start_seconds=mic_start_seconds,
                          sys_start_seconds=sys_start_seconds, capacity_samples=capacity,
                          library=compile_queue(temporary))
    queue = NativeQueue(capacity, library)
    now = produced = delivered = ready = 0
    missing_since = None
    maximum = retained_maximum = overflow_padding = 0
    stop_backlog = None
    asr_calls = lost_turns = completed_turns = 0
    total_pause = 0
    zeros = [0, 0]
    turn_start, retained_audio = 0, 0
    current = timing_at(timings, 0, startup_turns)
    end_audio = current.audio_frames
    end_turn = end_audio + gap_frames
    stop = total_frames * FRAME_NS
    zero = bytes(FRAME * 4)

    def backlog():
        return max(queue.available(0), queue.available(1))

    try:
        while True:
            pending_gap = queue.dropped() - overflow_padding
            maximum = max(maximum, backlog())
            retained_maximum = max(retained_maximum, max(0, backlog() - pending_gap))
            stopped = produced == total_frames
            if stopped and stop_backlog is None:
                stop_backlog = backlog()
            if now >= ready:
                while True:
                    available = [queue.available(0), queue.available(1)]
                    have_pair = min(available) >= FRAME
                    if have_pair or max(available) == 0:
                        missing_since = None
                    elif missing_since is None:
                        missing_since = now
                    pad = stopped or (missing_since is not None and now - missing_since >= MISSING_NS)
                    pending_gap = queue.dropped() - overflow_padding
                    count, mic, sys = queue.pop(pad)
                    if not count:
                        break
                    if count != FRAME:
                        raise AssertionError("Replay lost its 20ms frame alignment")
                    frame = delivered // FRAME
                    if pending_gap:
                        if mic != zero or sys != zero:
                            raise AssertionError("Overflow gap was not equal silence on both channels")
                        overflow_padding += count
                    else:
                        for channel, payload in enumerate((mic, sys)):
                            expected = zero if frame < starts[channel] else frame_bytes(frame)
                            if payload != expected:
                                raise AssertionError("Native queue reordered or changed channel %d frame %d" % (channel, frame))
                    for channel, payload in enumerate((mic, sys)):
                        if payload == zero:
                            zeros[channel] += count
                    if turn_start <= frame < end_audio and (sys if completed_turns % 2 == 0 else mic) != zero:
                        retained_audio += count
                    delivered += count
                    if delivered // FRAME == end_turn:
                        completed_turns += 1
                        if retained_audio:
                            asr_calls += 1
                            ready = now + current.elapsed_ns
                            total_pause += current.elapsed_ns
                        else:
                            lost_turns += 1
                        turn_start = end_turn
                        current = timing_at(timings, completed_turns, startup_turns)
                        end_audio = turn_start + current.audio_frames
                        end_turn = end_audio + gap_frames
                        retained_audio = 0
                        if now < ready:
                            break
            if stopped and backlog() == 0 and now >= ready:
                break
            events = []
            if produced < total_frames:
                events.append((produced + 1) * FRAME_NS)
            if ready > now:
                events.append(ready)
            elif missing_since is not None and backlog():
                events.append(max(now + 1, missing_since + MISSING_NS))
            if not events:
                raise AssertionError("Queue replay cannot progress or drain")
            now = min(events)
            if produced < total_frames and now == (produced + 1) * FRAME_NS:
                for channel in (1, 0):  # SYS before MIC, as in the native consumer.
                    if produced >= starts[channel]:
                        queue.push(channel, produced)
                produced += 1
        if delivered != total_frames * FRAME or completed_turns != planned_turns:
            raise AssertionError("Final drain shortened or extended the capture timeline")
        dropped = queue.dropped()
        if overflow_padding != dropped or retained_maximum > capacity:
            raise AssertionError("Overflow accounting or bounded ring invariant failed")
        return dict(schema_version=1, kind="deterministic_native_capture_queue_replay",
                    requested_capture_s=duration_seconds, capture_s=total_frames * FRAME / RATE,
                    measured_timing_records=len(timings), failed_timing_records=sum(t.failed for t in timings),
                    startup_turns=min(startup_turns, len(timings) - 1), planned_turns=planned_turns,
                    asr_calls=asr_calls, wholly_dropped_turns=lost_turns,
                    sample_rate=RATE, frame_samples=FRAME, capacity_samples=capacity,
                    capacity_s=capacity / RATE, native_ring_payload_bytes=capacity * 2 * 4,
                    channel_start_s=dict(mic=starts[0] * FRAME / RATE, sys=starts[1] * FRAME / RATE),
                    gap_s=gap_frames * FRAME / RATE,
                    max_backlog_s=maximum / RATE, max_retained_backlog_s=retained_maximum / RATE,
                    stop_backlog_s=stop_backlog / RATE, overflow_lost_s=dropped / RATE,
                    overflow_equal_padding_s=overflow_padding / RATE,
                    native_padding_s=dict(mic=queue.padded(0) / RATE, sys=queue.padded(1) / RATE),
                    delivered_zero_s=dict(mic=zeros[0] / RATE, sys=zeros[1] / RATE),
                    timeline_samples=dict(mic=delivered, sys=delivered),
                    final_drain_s=(now - stop) / 1e9, final_virtual_wall_s=now / 1e9,
                    scheduled_asr_pause_s=total_pause / 1e9, queue_empty=backlog() == 0,
                    indexed_pcm_verified=True, equal_overflow_padding_verified=True,
                    assumptions=["Unchanged portable capture_buffer.c executes every queue push/pop",
                                 "Indexed synthetic PCM verifies both channels; no microphone, physical capture or ASR runs",
                                 "Prescribed alternating SYS/MIC turns replace VAD decisions; silence gap defaults to 400ms",
                                 "The first measured timings run once; the remaining warm timings repeat",
                                 "Separate model-load events and other delays outside clip elapsed timings are excluded",
                                 "A wholly lost turn skips ASR; any retained turn pays its full measured latency",
                                 "Other consumer work is instantaneous, so this is an optimistic throughput replay",
                                 "Capture stops at a whole-turn boundary at or after the requested duration",
                                 "Final drain includes outstanding inference and preserves discarded time as paired silence"])
    finally:
        queue.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--duration-seconds", type=float, default=1800)
    parser.add_argument("--gap-seconds", type=float, default=0.4)
    parser.add_argument("--startup-turns", type=int, default=1)
    parser.add_argument("--mic-start-seconds", type=float, default=0)
    parser.add_argument("--sys-start-seconds", type=float, default=0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = replay(load_timings(args.manifest, args.results, args.metadata),
                    duration_seconds=args.duration_seconds, gap_seconds=args.gap_seconds,
                    startup_turns=args.startup_turns, mic_start_seconds=args.mic_start_seconds,
                    sys_start_seconds=args.sys_start_seconds)
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()
    report["provenance"] = dict(manifest=str(args.manifest.resolve()), results=str(args.results.resolve()),
                                manifest_sha256=digest(args.manifest), results_sha256=digest(args.results),
                                queue_source_sha256=digest(ROOT / "src/capture_buffer.c"),
                                production_capacity_sources=["src/capture_macos.c:CAP_RING_CAPACITY",
                                                             "src/audio_backend.c:ascolto_audio_queue_start"])
    metadata = args.metadata or args.manifest.with_name("metadata.json")
    if metadata.exists():
        report["provenance"]["metadata"] = str(metadata.resolve())
        report["provenance"]["metadata_sha256"] = digest(metadata)
    report["reproduce_command"] = shlex.join([sys.executable, str(Path(__file__).resolve()),
        "--manifest", str(args.manifest.resolve()), "--results", str(args.results.resolve()),
        "--duration-seconds", str(args.duration_seconds), "--gap-seconds", str(args.gap_seconds),
        "--startup-turns", str(args.startup_turns), "--mic-start-seconds", str(args.mic_start_seconds),
        "--sys-start-seconds", str(args.sys_start_seconds), "--output", str(args.output.resolve())] +
        (["--metadata", str(args.metadata.resolve())] if args.metadata else []))
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    report["replay_peak_rss_bytes"] = peak if platform.system() == "Darwin" else peak * 1024
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps({key: report[key] for key in ("capture_s", "asr_calls", "max_backlog_s", "overflow_lost_s",
                                                  "final_drain_s", "equal_overflow_padding_verified")}, allow_nan=False))


if __name__ == "__main__":
    main()
