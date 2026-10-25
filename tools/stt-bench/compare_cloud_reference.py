#!/usr/bin/env python3
"""Compare local outputs to an explicitly prioritized automatic cloud reference."""
import argparse
import json
import os
from pathlib import Path
import re
import unicodedata


def words(text):
    return re.findall(r"[^\W_]+", unicodedata.normalize("NFKC", text).casefold(), flags=re.UNICODE)


def alignment(reference, hypothesis):
    table = [[0] * (len(hypothesis) + 1) for _ in range(len(reference) + 1)]
    for i in range(len(reference) + 1):
        table[i][0] = i
    for j in range(len(hypothesis) + 1):
        table[0][j] = j
    for i, r in enumerate(reference, 1):
        for j, h in enumerate(hypothesis, 1):
            table[i][j] = min(table[i-1][j] + 1, table[i][j-1] + 1,
                              table[i-1][j-1] + (r != h))
    i, j = len(reference), len(hypothesis)
    counts = dict(substitutions=0, deletions=0, insertions=0, matches=0)
    operations = []
    while i or j:
        if i and j and table[i][j] == table[i-1][j-1] + (reference[i-1] != hypothesis[j-1]):
            same = reference[i-1] == hypothesis[j-1]
            counts["matches" if same else "substitutions"] += 1
            operations.append(dict(op="equal" if same else "substitute", reference=reference[i-1], local=hypothesis[j-1]))
            i, j = i-1, j-1
        elif i and table[i][j] == table[i-1][j] + 1:
            counts["deletions"] += 1
            operations.append(dict(op="delete", reference=reference[i-1], local=""))
            i -= 1
        else:
            counts["insertions"] += 1
            operations.append(dict(op="insert", reference="", local=hypothesis[j-1]))
            j -= 1
    counts.update(distance=table[-1][-1], reference_length=len(reference), hypothesis_length=len(hypothesis))
    return counts, list(reversed(operations))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("build/stt-bench"))
    parser.add_argument("--data", type=Path, default=Path("reports/local-stt-2026-10-16/report-data.json"))
    args = parser.parse_args()
    data = json.loads(args.data.read_text())
    reference = {}
    for line in (args.root / "cloud-reference/responses.jsonl").read_text().splitlines():
        item = json.loads(line)
        if item["status"] == "completed":
            reference[item["clip"]] = item
    records = []
    for row in data["rows"]:
        if row["repeat"] != 0 or row["error"] or row["clip"] not in reference:
            continue
        ref = reference[row["clip"]]
        rw, hw = words(ref["text"]), words(row["text"])
        counts, operations = alignment(rw, hw)
        char_counts, _ = alignment(list(" ".join(rw)), list(" ".join(hw)))
        records.append(dict(model=row["model"], clip=row["clip"], **counts,
                            character_distance=char_counts["distance"], reference_characters=char_counts["reference_length"],
                            operations=operations, local_text=row["text"], reference_text=ref["text"]))
    result = dict(model="openai/whisper-large-v3-turbo", provider="OpenRouter", reference_policy="cloud_primary_reference_no_local_majority_override",
                  normalization="NFKC, casefold, Unicode alphanumeric words; punctuation/apostrophes are separators; accents and numeral spelling retained",
                  reference_weight="primary_anchor_requested_by_user_not_a_calibrated_confidence_probability",
                  limitations=["Automatic reference, not human ground truth.", "The reference model is also the model family of local Whisper turbo; agreement is not an independent accuracy test.",
                               "One cloud response per clip; local pass 0 only to avoid triple-counting repeated identical audio.",
                               "Cloud input is PCM16 WAV derived from the native float32 corpus; network timing is not pure inference.",
                               "Overlapping 10s/30s clips must not be mistaken for independent speech; use short-only scope for the six independent windows."],
                  references=reference, records=records)
    out = args.root / "cloud-reference/comparison.json"
    out.write_text(json.dumps(result, ensure_ascii=False, indent=2))
    os.chmod(out, 0o600)
    totals = []
    for model in sorted({r["model"] for r in records}):
        rr = [r for r in records if r["model"] == model and r["clip"].endswith("10s")]
        n = sum(r["reference_length"] for r in rr)
        totals.append(dict(model=model, words=n, disagreement_percent=100*sum(r["distance"] for r in rr)/n if n else None))
    print(json.dumps(sorted(totals, key=lambda r:r["disagreement_percent"] if r["disagreement_percent"] is not None else float("inf"))))


if __name__ == "__main__":
    main()
