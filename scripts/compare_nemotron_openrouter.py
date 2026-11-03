#!/usr/bin/env python3
"""Opt-in, same-PCM comparison of the shipped native ABI and OpenRouter STT.

Preparation is offline and freezes references before inference. Only `run
--backend openrouter` uploads the suite's WAV chunks, using Ascolto's existing
Keychain/Secret Service credential. No app settings or catalog are written.
"""
import argparse
from array import array
import base64
import copy
import ctypes as ct
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time

from ai_providers import ProviderError, ProviderManager, check_openrouter_error
from ai_workflows import OPENROUTER_STT, capabilities
from ascolto_files import atomic_text
from prepare_nemotron_eval import RATE, digest, read_pcm, write_pcm
from score_nemotron_eval import distribution, edit_counts, normalize

MODEL = "nvidia/nemotron-3.5-transcribe"
MAX_SAMPLES = 30 * RATE
ITALIAN_REFERENCE = "Domani inviamo il documento aggiornato e confermiamo la riunione delle dieci."


def write_json(path, value):
    atomic_text(path, json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def prepare(corpus, destination, italian_wav=None):
    metadata_path = corpus / "metadata.json"
    metadata = json.loads(metadata_path.read_text())
    if destination.exists():
        raise ValueError("Suite destination already exists; choose a new directory")
    destination.mkdir(parents=True, mode=0o700)
    cases = []

    def add(identifier, samples, language, expected, condition, scored=True, **extra):
        chunks = []
        for start in range(0, len(samples), MAX_SAMPLES):
            path = destination / (identifier + "-%03d.wav" % len(chunks))
            write_pcm(path, samples[start:start + MAX_SAMPLES])
            path.chmod(0o600)
            chunks.append(dict(id=path.stem, path=path.name, sha256=digest(path),
                               samples=min(MAX_SAMPLES, len(samples) - start), start_sample=start))
        cases.append(dict(id=identifier, language=language, expected_text=expected,
                          condition=condition, score=scored, chunks=chunks, **extra))

    for clip in metadata["clips"]:
        path = Path(clip["path"])
        if digest(path) != clip["sha256"]:
            raise ValueError("Corpus WAV checksum mismatch")
        add(clip["id"], read_pcm(path), "en", clip["expected_text"], clip["condition"],
            clip["score"], source_sha256=clip["sha256"])
    first = metadata["clips"][0]
    samples = read_pcm(Path(first["path"]))
    add("quiet-24db", array("h", (round(x * 10 ** (-24 / 20)) for x in samples)),
        "en", first["expected_text"], "quiet", source_id=first["id"])
    add("silence", array("h", [0]) * (3 * RATE), "en", "", "silence", False)
    # The adapter accepts final fragments shorter than one 20 ms processing frame.
    add("silence-tail", array("h", [0]) * 8, "en", "", "silence", False)
    if italian_wav:
        add("italian-synthetic", read_pcm(italian_wav), "it", ITALIAN_REFERENCE,
            "synthetic-it", source_sha256=digest(italian_wav),
            reference_origin="Predeclared macOS Alice synthesis text; not human speech")
    add("repeat-first", samples, "en", first["expected_text"], "repeat", source_id=first["id"])
    suite = dict(schema_version=1, model=MODEL, sample_rate=RATE, max_chunk_samples=MAX_SAMPLES,
                 corpus_metadata_sha256=digest(metadata_path),
                 attribution={k: metadata[k] for k in ("corpus", "source", "license", "license_url", "attribution")},
                 protocol="Original contiguous PCM16 split at 30 seconds; concatenate chunk text without deduplication",
                 cases=cases)
    write_json(destination / "suite.json", suite)
    # Freeze an independent A -> short-input -> A diagnostic as well. It lets
    # callers investigate recovery without resubmitting the complete corpus.
    recovery = []
    tail = next(case for case in cases if case["id"] == "silence-tail")
    for source, identifier in ((cases[0], "valid-before"), (tail, "invalid-short"), (cases[0], "valid-after")):
        case = copy.deepcopy(source)
        case.update(id=identifier, condition="recovery-probe")
        for index, chunk in enumerate(case["chunks"]):
            chunk["id"] = identifier + "-%03d" % index
        recovery.append(case)
    write_json(destination / "recovery-probe.json", dict(suite, cases=recovery,
               protocol="Identical valid input before and after an eight-sample input; no retries"))
    return suite


def load_suite(path):
    suite = json.loads(path.read_text())
    if suite.get("schema_version") != 1 or suite.get("model") != MODEL:
        raise ValueError("Unsupported suite")
    case_ids, chunk_ids = set(), set()
    for case in suite["cases"]:
        if case["id"] in case_ids or case["language"] not in ("en", "it"):
            raise ValueError("Duplicate case or unsupported language")
        case_ids.add(case["id"])
        if not isinstance(case["expected_text"], str) or not case["chunks"]:
            raise ValueError("Invalid case")
        if case["score"] and not normalize(case["expected_text"]):
            raise ValueError("Scored case needs a reference")
        offset = 0
        for chunk in case["chunks"]:
            target = Path(chunk["path"])
            if target.is_absolute() or ".." in target.parts or chunk["id"] in chunk_ids:
                raise ValueError("Invalid chunk path or duplicate identifier")
            chunk_ids.add(chunk["id"])
            audio = path.parent / target
            if digest(audio) != chunk["sha256"]:
                raise ValueError("Suite WAV checksum mismatch")
            if not 0 < chunk["samples"] <= MAX_SAMPLES or len(read_pcm(audio)) != chunk["samples"]:
                raise ValueError("Invalid chunk sample count")
            if chunk["start_sample"] != offset:
                raise ValueError("Noncontiguous chunk timeline")
            offset += chunk["samples"]
    if not case_ids:
        raise ValueError("Empty suite")
    return suite


def plan(suite):
    chunks = [chunk for case in suite["cases"] for chunk in case["chunks"]]
    return dict(cases=len(suite["cases"]), requests=len(chunks),
                audio_s=sum(chunk["samples"] for chunk in chunks) / RATE)


# Mirror the versioned public C ABI in include/ascolto/local_stt.h. All model
# computation stays inside the unchanged native adapter, with one resident model.
class NativeText(ct.Structure):
    _fields_ = [("text", ct.c_char * 2048), ("language", ct.c_char * 16)]


Load = ct.CFUNCTYPE(ct.c_void_p, ct.c_char_p, ct.c_void_p, ct.c_size_t)
Transcribe = ct.CFUNCTYPE(ct.c_int, ct.c_void_p, ct.POINTER(ct.c_float), ct.c_int,
                         ct.c_char_p, ct.c_char_p, ct.POINTER(NativeText), ct.c_void_p, ct.c_size_t)
Destroy = ct.CFUNCTYPE(None, ct.c_void_p)


class NativeAPI(ct.Structure):
    _fields_ = [("abi", ct.c_uint), ("load", Load), ("transcribe", Transcribe),
                ("destroy", Destroy), ("backend", ct.c_char_p)]


class Native:
    def __init__(self, library, package):
        self.library = ct.CDLL(str(library.resolve()), mode=os.RTLD_LOCAL | os.RTLD_NOW)
        self.library.ascolto_stt_get_api.restype = ct.POINTER(NativeAPI)
        entry = self.library.ascolto_stt_get_api()
        if not entry or entry.contents.abi != 1:
            raise ValueError("Unsupported native STT ABI")
        self.api = entry.contents
        if not self.api.load or not self.api.transcribe or not self.api.destroy:
            raise ValueError("Incomplete native STT ABI")
        error = ct.create_string_buffer(256)
        self.context = self.api.load(os.fsencode(package.resolve()), error, len(error))
        if not self.context:
            raise ValueError("Native model load failed; inspect native stderr")

    def transcribe(self, path, language):
        pcm = read_pcm(path)
        floats = (ct.c_float * len(pcm))(*(sample / 32768 for sample in pcm))
        text, error = NativeText(), ct.create_string_buffer(256)
        status = self.api.transcribe(self.context, floats, len(pcm), language.encode(), b"",
                                     ct.byref(text), error, len(error))
        return dict(ok=status == 0, text=text.text.decode("utf-8") if status == 0 else "",
                    error="" if status == 0 else "native_inference_failed")

    def close(self):
        self.api.destroy(self.context)


class Cloud:
    def __init__(self, config, manager=None):
        self.manager = manager or ProviderManager(config)
        # Public discovery does not mutate the saved model catalog or preferences.
        data = self.manager.transport.request("https://openrouter.ai/api/v1/models?output_modalities=transcription")
        self.metadata = next((m for m in data.get("data", []) if m.get("id") == MODEL), {})
        if not capabilities("openrouter", self.metadata).transcription:
            raise ValueError("Nemotron is absent from the dedicated STT catalog")
        self.credential()  # Fail before creating a run if the saved key is absent.

    def credential(self):
        value = self.manager.store.get("openrouter")
        if not value or not value.get("key"):
            raise ValueError("No saved Ascolto OpenRouter credential")
        return value["key"]

    def transcribe(self, path, language):
        body = dict(model=MODEL, input_audio=dict(data=base64.b64encode(path.read_bytes()).decode("ascii"),
                                                format="wav"), language=language,
                    temperature=0, response_format="json")
        data = self.manager.transport.request(OPENROUTER_STT, self.credential(), body=body,
                                             headers={"X-Title": "Ascolto native inference evaluation"})
        check_openrouter_error(data)
        if not isinstance(data.get("text"), str):
            raise ProviderError("invalid_response", "Missing transcription text")
        # Never copy arbitrary response fields, error messages or headers to reports.
        reported_usage = data.get("usage")
        if not isinstance(reported_usage, dict):
            reported_usage = {}
        usage = {key: value for key in ("seconds", "cost")
                 if isinstance(value := reported_usage.get(key), (int, float))
                 and not isinstance(value, bool) and math.isfinite(value) and value >= 0}
        return dict(ok=True, text=data["text"], error="", usage=usage)

    def close(self):
        pass


def run(suite_path, output, backend, config=None, library=None, package=None,
        max_requests=64, max_upload_seconds=600):
    suite = load_suite(suite_path)
    planned = plan(suite)
    if backend == "openrouter" and (planned["requests"] > max_requests or planned["audio_s"] > max_upload_seconds):
        raise ValueError("Suite exceeds the explicit upload/request budget")
    if output.exists():
        raise ValueError("Run output already exists; choose a new file")
    started = time.perf_counter()
    engine = Cloud(config) if backend == "openrouter" else Native(library, package)
    try:
        header = dict(type="run", backend=backend, model=MODEL, suite_sha256=digest(suite_path),
                      harness_sha256=digest(Path(__file__)), plan=planned,
                      load_s=time.perf_counter() - started,
                      time_unix_s=time.time(),
                      parameters=dict(language="per case", keywords="", temperature=0))
        if backend == "local":
            header.update(library_sha256=digest(library),
                          package_files={name: dict(bytes=(package / name).stat().st_size,
                                                   mtime_ns=(package / name).stat().st_mtime_ns)
                                         for name in ("encoder.gguf", "decoder.gguf")})
        else:
            header["catalog"] = {key: engine.metadata.get(key) for key in ("id", "architecture", "pricing", "supported_parameters")}
        # Exclusive creation prevents accidental overwrites or paid reruns. Each
        # completed chunk is durable, including when a later request fails.
        output.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as report:
            def emit(value):
                report.write(json.dumps(value, ensure_ascii=False) + "\n")
                report.flush()
                os.fsync(report.fileno())
            emit(header)
            failures = 0
            for case in suite["cases"]:
                for chunk in case["chunks"]:
                    start = time.perf_counter()
                    try:
                        result = engine.transcribe(suite_path.parent / chunk["path"], case["language"])
                    except ProviderError as error:
                        # A per-input 400 is an observed rejection, not a reason
                        # to omit later cases. Stop on other provider errors;
                        # never retry, fall back or resubmit a rejected chunk.
                        result = dict(ok=False, text="", error=error.code, details=error.details)
                        status = error.details.get("http_status", error.details.get("upstream_status"))
                        if status != 400:
                            emit(dict(type="chunk", id=chunk["id"], case_id=case["id"],
                                      sha256=chunk["sha256"], audio_s=chunk["samples"] / RATE,
                                      elapsed_s=time.perf_counter() - start, **result))
                            return 1
                    emit(dict(type="chunk", id=chunk["id"], case_id=case["id"], sha256=chunk["sha256"],
                              audio_s=chunk["samples"] / RATE, elapsed_s=time.perf_counter() - start, **result))
                    failures += not result["ok"]
                    print(json.dumps(dict(case=case["id"], chunk=chunk["id"], ok=result["ok"])), flush=True)
            return int(bool(failures))
    finally:
        engine.close()


def score_run(suite_path, result_path):
    suite = load_suite(suite_path)
    records = [json.loads(line) for line in result_path.read_text().splitlines() if line.strip()]
    if not records or records[0].get("type") != "run" or records[0].get("suite_sha256") != digest(suite_path):
        raise ValueError("Run belongs to a different suite")
    by_id = {}
    wanted = {chunk["id"]: chunk for case in suite["cases"] for chunk in case["chunks"]}
    for record in records[1:]:
        if record.get("type") != "chunk" or record.get("id") not in wanted or record["id"] in by_id:
            raise ValueError("Unknown or duplicate result")
        if record.get("ok") is True and record.get("sha256") != wanted[record["id"]]["sha256"]:
            raise ValueError("Result PCM identity mismatch")
        if not isinstance(record.get("text"), str):
            raise ValueError("Invalid transcript")
        by_id[record["id"]] = record
    rows = []
    for case in suite["cases"]:
        chunks = [by_id.get(chunk["id"], {}) for chunk in case["chunks"]]
        successful = [chunk for chunk in chunks if chunk.get("ok") is True]
        text = " ".join(chunk["text"] for chunk in successful)
        reference, hypothesis = normalize(case["expected_text"]), normalize(text)
        elapsed = sum(chunk.get("elapsed_s", 0) for chunk in chunks)
        audio = sum(chunk["samples"] for chunk in case["chunks"]) / RATE
        rows.append(dict(id=case["id"], condition=case["condition"], scored=case["score"], text=text,
                         failed_chunks=len(chunks) - len(successful), empty=not hypothesis,
                         elapsed_s=elapsed, audio_s=audio,
                         rtf=elapsed / audio if len(successful) == len(chunks) else None,
                         word=edit_counts(reference.split(), hypothesis.split()) if case["score"] else None,
                         character=edit_counts(reference.replace(" ", ""), hypothesis.replace(" ", "")) if case["score"] else None,
                         silence_inserted_words=len(hypothesis.split()) if case["condition"] == "silence" else None))
        rows[-1]["errors"] = [dict(chunk_id=chunk["id"], error=by_id.get(chunk["id"], {}).get("error", "missing_result"),
                                   details=by_id.get(chunk["id"], {}).get("details", {}))
                              for chunk in case["chunks"] if by_id.get(chunk["id"], {}).get("ok") is not True]

    def aggregate(selected):
        scored = [row for row in selected if row["scored"]]
        output = dict(cases=len(selected), scored_cases=len(scored),
                      failed_chunks=sum(row["failed_chunks"] for row in selected),
                      empty_cases=sum(row["empty"] for row in selected),
                      latency_s=distribution([row["elapsed_s"] for row in selected if not row["failed_chunks"]]),
                      weighted_rtf=sum(row["elapsed_s"] for row in selected) / sum(row["audio_s"] for row in selected)
                      if selected and not any(row["failed_chunks"] for row in selected) else None)
        for unit in ("word", "character"):
            errors = sum(row[unit]["errors"] for row in scored)
            total = sum(row[unit]["reference_units"] for row in scored)
            output[unit] = dict(errors=errors, reference_units=total, rate=errors / total if total else None)
        return output

    successful = [record for record in by_id.values() if record.get("ok") is True]
    successful_audio = sum(wanted[record["id"]]["samples"] for record in successful) / RATE
    repeat_checks = []
    rows_by_id = {row["id"]: row for row in rows}
    for case in suite["cases"]:
        if case["condition"] == "repeat":
            before, after = rows_by_id[case["source_id"]], rows_by_id[case["id"]]
            complete = not before["failed_chunks"] and not after["failed_chunks"]
            repeat_checks.append(dict(source_id=before["id"], repeat_id=after["id"], complete=complete,
                                      raw_equal=before["text"] == after["text"] if complete else None,
                                      normalized_equal=normalize(before["text"]) == normalize(after["text"]) if complete else None))
    return dict(run=records[0], aggregate=aggregate(rows), cases=rows, repeat_checks=repeat_checks,
                observed_request_s=sum(record.get("elapsed_s", 0) for record in by_id.values()),
                successful_request_weighted_rtf=sum(record.get("elapsed_s", 0) for record in successful) / successful_audio
                if successful_audio else None,
                conditions={condition: aggregate([row for row in rows if row["condition"] == condition])
                            for condition in sorted({row["condition"] for row in rows})},
                cost_usd=sum(record.get("usage", {}).get("cost", 0) for record in by_id.values()))


def comparison(suite_path, paths):
    runs = {label: score_run(suite_path, path) for label, path in paths.items()}
    pairs = {}
    labels = list(runs)
    for i, left in enumerate(labels):
        for right in labels[i + 1:]:
            differences = []
            for a, b in zip(runs[left]["cases"], runs[right]["cases"]):
                valid = not a["failed_chunks"] and not b["failed_chunks"]
                differences.append(dict(id=a["id"], complete=valid,
                                        normalized_equal=normalize(a["text"]) == normalize(b["text"]) if valid else None,
                                        word_error_delta=b["word"]["errors"] - a["word"]["errors"] if a["scored"] else None))
            pairs[left + " -> " + right] = differences
    common = [index for index, row in enumerate(runs[labels[0]]["cases"])
              if row["scored"] and all(not run["cases"][index]["failed_chunks"] for run in runs.values())]
    shared = {}
    for label, run in runs.items():
        shared[label] = {}
        for unit in ("word", "character"):
            errors = sum(run["cases"][index][unit]["errors"] for index in common)
            total = sum(run["cases"][index][unit]["reference_units"] for index in common)
            shared[label][unit] = dict(errors=errors, reference_units=total, rate=errors / total if total else None)
    return dict(schema_version=1, suite_sha256=digest(suite_path), runs=runs, pairs=pairs,
                recognition_on_shared_complete_cases=dict(
                    case_ids=[runs[labels[0]]["cases"][index]["id"] for index in common], results=shared),
                limitations=["Cloud and native serving prompts/precision are not guaranteed identical",
                             "Cloud output is a comparator, not ground truth; WER/CER use frozen references",
                             "Direct native ABI bypasses model-manager verification, capture, queue and diarization",
                             "Native package file sizes/mtimes are provenance, not fresh full-weight checksums",
                             "Read English and optional synthetic Italian do not establish real-call accuracy"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--corpus", type=Path, default=Path("build/nemotron-eval"))
    prepare_parser.add_argument("--output", type=Path, required=True)
    prepare_parser.add_argument("--italian-wav", type=Path,
                                help="Previously synthesized mono PCM16/16k WAV of ITALIAN_REFERENCE")
    run_parser = commands.add_parser("run")
    run_parser.add_argument("--suite", type=Path, required=True)
    run_parser.add_argument("--output", type=Path, required=True)
    run_parser.add_argument("--backend", choices=("local", "openrouter"), required=True)
    run_parser.add_argument("--config", type=Path, default=Path.home() / ".config/ascolto/config.json")
    run_parser.add_argument("--library", type=Path)
    run_parser.add_argument("--package", type=Path)
    run_parser.add_argument("--max-requests", type=int, default=64)
    run_parser.add_argument("--max-upload-seconds", type=float, default=600)
    report_parser = commands.add_parser("report")
    report_parser.add_argument("--suite", type=Path, required=True)
    report_parser.add_argument("--result", action="append", required=True, help="LABEL=JSONL")
    report_parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            print(json.dumps(plan(prepare(args.corpus, args.output, args.italian_wav))))
        elif args.command == "run":
            if args.backend == "local" and (not args.library or not args.package):
                parser.error("local runs require --library and --package")
            if not math.isfinite(args.max_upload_seconds) or args.max_upload_seconds <= 0 or args.max_requests <= 0:
                parser.error("budgets must be positive and finite")
            # Diagnostic tuning switches must not alter the production defaults.
            for key in list(os.environ):
                if key.startswith("ASCOLTO_NEMOTRON_"):
                    del os.environ[key]
            return run(args.suite, args.output, args.backend, args.config, args.library, args.package,
                       args.max_requests, args.max_upload_seconds)
        else:
            paths = {}
            for value in args.result:
                label, path = value.split("=", 1)
                if not label or label in paths:
                    raise ValueError("Result labels must be nonempty and unique")
                paths[label] = Path(path)
            write_json(args.output, comparison(args.suite, paths))
    except ProviderError as error:
        print(json.dumps(dict(error=error.code, details=error.details)), file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, TypeError) as error:
        # These errors are local validation failures, never provider response bodies.
        print("Evaluation failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
