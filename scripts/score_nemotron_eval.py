#!/usr/bin/env python3
"""Score native decoder JSONL against a prepared evaluation manifest.

Normalization: Unicode NFKC and casefold; delete apostrophes; replace other
punctuation/symbols with spaces; collapse whitespace. WER uses whitespace words;
CER uses the same normalized text with all spaces removed. Aggregate errors are
micro-averaged by reference tokens/characters, never averaged clip percentages.
Missing, failed and empty results remain in the denominator. Overlap is excluded
only when the preparation metadata explicitly marks it score=false.

JSONL clip records require id and text; optional ok/error, elapsed_s (or
elapsed_ms), audio_s/duration_s, rss_bytes/rss_mb, phase, prefix_hits/misses.
Additional load/summary events without id are retained as diagnostics.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import unicodedata


def normalize(text):
    text = unicodedata.normalize("NFKC", text).casefold()
    return " ".join("".join(character if character.isalnum() or character.isspace()
                            else "" if character in "'’" else " "
                            for character in text).split())


def edit_counts(reference, hypothesis):
    """Levenshtein alignment, ties prefer substitution, deletion, insertion."""
    previous = [(j, 0, 0, j) for j in range(len(hypothesis) + 1)]
    for i, expected in enumerate(reference, 1):
        current = [(i, 0, i, 0)]
        for j, actual in enumerate(hypothesis, 1):
            if expected == actual:
                current.append(previous[j - 1])
                continue
            cost, sub, delete, insert = previous[j - 1]
            substitution = (cost + 1, sub + 1, delete, insert)
            cost, sub, delete, insert = previous[j]
            deletion = (cost + 1, sub, delete + 1, insert)
            cost, sub, delete, insert = current[j - 1]
            insertion = (cost + 1, sub, delete, insert + 1)
            current.append(min((substitution, deletion, insertion), key=lambda item: item[0]))
        previous = current
    errors, substitutions, deletions, insertions = previous[-1]
    return dict(errors=errors, substitutions=substitutions, deletions=deletions,
                insertions=insertions, reference_units=len(reference),
                hypothesis_units=len(hypothesis), rate=errors / len(reference) if reference else None)


def accuracy_counts(reference, hypothesis, scored):
    if scored:
        return edit_counts(reference, hypothesis)
    return dict(errors=None, substitutions=None, deletions=None, insertions=None,
                reference_units=len(reference), hypothesis_units=len(hypothesis), rate=None)


def percentile(values, percentage):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentage / 100
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def distribution(values):
    return dict(count=len(values), minimum=min(values) if values else None,
                median=percentile(values, 50), p90=percentile(values, 90),
                p95=percentile(values, 95), maximum=max(values) if values else None)


def number(record, *keys):
    for key in keys:
        value = record.get(key)
        if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0:
            return value
    return None


def load_inputs(manifest, metadata, results):
    information = {clip["id"]: clip for clip in json.loads(metadata.read_text())["clips"]} if metadata.exists() else {}
    clips, identifiers = [], set()
    with manifest.open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            if not row.get("id") or row["id"] in identifiers or None in row:
                raise ValueError("Invalid or duplicate manifest identifier")
            identifiers.add(row["id"])
            clip = dict(information.get(row["id"], {}))
            clip.update(row)
            clip.setdefault("condition", "unspecified")
            clip.setdefault("score", True)
            clips.append(clip)
    records, diagnostics = {}, []
    with results.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            if not line.strip():
                continue
            record = json.loads(line)
            if not isinstance(record, dict):
                raise ValueError("Expected JSON object at line %d" % line_number)
            identifier = record.get("id")
            if identifier is None:
                diagnostics.append(record)
                continue
            if identifier not in identifiers:
                raise ValueError("Result identifier absent from manifest: " + str(identifier))
            if identifier in records:
                raise ValueError("Duplicate result identifier: " + str(identifier))
            records[identifier] = record
    return clips, records, diagnostics


def aggregate(rows):
    scored = [row for row in rows if row["scored"]]
    output = dict(clips=len(rows), scored_clips=len(scored), failures=sum(row["failed"] for row in rows),
                  empty_outputs=sum(row["empty"] for row in rows))
    for label in ("word", "character"):
        totals = {key: sum(row[label][key] for row in scored)
                  for key in ("errors", "substitutions", "deletions", "insertions", "reference_units", "hypothesis_units")}
        totals["rate"] = totals["errors"] / totals["reference_units"] if totals["reference_units"] else None
        output[label] = totals
    output["latency_s"] = distribution([row["elapsed_s"] for row in rows if row["elapsed_s"] is not None])
    output["rtf"] = distribution([row["rtf"] for row in rows if row["rtf"] is not None])
    paired = [row for row in rows if row["elapsed_s"] is not None and row["audio_s"]]
    total_elapsed = sum(row["elapsed_s"] for row in paired)
    total_audio = sum(row["audio_s"] for row in paired)
    output["weighted_rtf"] = total_elapsed / total_audio if total_audio else None
    return output


def score(clips, records, diagnostics=()):
    rows, memory, footprints, prefixes = [], [], [], []
    # Preserve decoder order for chronology and memory trend, then account for omissions.
    ordered = list(records) + [clip["id"] for clip in clips if clip["id"] not in records]
    by_id = {clip["id"]: clip for clip in clips}
    for index, identifier in enumerate(ordered):
        clip, record = by_id[identifier], records.get(identifier, {})
        text = record.get("text", "")
        if not isinstance(text, str):
            raise ValueError("Decoder text must be a string: " + identifier)
        missing = identifier not in records
        failed = missing or record.get("ok") is False or bool(record.get("error")) or not normalize(text)
        normalized = normalize(text) if not missing and record.get("ok") is not False and not record.get("error") else ""
        expected = normalize(clip["expected_text"])
        if clip["score"] and not expected:
            raise ValueError("Scored clip has no reference text: " + identifier)
        elapsed = number(record, "elapsed_s", "latency_s", "decode_s")
        if elapsed is None:
            milliseconds = number(record, "elapsed_ms", "latency_ms", "decode_ms")
            elapsed = milliseconds / 1000 if milliseconds is not None else None
        audio = number(record, "audio_s", "duration_s")
        if audio is None:
            audio = number(clip, "duration_s")
        phase = record.get("phase", "first" if index == 0 else "warm")
        rss = number(record, "rss_bytes")
        if rss is None:
            rss_mb = number(record, "rss_mb")
            rss = rss_mb * 1024 * 1024 if rss_mb is not None else None
        if rss is not None:
            memory.append((index, rss))
        footprint = number(record, "footprint_bytes")
        if footprint is not None:
            footprints.append((index, footprint))
        prefix = {key: record[key] for key in ("prefix_hits", "prefix_misses", "prefix_reused", "prefix_tokens") if key in record}
        if prefix:
            prefixes.append(dict(id=identifier, **prefix))
        rows.append(dict(id=identifier, condition=clip["condition"], phase=phase,
                         scored=clip["score"], failed=failed, missing=missing,
                         empty=not normalized, error=record.get("error"),
                         expected=clip["expected_text"], text=text, normalized_text=normalized,
                         word=accuracy_counts(expected.split(), normalized.split(), clip["score"]),
                         character=accuracy_counts(expected.replace(" ", ""), normalized.replace(" ", ""), clip["score"]),
                         elapsed_s=elapsed, audio_s=audio,
                         rtf=elapsed/audio if elapsed is not None and audio else None, rss_bytes=rss,
                         footprint_bytes=footprint, peak_footprint_bytes=number(record, "peak_footprint_bytes")))
    def memory_summary(samples):
        result = dict(samples=len(samples), first_bytes=samples[0][1] if samples else None,
                      last_bytes=samples[-1][1] if samples else None,
                      peak_bytes=max((value for _, value in samples), default=None),
                      delta_bytes=samples[-1][1]-samples[0][1] if samples else None,
                      slope_bytes_per_turn=None)
        if len(samples) >= 2:
            mean_x, mean_y = statistics.mean(x for x, _ in samples), statistics.mean(y for _, y in samples)
            result["slope_bytes_per_turn"] = sum((x-mean_x)*(y-mean_y) for x, y in samples) / sum((x-mean_x)**2 for x, _ in samples)
        return result
    return dict(schema_version=1,
                normalization="NFKC + casefold; apostrophes deleted; other punctuation/symbols replaced by spaces; whitespace collapsed; CER excludes spaces",
                percentile_method="Linear interpolation at (n-1)*p/100",
                caveats=["WER/CER measure this fixed subset, not general call accuracy",
                         "Overlap excluded from WER/CER; failures and latency still reported",
                         "RSS slope is observational and is not proof of a memory leak",
                         "First/warm defaults to decoder order when phase is absent; it does not assert cache state"],
                aggregate=aggregate(rows),
                conditions={condition: aggregate([row for row in rows if row["condition"] == condition])
                            for condition in sorted({row["condition"] for row in rows})},
                phases={phase: aggregate([row for row in rows if row["phase"] == phase])
                        for phase in sorted({row["phase"] for row in rows})},
                memory=memory_summary(memory), footprint=memory_summary(footprints), prefix_observations=prefixes,
                startup_latency_s=distribution([value for record in diagnostics if record.get("type") == "load"
                                                for value in [number(record, "elapsed_s")] if value is not None]),
                diagnostics=list(diagnostics), clips=rows)


def compare(current, baseline):
    previous = {row["id"]: row for row in baseline["clips"]}
    if set(previous) != {row["id"] for row in current["clips"]}:
        raise ValueError("Comparison runs must cover the same manifest identifiers")
    rows = []
    for row in current["clips"]:
        before = previous[row["id"]]
        elapsed_delta = row["elapsed_s"] - before["elapsed_s"] if row["elapsed_s"] is not None and before["elapsed_s"] is not None else None
        rows.append(dict(id=row["id"], transcript_changed=row["text"] != before["text"],
                         normalized_transcript_changed=row["normalized_text"] != before["normalized_text"],
                         baseline_text=before["text"], current_text=row["text"],
                         word_error_delta=row["word"]["errors"]-before["word"]["errors"] if row["scored"] and before["scored"] else None,
                         character_error_delta=row["character"]["errors"]-before["character"]["errors"] if row["scored"] and before["scored"] else None,
                         elapsed_s_delta=elapsed_delta, baseline_failed=before["failed"], current_failed=row["failed"]))
    def delta(label):
        a, b = current["aggregate"][label]["rate"], baseline["aggregate"][label]["rate"]
        return a-b if a is not None and b is not None else None
    return dict(direction="Current minus baseline; negative error/latency deltas improve",
                changed_transcripts=sum(row["transcript_changed"] for row in rows),
                changed_normalized_transcripts=sum(row["normalized_transcript_changed"] for row in rows),
                wer_delta=delta("word"), cer_delta=delta("character"),
                baseline_aggregate=baseline["aggregate"], clips=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, help="Baseline decoder JSONL using the same manifest")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    metadata = args.metadata or args.manifest.with_name("metadata.json")
    report = score(*load_inputs(args.manifest, metadata, args.results))
    if args.baseline:
        report["comparison"] = compare(report, score(*load_inputs(args.manifest, metadata, args.baseline)))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps(report["aggregate"], indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
