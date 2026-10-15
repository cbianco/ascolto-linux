# SQLite persistence contract

`<config-parent>/ascolto.sqlite3` is authoritative for native settings, vocabulary,
sessions, utterances, AI choices, jobs and artifact references on Linux and macOS.
`src/database.c` and `scripts/ascolto_database.py` share
`scripts/database_schema.sql`. Regenerate its native embedding with
`python3 scripts/generate_database_schema.py`; package the SQL and Python workers
together. Native builds use `-lsqlite3` without loading SQL assets at runtime.
`src/storage.c` connects both native controllers to this API;
`scripts/ascolto_runtime.py` owns bootstrap, export recovery and persisted AI tasks.

Handles use foreign keys, WAL, full synchronization and a bounded busy timeout
(default 1,500 ms; accepted range 0–10,000 ms). New files are owner-only. Use a
separate handle on each background worker. Never open/write the database from
menu construction, the main UI thread or an audio callback. UI reads continue to
use copied snapshots; capture producers only enqueue PCM.

## Native settings and session writes

Config startup runs standard-library bootstrap before the UI/control server.
A `bootstrap_state` marker distinguishes adoption from defaults created by an AI
worker. Later native saves update SQLite, leaving imported JSON/INI/keyword files
unchanged. The small `native_settings` table retains legacy values and the explicit
OpenCode Go acknowledgment. `ascolto_db_setting_get/set()` allowlist app columns;
`oauth_host_id` is generated once and read-only. Native devices retain their
platform identifiers. Config adapters use explicit conversions and key/value
lists; settings and Linux keywords commit in the caller's transaction.

At recording start, `ascolto_storage_session()` commits the session and pending
transcript references together. Session DTOs include UTC start milliseconds,
backend, cached call metadata, vocabulary and `metadata_json`. macOS publishes
this complete initial snapshot once. The database ID is separate from recording
state and Linux's in-memory session counter.

After ASR, `ascolto_storage_write()` owns a copy of each turn until its commit
succeeds. Speaker creation, utterance text/diagnostics and the durable export
request form one atomic database operation. No second metadata UPDATE or outer
per-turn transaction is needed. Flush uses one handle for queued commits, then
publishes the resulting transcript once. A failed commit leaves that turn and
later turns queued; its text is never published. A publication failure leaves
committed text in the durable outbox. Retry occurs on the next turn and final
drain; a persistent error marks the session failed and prevents automatic AI.
Uncommitted memory cannot survive process termination during persistent storage
failure; original audio, when enabled, remains available for recovery.

`ascolto_db_session_state()` atomically changes lifecycle state, optional metadata
and the export request. A null metadata argument retains the previous snapshot.
Stop still drains before committing terminal state. `ended_at_ms` is UTC time;
`duration_s` is the delivered sample timeline, including represented overflow gaps.
Utterance offsets are integer call-relative milliseconds. Speaker identity is
scoped to session and channel; its foreign key rejects cross-call/channel links.

Utterance metadata retains acoustic/ASR diagnostics. Session metadata retains
calendar organizer, attendees, agenda, application/audio names and capture
counters. Audio stays on disk: mark M4A or MIC/SYS FLAC artifacts ready only after
successful finalization. No database API deletes filesystem resources. Paths are
normalized absolute bound values, supporting apostrophes/Unicode and surviving
future output-directory changes. `ascolto_db_sqlite()` exposes the same connection
for additional bound queries, without bypassing its transaction/foreign keys.

## Exports and restart recovery

Native and Python publishers derive Markdown/JSONL from committed rows. JSONL
includes both `start_s/end_s` and `start_sec/end_sec`, diagnostics and immutable
`db:<session>:<utterance>` source IDs. An earlier late turn cannot renumber recap
citations. New Markdown uses English prose; original imported transcripts are
never rewritten or republished merely because bootstrap ran.

Publication reserves a database write transaction while rendering and replacing
both files. Each uses a sibling owner-only temporary file, flush/fsync, atomic
replacement and directory fsync. Only then are ready references and the exact
outbox revision acknowledged. Python also records content hashes. A crash between
renames leaves the revision pending, so recovery rewrites both. The reservation
prevents an older publisher from overwriting a newer acknowledged export. Never
perform network calls in this transaction. Python publication shares
`scripts/ascolto_files.py` with the AI and legacy recap workers.

Startup restores the newest readable ready local transcript to archive shortcuts,
using persisted references even outside the current output directory. It never
restores a historical session ID, recording state, elapsed time, speakers or live
utterance/sample counters.

## AI metadata and jobs

AI connection/catalog/preference/job mutations have one implementation in Python;
the native database API contains only recording, settings and export operations.
`ProviderManager.load/save` uses SQLite under its existing process lock. It
roundtrips account/client/subject/workspace identities, welcome flags, catalog
order, model route metadata and each profile's model choice. Secrets remain in
Keychain/Secret Service. The secure-store namespace still hashes the original
canonical `ai-providers.json` path; `credential_ref` is an identifier such as
`openrouter` or `openai:<account-uuid>`, never a credential value. Python rejects
nested secret fields, C DTOs expose no credential fields, and provider responses
must not enter model metadata or error columns. Disconnect removes the secret and
marks the connection disconnected while retaining registration/history.

`recap_preferences` selects recap connection/model/language with automatic work
off by default. `transcription_preferences` separately selects cloud STT with
`enabled=0` and `cloud_audio_opt_in=0`. Enabling requires explicit audio consent.
Disconnection/catalog unavailability disables the affected automatic task without
choosing a replacement. `Database.task_selection(task)` returns the selected
provider/model/language/consent; callers separately check automation flags.
Whistle remains the live engine. See [AI_WORKFLOWS.md](AI_WORKFLOWS.md) for provider,
audio and evidence contracts, including text recaps with audio saving off.

Enqueue snapshots catalog identity, language and the exact source SHA-256 before
inference. Attempts increment per session/task, and provisional/final recap phases
remain distinct. Each call has `transcript.md`, `Audio/` and `Data/transcript.jsonl`.
Recap attempts use `Recap-history/job-<id>/` inside that call folder, with latest
final aliases at `recap.md` and `Data/recap.json`. Cloud transcription attempts use
`Data/Transcription-history/job-<id>/`. Existing artifact paths remain valid after
archive-directory changes; previous
notes are preserved on explicit retry. States are
`queued|validating|running|completed|failed|cancelled`; SQL triggers append
content-free `job_events`. Store only short sanitized error codes. Commit ready
artifacts and terminal state together before reporting success.

Startup/status reconciliation uses a nonblocking session/task flock to prove a
worker is gone before failing an interrupted attempt. It never retries inference
automatically. A provisional attempt cannot suppress automatic final notes.
Deleting a connection preserves historical job snapshots; deleting a job detaches
artifact references without deleting files. Preference/model deletion clears both
halves of nullable selections. API-key identity is unique by provider/profile;
OAuth identity by provider/profile/subject/normalized workspace, allowing distinct
workspace registrations for the same person.

## Legacy adoption

Bootstrap scans only native session names in the configured archive root, without
recursive/global fixture searches. The explicit CLI accepts path lists and can
specify the timezone of legacy filenames:

```sh
python3 scripts/ascolto_database.py \
  --database /path/to/config-parent/ascolto.sqlite3 \
  --platform macos --config /path/to/config.json \
  --ai-settings /path/to/ai-providers.json \
  --legacy-timezone Europe/Rome \
  --sessions /path/to/2026-10-15_10-20-30-123456_call.jsonl
```

Linux uses `--platform linux`, section-optional `config.ini`, and
`call_<timestamp>_<counter>/transcript.jsonl`. Keywords are deduplicated. No migrated
legacy cloud flag enables a new upload route. Module import itself performs no
adoption.

Config/keywords/AI metadata receive immutable owner-only sibling backups named
`<filename>.ascolto-backup.<sha256-prefix>`. `legacy_imports` records source digests,
backup paths, timestamps and session IDs. Repeated identical imports are no-ops;
changed imported sources require explicit reconciliation. The CLI batches changes
in one transaction. Original transcripts/audio are never rewritten, renamed or
removed. JSONL is preferred; Markdown-only imports retain supported speaker/time
syntax. Invalid data, cross-call references, invalid offsets or inconsistent
duration abort the import.

Legacy names contain local wall time without an offset. Use an explicit IANA
zone when known; otherwise adoption records the machine-local interpretation.
macOS `session_end.duration_s` establishes the legacy end timestamp. Linux files
without exact end time can remain completed with `ended_at_ms=NULL`; no timestamp
is invented from the final turn or file modification time. Missing closing
markers indicate failed/interrupted sessions.

A newer, unrelated or corrupt database fails without reset. Schema version 1 has
no destructive downgrade; future migrations must first use SQLite's backup API,
then run ordered migration transactions.

## Verification

`make test-integration` checks the generated schema, 44 native database checks,
24 Python database tests, 46 native storage fault/recovery checks and eleven
application wiring tests. Coverage includes commit failure, copied queued buffers,
interrupted two-file publication, metadata, Unicode/apostrophes and relative config
paths. Tests use temporary data and mocks, without credentials or cloud calls.
`make test-history` verifies AppKit restart/archive actions; `make test-tray` covers
GTK, zero restored live counters and menu responsiveness with a busy writer.
Linux verification compiles an isolated source copy, never the host's macOS
`bin/` or `build/` outputs.

## Explicit archive reorganization

`scripts/organize_archive.py` previews by default. Its `--apply` mode requires an
inactive app and no running session or AI job. It creates a sibling full archive
backup and SQLite backup, verifies copied files, atomically updates artifact paths
and path-bearing metadata, then removes originals from the active archive. Source
JSONL retains its fields and citation identity; only known path values relocate.
Session, utterance and job IDs remain unchanged. A copy/transaction failure retains
original references and originals. No inference or credential access is involved.
New-layout transcript imports are supported alongside both older native layouts.
