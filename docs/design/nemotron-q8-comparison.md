# Nemotron Q8_0 versus BF16 SSD

Measured on 2026-10-30 on the development Apple M3 Max / 36 GB Mac.
The optional native packages share the pinned NeMo/llama.cpp runtime, greedy
prompt, 4 GiB expert cache, eight SSD readers and prefetch configuration.
Q8 uses packed 8-bit decoder weights and an F16 perception encoder; BF16 keeps
original decoder weights and lossless F32 perception values. This compares the
complete available packages, not an isolated quantization kernel.

## Protocol

Four serial processes in Q8 → BF16 → BF16 → Q8 order, with six turns per process:
the public/synthetic English fixture twice, LibriSpeech `3729-6852-0043` and
`908-31957-0002`, then an Italian macOS Alice synthetic sentence twice. Four
unique recordings, 23.89 seconds including repeats per process. No user call,
microphone, cloud ASR, concurrent model run or weight download was used during
measurement. OS caches were not purged; “first” means first turn for that language
prefix in the process, not a guaranteed cold SSD. Sample sizes are small.

The Italian reference is: “Domani inviamo il documento aggiornato e confermiamo
la riunione delle dieci.” PCM is mono 16 kHz. Measurements use the existing
`ascolto-nemotron-decoder` executable and its JSONL timing/memory reports.
Artifacts and the repeatable driver are under ignored
`build/nemotron-q8-validation/` (`compare.py`, `manifest.tsv`, four JSONL/log pairs,
`comparison.json`). Startup integrity hashing and download time are excluded.

## Measurements

| Metric | Q8_0 SSD | BF16 SSD |
| --- | ---: | ---: |
| Total inference, run 1 | 47.44 s | 104.17 s |
| Total inference, run 2 | 46.29 s | 94.80 s |
| Median total inference | 46.86 s | 99.48 s |
| Median native load | 1.38 s | 2.20 s |
| Maximum process footprint | 7.70 GB | 9.94 GB |
| Expert pairs in 4 GiB | 405 | 215 |

Q8 was **2.12× faster**, with
**52.9% less inference time**
on this test. Peak process footprint includes more than the expert cache;
shared Metal allocations must not be added again to the process footprint.

| Turn (median across two runs) | Q8_0 SSD | BF16 SSD |
| --- | ---: | ---: |
| english-cold | 9.22 s | 17.37 s |
| english-warm | 5.42 s | 9.98 s |
| 3729-6852-0043 | 6.25 s | 12.88 s |
| 908-31957-0002 | 7.12 s | 13.12 s |
| italian-cold | 10.97 s | 29.09 s |
| italian-warm | 7.87 s | 17.04 s |

Both packages completed every turn and produced the same normalized words;
one capitalization differed. Both have the same proper-name error on the first
LibriSpeech clip (3 word edits / 60 reference words including repeats under a
simple case/punctuation normalizer). Synthetic Italian and English match their
reference words. This is not a broad accuracy or conversational-call evaluation,
and quantization is not lossless. Neither aggregate run achieved real time.

## Correctness and integration

Packed Q8 extents use `ggml_row_size`, remain quantized in cache/Metal, and share
BF16's lease, eviction, asynchronous reader and failure contracts. Synthetic
resident-versus-streamed comparisons cover separate/fused projections on CPU and
Metal, mapping fallback, prefetch, short-read poisoning and pin release. Wide Q8
fixtures use all 405 slots, including slot 404; Metal comparisons are bit exact.
BF16 regressions remain passing. Cache/prototype tests pass 2,874 checks.

The catalog pins the complete Q8 package (36,675,056,006 bytes) by revision,
size and SHA-256. Native model-manager tests cover verified download, independent
selection, frozen jobs and active-package removal. macOS and GTK use one Nemotron
card with 8/16-bit choices; browsing does not change the saved model. The macOS
historical-transcription sheet enables only installed variants, and passes the
exact Q8 package ID to background preparation.

The real Q8 adapter also passed two complete verified-package sessions on a
512 KiB worker stack using an isolated configuration and the English fixture.
This exercises the actual model manager, exported ABI, 4 GiB cache configuration,
recognition, teardown and reload; the source package hashes matched the manifest.
No production selection or historical transcript was changed by validation.
