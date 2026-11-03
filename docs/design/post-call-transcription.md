# Native post-call transcription on macOS

The shipped macOS app records first and transcribes after capture stops. Starting
or detecting a call does not load the selected speech model or wait for its
integrity scan. Linux and the explicit offline/live test harness keep their
existing native paths.

## Capture and processing

CoreAudio producers still enqueue paired, timestamped PCM. The consumer writes
16 kHz float MIC and SYS files, tracks levels and advances the capture timeline;
it performs no speech recognition, speaker embedding or speech segmentation.
Files are owner-only, flushed and synchronized approximately once per second,
then synchronized and closed after capture drains. Overflow remains represented
as equal-duration silence; storage errors are surfaced rather than silently
completing a recording. A crash can lose the most recent unsynchronized audio.

A native job is registered before capture starts. It freezes model ID, language,
keywords including attendee bias, diarization threshold, source paths and archive
preferences. Closing capture seals the durable sample count and queues the job.
Temporary raw stereo data uses approximately 461 MB per hour. The audio-retention
setting controls the final M4A archive: temporary PCM is necessary even when that
setting is off. Failed/cancelled jobs retain their source audio for explicit retry.

One native background worker owns its own context, database connection and
cross-process lock. It processes jobs serially, reuses a loaded model for
consecutive jobs with the same selection, and releases it when the queue empties
or inference fails. A new recording pauses replay between frames/segments; an
adapter call already running finishes first. Recording never waits for that call.
The loaded model may remain resident while replay is paused for another call.

Replay reads paired one-second blocks, SYS before MIC, and assembles contiguous
30-second requests independently for each channel. All original PCM reaches the
selected model, including silence and very short final fragments. No energy VAD,
MIC echo gate or text suppression runs before or after recognition. Native speaker
embeddings remain separate from recognition. Only empty model responses produce
no utterance. Request boundaries are transport limits, not inferred sentence or
speaker boundaries.

Replay reuses its two request buffers between jobs and retries. File-read batching
does not change request PCM, checkpoint ordinals or the one-second progress cadence;
cancellation is checked between reads and before each model request. Capture keeps
its independent 20 ms queue frames.

## Recovery and publication

Every deterministic audio request has an ordinal. A versioned checkpoint stores the
ASR output, projected voiceprint and SHA-256 of that segment's PCM, including
empty model responses. Checkpoint, accepted utterance and progress
commit atomically. Restart replays PCM from the beginning to rebuild request and
speaker state, while completed checkpoints skip model inference and duplicate
utterance insertion. Changed segment PCM or an incompatible checkpoint fails
safely. Increment the checkpoint version when changing the replay algorithm or
payload representation.

The progress counters named `processed_frames` and `total_frames` use PCM sample
positions per channel, not counts of 20 ms frames. Transcript exports derive from
committed rows and are refreshed after accepted turns and at completion.

Only the exclusive worker recovers interrupted jobs. Interrupted capture preserves
the longer durable channel and pads the shorter channel with silence; its metadata
records the interruption. Invalid/missing sources fail without deletion. A model
still being checked is awaited; an unavailable model fails with a retry action,
without changing the job's frozen model choice.

Successful transcript publication precedes the durable completed state. Archive
compression and independently opted-in AI work happen afterward. Raw audio is
removed only after successful archive publication, or after successful local
transcription when archive retention is disabled. Startup retries interrupted
finalization. An existing ready, readable archive is preserved even if a prior
crash removed only one raw channel.

Checkpoint format version 2 records the complete-audio request pipeline. Previous
version-1 VAD checkpoints are incompatible and must never be reused as if they
contained complete original audio. A partial old job fails safely and retains
sources/committed text. A separate new transcription version can use the new
pipeline when an eligible ready archive exists. The existing version UI cannot
recover a raw-only interrupted job; those sources require manual recovery.
Retry does not convert old checkpoints. Existing completed transcripts are not
rewritten.

Schema v2 adds dedicated native queue/checkpoint tables, separate from provider
jobs. C and Python openers make owner-only SQLite backups before migrating v1.
The app checks for an existing instance before bootstrap/migration; the currently
running older binary must close before the new version opens its database.

## Controls and verification

The menu separates recording from the local transcription queue. It displays
progress, queued/failed counts and cancellation/resume actions. All menu/status
reads use copied snapshots; UI requests enqueue commands without database I/O.
`ascoltoctl STATUS` includes `deferred_transcription`;
`LOCAL_QUEUE_CANCEL <job_id>` and `LOCAL_QUEUE_RETRY <job_id>` target a saved job.
Cancellation and shutdown cooperate between model calls; they do not interrupt
an adapter in the middle of inference.

`make test-deferred` exercises persistent transactions/migration, worker
scheduling, capture/replay, checkpoint reuse, source mutation, failure recovery
and cleanup with synthetic PCM and mocked model/hardware calls. The pipeline test
uses production audio request/diarization/storage/export code. It does not establish
recognition accuracy or physical capture reliability on arbitrary conversations.
The existing macOS acoustic-room, lifecycle, socket, archive and smoke suites
remain applicable. Shared database/store fixtures also run on Linux.

## Re-transcribing a saved call

Activity & diagnostics exposes **Transcribe again…** in the selected call's
Transcription section, beside the original transcript. The sheet lists ready
installed local models, defaults to the current local-model selection and restores
the call's saved language when available. **Manage models…** opens the existing
on-device settings page. **Add to queue** prepares the saved audio on a background
queue and does not change the model selected for future calls.

This action currently accepts ready CoreAudio stereo M4A archives. Missing audio,
an unfinished original transcription and unsupported archive layouts disable or
reject the request with an explanation. AudioToolbox decodes both channels to a
private 16 kHz float working copy, preserving MIC-left/SYS-right order, using
bounded buffers. The original archive, transcript, utterances and recaps remain
unchanged. Closing the window cooperatively cancels uncommitted preparation;
a committed job continues in the persistent queue.

Each successful preparation atomically creates a separate internal transcript
session and native queue job. `native_transcription_source_session` in their
metadata identifies the original call. Activity groups these sessions as versions
under that call; latest-call shortcuts exclude them. Original and version output
files remain independently readable, with recorded model, date, state and progress.
Historical model/language identity that was never saved is shown as unknown.

Version files live under the call's
`Data/Local-transcription-history/version-<unique>/` folder. Legacy flat archives
use the matching transcript-stem call folder without moving old files. Each
version has its own Markdown/JSONL, speaker identities, checkpoints and export
outbox. Recognition uses the frozen vocabulary, language and model and reuses the
existing serial native worker. Duplicate active requests for the same source,
model and language are rejected; explicit later versions remain possible.

Completion removes only that version's temporary decoded PCM. It does not
regenerate notes, enable cloud work or replace the original transcript. Existing
queue Cancel/Resume controls also apply to version jobs. Archive reorganization
is blocked while native jobs are queued or running so their frozen source paths
cannot move underneath processing.

`make test-retranscribe` uses synthetic WAV and native AAC archives to verify
channel identity/resampling, SHA-256 preservation of original files, model and
language selection, legacy layouts, duplicate requests, cancellation and atomic
rollback. It requires access to the macOS codecs, just like other CoreAudio tests.
`make test-activity` covers the native sheet and actions with a mocked enqueue;
shared diagnostics tests verify grouping and latest-call selection. These tests
do not load a real recognition model or reprocess user recordings.
