# Native Nemotron versus OpenRouter

`scripts/compare_nemotron_openrouter.py` is an opt-in evaluation driver for the
shipped Nemotron Q8_0/BF16 adapters and `nvidia/nemotron-3.5-transcribe` on
OpenRouter. It does not change the app's local engine, cloud preferences,
catalog, transcripts or database. Only the explicit `run --backend openrouter`
command uploads audio and incurs API charges. Credentials come directly from
Ascolto's existing Keychain/Secret Service namespace, never argv, environment
fallbacks or report files. Each request rereads the saved credential.

## Frozen cases and scoring

Prepare the public corpus first with `python3 scripts/prepare_nemotron_eval.py`.
The comparison driver verifies its WAV hashes, copies the exact samples into an
independent suite, and freezes references before any inference. It includes:

- Twenty LibriSpeech clips from twenty speakers: twelve clean and eight other.
- Two approximately 50-second concatenations, each split into contiguous
  30-second requests and a final fragment, matching the application's limit.
- Two seeded 10 dB noise probes and two simultaneous-speaker probes.
- A 24 dB quieter copy of the first utterance.
- Three seconds of digital silence and an eight-sample (0.5 ms) silent tail.
- An optional predeclared Italian synthesis and a repeat of the first English
  input after the intervening cases, probing language/prefix state reuse.

The complete suite with Italian contains 31 cases and 33 requests, totaling
324.993375 seconds. Every WAV and the suite manifest have SHA-256 identities.
Preparation also freezes `recovery-probe.json`: the same valid English input
before and after the eight-sample input, using the same WAV bytes. This separate
three-request probe can investigate sequence-dependent failures economically.
The native adapter receives the same PCM16 samples divided by 32768 as float32;
OpenRouter receives the exact WAV bytes as base64 JSON. Chunks stay serialized
and each local process keeps one decoder resident until completion.

WER/CER use the existing scorer's Unicode NFKC/case/apostrophe/punctuation
normalization and micro-average edit counts over fixed reference lengths.
Multi-request case text is concatenated in order without deduplication or text
filtering. Missing and failed chunks remain in the reference denominator;
successful empty output is distinct from a transport/inference failure. Overlap
has no canonical word order and is excluded from accuracy. Silence reports
inserted words separately, without a fabricated WER denominator.

Reports contain per-case text, errors, latency, conditions, observed API cost,
and pairwise normalized agreement. A separate shared-complete-case comparison
measures recognition on the same successfully processed cases, while the full
aggregate also penalizes unavailable results. Cloud transcripts are a comparator,
not the reference. Pairwise agreement is unavailable for incomplete cases. A per-input
HTTP 400 is retained and later cases continue; other provider failures stop the
run with remaining cases marked missing. There are no automatic retries,
provider/model fallbacks or overwrites of existing result files. The command
returns nonzero if any request fails, while preserving completed results.

## Reproduction

Prepare and inspect the plan without accessing credentials or uploading audio:

```sh
python3 scripts/compare_nemotron_openrouter.py prepare \
  --corpus build/nemotron-eval --output build/nemotron-openrouter/suite
```

To include Italian, pass `--italian-wav /absolute/italian.wav`. This must be mono
PCM16 at 16 kHz, synthesized from the predeclared `ITALIAN_REFERENCE` in the
driver: “Domani inviamo il documento aggiornato e confermiamo la riunione delle
dieci.” For example, synthesize with macOS `say -v Alice -o /tmp/italian.aiff`
and that exact text, then convert with ffmpeg. Do not substitute an unrelated
recording: this option intentionally uses a known synthetic reference.

Run each installed native adapter in its own process, serially. On macOS:

```sh
python3 scripts/compare_nemotron_openrouter.py run \
  --suite build/nemotron-openrouter/suite/suite.json --backend local \
  --library bin/Ascolto.app/Contents/Resources/local-stt/nemotron-3.5-q8.dylib \
  --package "$HOME/.config/ascolto/local-models/nemotron-3.5-q8" \
  --output build/nemotron-openrouter/q8.jsonl
```

For BF16, use `nemotron-3.5-bf16-ssd` for both library and package and a distinct
output file. On Linux, pass the corresponding built `.so`. Native inference
requires GPU access where Metal is selected. Existing diagnostic environment
switches prefixed `ASCOLTO_NEMOTRON_` are cleared by this driver so production
defaults apply. Native stderr can be redirected to a separate log.

Explicitly execute the cloud comparison with the saved Ascolto key:

```sh
python3 scripts/compare_nemotron_openrouter.py run \
  --suite build/nemotron-openrouter/suite/suite.json --backend openrouter \
  --output build/nemotron-openrouter/openrouter.jsonl
```

Use `--config /absolute/config.json` for another Ascolto configuration namespace.
Pass `--suite build/nemotron-openrouter/suite/recovery-probe.json` and a new
output filename to run only the recovery probe with either backend.
The default upload budget is 600 seconds and 64 requests, checked before any
credential access or network request. The documented STT request uses
`/api/v1/audio/transcriptions`, WAV `input_audio`, the per-case language,
`temperature=0` and `response_format=json`. Public catalog discovery validates
the audio-to-transcription modality without updating saved settings.

```sh
python3 scripts/compare_nemotron_openrouter.py report \
  --suite build/nemotron-openrouter/suite/suite.json \
  --result q8=build/nemotron-openrouter/q8.jsonl \
  --result bf16=build/nemotron-openrouter/bf16.jsonl \
  --result openrouter=build/nemotron-openrouter/openrouter.jsonl \
  --output build/nemotron-openrouter/comparison.json
make test-nemotron-openrouter
```

Suite/result files are owner-only under ignored `build/`. The JSONL header
records suite/driver/library hashes, package sizes/mtimes, parameters and load
duration. It does not perform fresh full-weight checksum verification: installed
packages should already have passed Ascolto's model manager. Direct ABI testing
exercises the real native prompt, configuration, decoder and result contract;
capture, complete-audio assembly, model-manager verification, deferred storage
and speaker attribution remain separate integration tests.
The standalone test target and `make test-nemotron-ssd` run the eleven new offline
fixtures without credentials, optional weights or cloud requests.

## Interpretation and limits

Similar reference error rates and stable repeated inputs support the practical
validity of the native implementation on these cases. Different outputs alone
do not locate a kernel bug. OpenRouter does not guarantee the same upstream
checkpoint, precision, prompt, decoding configuration or serving revision. Its
top-level transcription `prompt` is documented as ignored, so this comparison
does not pretend to align the internal serving prompts. Q8 and BF16 also differ
in encoder precision. Use the existing numerical/reference tests to investigate
kernel parity and quantization independently.

Latency is observed wall time: native load is separate from requests, cloud
requests include network/serving delay, and OS caches are not purged. First and
repeat cases do not claim measured cache hits. Synthetic quiet/noise/silence
probes and read English do not establish Italian conversational-call accuracy,
diarization quality or a complete LibriSpeech benchmark score.

Primary API references, checked 2026-11-03:
[model](https://openrouter.ai/nvidia/nemotron-3.5-transcribe),
[speech-to-text contract](https://openrouter.ai/docs/guides/overview/multimodal/stt),
[NVIDIA checkpoint](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Transcribe-30B-A4B).

## Recorded execution: 2026-11-03

Artifacts are under ignored `build/nemotron-openrouter-20261103/`. The host is
an Apple M3 Max with 36 GiB of RAM. Native adapters use Metal and their production
4 GiB SSD expert cache. Native model runs are serial; the second cloud run and
small offline/diagnostic checks overlap native inference. Latency is therefore
an indicative observation, not an isolated performance benchmark.

Q8 completed 32 of 33 requests. Its complete scored-reference WER is 23/803
(2.86%). On the twenty original LibriSpeech clips it has 8/475 word errors
(1.68%), versus OpenRouter's 12/475 (2.53%). On the 25 scored cases completed by
both, Q8 has 20/783 word errors (2.55%) and OpenRouter has 23/783 (2.94%);
their CER is 40/3324 (1.20%) and 41/3324 (1.23%), respectively. These small
differences do not establish a statistically significant ranking.

BF16 also completed 32 of 33 requests, with 22/803 word errors (2.74%) over
the full scored suite and 19/783 (2.43%) on the 25 shared completed cases. Its
shared-case CER is 39/3324 (1.17%). It matches Q8's normalized text on 29 of the
30 cases both completed; only the quiet probe differs, with one fewer BF16 word
error. Both have 8/475 word errors on the original twenty public clips.

| Engine | Original 20 WER | Shared 25 WER | Full scored-suite WER | Requests succeeded |
| --- | ---: | ---: | ---: | ---: |
| Native Q8_0 | 1.68% | 2.55% | 2.86% | 32/33 |
| Native BF16 | 1.68% | 2.43% | 2.74% | 32/33 |
| OpenRouter | 2.53% | 2.94% | 5.35% | 30/33 |

Native observed request time totals 583.20 seconds for Q8 and 1041.49 seconds
for BF16, excluding loads of 1.91 and 4.37 seconds. Q8 takes 44.0% less time
in this single pair of serial runs. Successful-request weighted RTF is 1.79 for
Q8, 3.20 for BF16 and 0.135 for the cloud, with different serving hardware and
network delay. The full-run RTF remains unavailable because each run has at
least one failed request. These are not sustained live-call measurements.

Observed boundaries and recovery:

- The 0.5 ms silent fragment fails with `native_inference_failed` on both native adapters and HTTP
  400 on OpenRouter. Both requests contain the original eight samples; no
  application-side padding, filtering or alternative request hides the result.
- Both native adapters subsequently transcribe the Italian synthesis correctly and reproduce
  the first English result byte for byte, demonstrating recovery and stable
  language/prefix reuse for this sequence.
- The complete cloud run attempts all 33 requests and has three HTTP 400
  rejections: the short fragment, Italian, and the repeated English input.
  Its full-suite WER is 43/803 (5.35%), including twenty reference-word deletions
  for the two rejected scored cases. Use the shared-complete comparison above
  to assess recognition separately from availability.
- An isolated Italian request through the same Ascolto transport succeeds with
  the exact expected words. A separate three-request cloud probe reproduces
  valid English success, short-input HTTP 400, then HTTP 400 for the identical
  valid English WAV. The reason for this sequence sensitivity remains unknown;
  neither Italian support nor native decoder correctness is inferred from those
  API rejections. Offline fixtures verify unchanged payloads after rejection.
- Three seconds of digital silence produce empty text on both native adapters and `I.` on
  OpenRouter. This is retained as an observed cloud insertion, without adding
  a transcript rejection filter.

The first cloud run was stopped at the short-input rejection; it is preserved
as `openrouter.jsonl`. The complete second run is
`openrouter-complete.jsonl`. No failed result was replaced by a successful
diagnostic or an implicit retry. `cloud-recovery-probe.jsonl` and
`italian-cloud-diagnostic.json` retain the independent diagnostic evidence.
The final three-engine report is `comparison.json`, alongside `q8.jsonl` and
`bf16.jsonl`. Suite/library hashes and model sizes/mtimes remained unchanged
during execution. Eleven driver fixtures, eleven existing scorer fixtures and
eleven portable queue-replay fixtures pass; the model-driven runs intentionally
retain the observed failures and return nonzero rather than claiming a full pass.

The native recognition results support practical correctness on this limited
corpus, with no large accuracy deficit relative to the hosted model. They do not
prove numerical equivalence or general conversational accuracy. The sub-millisecond
input failure is a concrete remaining boundary issue; sequence-dependent API
rejections require a separate gateway/provider investigation.
