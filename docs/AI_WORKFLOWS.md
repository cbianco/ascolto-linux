# Configured AI tasks and evidence contract

Reviewed against primary documentation on 2026-10-15. This is a provider API
workflow inspired by meeting notes, not an integration with the Google Meet or
Gemini meeting product.

## Product decisions and research

Keep Whistle as the default live ASR and diarizer. Offer two independent roles in
settings: **Recap** selects a text provider/model, while **Transcription** defaults
to local Whistle and requires a separate cloud provider/model and audio-upload
opt-in. A multimodal model name alone never establishes API capability.

Google Meet's notes support eight languages, including Italian, one language at a
time. Its useful patterns are a summary during the call, final notes, decisions,
actions and a visible note-taking state. Language and section availability vary;
the service itself warns that notes can be inaccurate. Ascolto adopts explicit
provisional/final artifacts and verifiable evidence, without claiming Meet's
sharing, Calendar, screenshot or participant-consent features. See [Meet notes
help](https://support.google.com/meet/answer/14754931?hl=en). Meet's timestamp
citations depend on having both notes and transcription enabled; that is the
reason to retain a source timeline, not evidence that the Gemini API exposes the
Meet feature. See [Google's transcript citation announcement](https://workspaceupdates.googleblog.com/2025/02/transcript-citations-take-notes-for-me-google-meet.html?m=1).

| Configured route | Text recap | Cloud transcription |
| --- | --- | --- |
| OpenAI, Sign in with ChatGPT | Eligible selected text model through Responses | Unsupported by this authentication flow |
| OpenRouter | Catalog text input/output, selected model | Dedicated STT catalog entry with audio input and transcription output |
| OpenCode Zen | Verified text model and metadata-selected API family | Not enabled: no documented STT gateway contract verified |
| OpenCode Go | Text route only after acknowledging the coding-plan restriction | Not enabled |

Sign in with ChatGPT explicitly excludes audio/video input and the transcription
API. It also requires streaming Responses with `store: false`, with a restricted
parameter set. The provider worker owns that protocol and authentication. An
ordinary OpenAI audio API key is a different product and is never silently sought
as fallback. See [official preview limitations](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations).
[Pi's public releases](https://github.com/earendil-works/pi/releases) show the new
OpenAI sign-in route separately from its legacy Codex provider; Ascolto does not
borrow Pi's stored credentials.

OpenRouter distinguishes dedicated `/api/v1/audio/transcriptions` from chat audio
input. STT discovery uses `GET /api/v1/models?output_modalities=transcription`;
JSON requests contain raw-byte base64 `input_audio.data` and `format`. The default
result is text JSON. Verbose timing is provider/model dependent and may be
rejected, so it is an explicit option, without an automatic downgrade. STT does
not apply upstream `order`, `only` or `ignore` routing preferences; Ascolto can
promise its selected gateway/model, not a pinned internal serving endpoint. See
[STT documentation](https://openrouter.ai/docs/guides/overview/multimodal/stt).
Chat `input_audio` has a different contract and is not treated as dedicated STT;
see [audio input](https://openrouter.ai/docs/guides/overview/multimodal/audio).
Catalog modality/context fields are documented in the [Models API](https://openrouter.ai/docs/api/api-reference/models/list-all-models-and-their-properties).
A listed model is not a guarantee of account permission, credit or capacity.

OpenCode's model list and inference routes change independently of the model
name; the provider worker uses returned API metadata. Current docs redirect the
former Zen page to Console models. Go is intended for typical coding-agent
traffic and requests an honest client user agent and stable session identifier.
Meeting traffic is outside that documented workload; acknowledging the warning
is not a claim of provider approval. Ascolto does not impersonate a coding client.
See [Console models](https://opencode.ai/docs/zen/) and [Go usage rules](https://opencode.ai/docs/go/).

Gemini's direct audio API is a possible future adapter, not a configured route in
this implementation. Audio understanding accepts files or inline audio and
combines multichannel audio into one channel; it must not erase Ascolto's MIC/SYS
identity. Direct transcription and live transcription have separate documented
models and limits. Batch post-call work avoids adding a cloud dependency to the
live capture path. None of those direct API capabilities establish support
through Zen/Go or a ChatGPT subscription. See [audio understanding](https://ai.google.dev/gemini-api/docs/audio),
[dedicated transcription](https://ai.google.dev/gemini-api/docs/transcribe) and
[live transcription](https://ai.google.dev/gemini-api/docs/live-api/live-transcribe).
Do not hardcode a recommended latest model: refresh the selected account's
catalog and check the route and modality intersection.

## Python integration

`scripts/ai_workflows.py` is independent of the native capture controller and has
no credential or task-preference store. It requires the provider worker contract
`complete(instructions, prompt, session_id, provider=None, model=None)` plus its
`locked`, `load`, `save`, `get_credential` and `transport.request` interfaces.
The compatibility catalog reads `profiles[provider].models` for recap and
`profiles[provider].transcription_models` for STT. The native/SQLite adapter owns
role preferences and imports the same model DTOs.

```python
from ai_workflows import TaskSelection, WorkflowRunner, load_turns

runner = WorkflowRunner(provider_manager, event_sink=record_job_event,
                        session_id=opaque_call_id, max_requests=256)
turns = load_turns(local_jsonl)
runner.recap(TaskSelection(recap_provider, recap_model), turns, output_dir,
             language="English", mode="final")
runner.transcribe(TaskSelection(stt_provider, stt_model), original_archive,
                  turns, output_dir, cloud_audio_opt_in=True, scope="full")
```

Run these operations on a background task queue. Do not hold the provider lock
while calling the runner. Credentials are retrieved under that lock for each STT
request; a disconnect stops the next request. Explicit recap selection does not
mutate the global selected provider. There are no alternate-provider/model
retries or legacy environment-credential lookups.

`refresh_transcription_models(manager)` verifies the saved OpenRouter key and
refreshes only its dedicated STT catalog. It returns model DTOs containing:
`id`, `name`, `input_modalities`, `output_modalities`, `context_length`,
`supported_parameters`. It also updates the existing provider compatibility
cache, never task preferences. `capabilities(provider, dto)` accepts modalities
in those top-level fields or under `architecture`, and returns
`provider/model/recap/transcription/context_length/reason/warning`.
SQLite adapters should import the returned DTOs into their model catalog.

Use SQLite's separate recap/transcription preference records, not a shared model
card with an inferred role. A transcription selection maps to
`TaskSelection(provider, model)` plus `cloud_audio_opt_in`; disabled/unselected
means local Whistle. Recap language is a display/output language, whereas STT
`language` is an optional ISO-639-1 hint. Omit the STT hint for automatic detection.
Do not pass the literal value `auto` to the provider.

## Inputs, coverage and output artifacts

`load_turns` accepts the native `start_s/end_s` and Linux `start_sec/end_sec`
JSONL records. It ignores metadata, uses stable exported database utterance IDs, and falls back
to physical-line source IDs for unchanged legacy/cloud artifacts.
Times must be finite and ordered; MIC is ME, SYS uses existing SPEAKER_N, REMOTE
or UNKNOWN labels. `load_turns(..., allow_empty=True)` permits an empty local
transcript for full-audio recovery.

Full transcription is the default cloud scope. A macOS source is the original
stereo archive, left MIC and right SYS. Linux can pass
`{"MIC": "/absolute/mic.flac", "SYS": "/absolute/sys.flac"}` instead. The runner
decodes each channel independently to private 16 kHz mono PCM and submits all
30-second intervals, including intervals absent from local ASR. Returned outer
offsets cover the original audio timeline. ME remains certain on MIC. SYS is
UNKNOWN in this full-audio view; overlapping local speaker intervals are retained
as `local_speaker_evidence` without extrapolating them to unrecognized speech.
Provider speaker names/labels are discarded. Empty text is represented explicitly
and does not remove time. Full output is `transcript.cloud.ai.jsonl` and `.md`.

With `scope="refine"`, only local turns are submitted, split at 30 seconds while
preserving their start/end offsets, source ID and original speaker. This mode
cannot recover missing local turns. Its distinct artifacts are
`transcript.refined.ai.jsonl` and `.md`. `include_provider_timestamps=True` requests
verbose timing; invalid/out-of-range provider offsets fail validation. Missing
provider timing is an empty array, never fabricated timestamps.

Recap uses redacted transcript text, not audio. Its default source is local Whistle.
An explicit cloud-derived transcript can be selected with
`source_kind="cloud_transcription"` or `"cloud_refinement"`; this provenance is
recorded in the output. Empty cloud-result intervals are not speech evidence. It extracts bounded chunks, validates
every output and deterministically coalesces duplicate claims while retaining
citations. It does not ask a final model pass to discard or reinterpret action
items. This can yield more summary bullets on long calls. The default input
budget is 12,000 UTF-8 bytes, further bounded by half the catalog context length
with reserved schema/instruction space. Oversized single turns fail explicitly;
no text is silently truncated. Default request limit is 64, configurable up to
4,096. A full one-hour dual-channel call needs 240 requests at 30 seconds, so the
caller should show a plan and explicitly set an appropriate limit.

Validated recap JSON contains `schema_version=1`, `summary`, `decisions`,
`actions`, and `open_questions`. Every item has `text` and nonempty `evidence`
with `source_id` plus an exact substring `quote` from the redacted source view. The runner supplies immutable
`start_s/end_s/channel/speaker` fields from the source. Actions additionally have
`owner` and `due`, each nullable. An owner must match a cited local speaker;
a deadline must occur verbatim in a cited quote. Unknown values render as
Unknown. The validator establishes citation and field integrity, not semantic
truth; a plausible but unsupported paraphrase still requires human review.

Final artifacts are `recap.ai.json` and `.md`; provisional snapshots are
`recap.provisional.ai.json` and `.md`. The application retains each attempt under `Recap-history/job-<id>/` in the
call folder on both platforms. Latest final notes are also published as `recap.md`
and `Data/recap.json`, beside `transcript.md`. Cloud transcript attempts are
retained under `Data/Transcription-history/job-<id>/`. Existing artifacts keep their recorded paths. A caller may submit completed local turns
for a provisional snapshot, then run final notes after stop/drain. This module
does not start a live cloud note-taking loop. JSON is canonical; Markdown is its
derived view. All inference must succeed before publishing. Files are atomically
replaced individually with mode 0600. Original transcripts and audio are never
modified. Private audio directories use mode 0700 and their decoded files 0600;
cleanup runs after success or failure. Redaction is best effort, not anonymization.

For a manual OpenCode API diagnostic, run `bash scripts/test_opencode_recap.sh`.
It prompts for a key without echo, sends only a synthetic meeting to
`deepseek-v4.1-flash-free`, and prints the HTTP status and response. It does not
change application settings or use a different model after a rejected request.

## CLI and job adapter

Invoke `python3 scripts/ai_workflows.py --config /absolute/config.json` and send
one JSON object on stdin. Secrets never appear in argv or the request object.
Actions are `capabilities`, `refresh_transcription_models`, `recap`,
`plan_transcription`, and `transcription`.

```json
{"action":"recap","selection":{"provider":"openrouter","model":"catalog/model-id"},"transcript":"/absolute/call.jsonl","output_dir":"/absolute/derived","mode":"final","language":"English","session_id":"opaque-call-id"}
```

```json
{"action":"plan_transcription","selection":{"provider":"openrouter","model":"catalog/stt-id"},"archive":"/absolute/call.m4a","scope":"full","max_requests":256}
```

```json
{"action":"transcription","selection":{"provider":"openrouter","model":"catalog/stt-id"},"archive":"/absolute/call.m4a","transcript":"/absolute/call.jsonl","output_dir":"/absolute/derived","scope":"full","cloud_audio_opt_in":true,"max_requests":256}
```

A full-audio plan decodes locally but sends no audio. It reports request count,
upload seconds, selected provider/model, opt-in requirement and whether the plan
fits the configured request limit. Preflight can describe up to 4,096 requests
without increasing the execution limit. Results are
`{ok:true,result:...}` or a sanitized `{ok:false,error_code,message}`. Exit status
is zero or one. Go recap requires `acknowledge_go_policy:true` explicitly.

`event_sink(event)` receives only `task`, `status`, `provider`, `model` and optional
`completed`, `total`, `error_code`. Tasks are `recap|transcription`; emitted states
are `validating|running|completed|failed`. The caller owns `queued|cancelled`, job
IDs, attempt numbers and cancellation; a retry is a new explicit attempt. Map
these events to SQLite job snapshots. Never store tokens, transcript text, audio,
application titles or calendar metadata in job logs.

Stable workflow errors include `unsupported_capability`, `model_unavailable`,
`provider_policy_acknowledgement_required`, `audio_consent_required`,
`request_limit`, `context_too_small`, `turn_too_large`, `invalid_source`,
`empty_source`, `source_too_large`, `invalid_evidence`, `invalid_owner`,
`invalid_deadline`, `invalid_output`, `unexpected_model`, `invalid_language`,
`audio_decode_failed`, `audio_timeline_mismatch`, and `invalid_request`.
Sanitized provider errors such as `authentication_failed`, `not_connected`,
`permission_denied`, `usage_limit` and `network_error` propagate unchanged. They
never trigger a provider, credential or model fallback.

## Integration checklist and validation

The native adapters invoke `ascolto_runtime.py` for actual application tasks.
`ProviderManager.load/save` uses SQLite and preserves the original secure-store
namespace. Recap and transcription have independent preference records; model
catalog metadata and task roles roundtrip separately. Refreshing recap models
does not invalidate a saved transcription catalog. Disconnection or actual catalog
unavailability disables the affected automatic task without selecting a replacement.

Native stop commits local text before requesting automatic final recap, including
when audio saving is off. Archive publication requests optional cloud transcription
only after its ready reference is committed. Legacy/new automatic flags cannot
send the same session twice. Provisional notes have a distinct job phase, so they
never suppress final notes. Every attempt uses a private output directory, records
safe job transitions and registers generated artifacts before reporting completion.
Manual native actions display errors, status and result locations in Settings.

The automatic adapter calculates the complete-channel request plan, bounded at
4,096, rather than inheriting the library's 64-request default. Explicit preview
displays request count and channel-seconds without uploading audio. Input digests
refer to the exact loaded transcript snapshot and, for speech jobs, original audio.
The original source files are never replaced by AI outputs.

The module uses Python's standard library and the existing provider worker.
Audio decoding additionally uses ffmpeg and ffprobe; its process environment prepends the
Homebrew/local binary paths required by a macOS app bundle. Tests use in-memory
mock provider responses and local synthetic WAVs only:

```sh
.venv/bin/python3 -m unittest discover -s tests -p test_ai_workflows.py -v
make test-recap
```

Coverage includes full-audio recovery with no local turns, separate-channel
samples, nonzero/interleaved offsets, bounded chunks, unknown remote identity,
strict evidence and action fields, catalog routing, expired/limited credentials,
privacy-safe job events, private temporary modes, cleanup and original retention.
These tests establish workflow behavior, not real-call ASR accuracy.
