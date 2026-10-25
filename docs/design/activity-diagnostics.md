# Activity and diagnostics

Status: implemented in native AppKit and GTK, 2026-10-16. The editable reference
is `ascolto.figcraft`. The boards describe the hierarchy and control inventory;
shipping windows adapt to the available display and use native appearance.

## Placement and hierarchy

The native **Activity & diagnostics** section shares the app window with settings, reachable from the
menu at all times and from Storage through **View activity…** beside the
selected call's artifacts. A shared sidebar contains Activity & diagnostics and
the existing five Settings categories. Menu shortcuts reuse the same window;
they never create a separate diagnostics window.
After a failure, show a quiet explanatory menu status row with the outstanding
recap count. Historical failures must not be described as the latest call's
failure when a newer call succeeded or contained no speech.

The activity section contains a recent-call list and a selected-call detail pane. Include
all calls and a Needs attention filter. Status belongs to a specific session,
workflow and attempt; never infer it solely from the newest output folder.
An empty call must not hide an earlier successful recap or an unresolved error.

Present a short outcome, preserved artifacts, reason and recovery action first.
Use Overview, Technical details and Attempts views. Color supplements explicit
text and symbols. Use AppKit/GTK controls, system appearance, keyboard navigation,
accessible labels and a scrollable detail pane at smaller window sizes. The
boards are control inventories rather than fixed-size shipping windows.

## Status and recovery

| State | Primary information | Action |
| --- | --- | --- |
| Not requested | Automatic processing disabled or task never started | Explain the reason; offer explicit generation when eligible |
| Skipped | No speech detected; no recap input | Open the call; do not label this a failure |
| Queued | Selected workflow, provider/model and queue time | Refresh or cancel when supported |
| Running | Current observable stage and elapsed time | Cancel request when supported |
| Needs attention | Plain-language cause, available artifacts and failed attempt | Retry when meaningful; fix connection or settings when required |
| Ready | Exact provisional or final artifact for this call | Open recap, transcript or call folder |
| Interrupted | Worker ended before completion | Explain interruption; allow an explicit new attempt |

Represent capture, local transcript persistence, optional cloud transcription
and recap as distinct operations when relevant. A recap failure must not imply
that recording or local transcription failed. An application-idle state must
not imply that all background work has completed.

For an evidence mismatch, explain that a generated citation did not match the
source transcript and publication was blocked. Do not weaken validation or
present the source transcript as lost. For deadline validation, connection,
credential, provider-limit or storage failures, use the corresponding recorded
cause and actionable next step. Unknown failures remain explicitly unknown.

Retry creates a new attempt and retains the previous attempt's status. Retry
uses the explicitly configured provider/model and the existing authorized
transcript workflow. It must not silently switch models, enable audio upload,
change preferences or overwrite a valid recap with a failed attempt. Disable
duplicate requests while that workflow is running. Re-evaluate consent and
eligibility through the existing execution path.

Cancel stops local processing; a provider may continue a request it has already
received. Show a cancelled or interrupted state rather than promising remote
cancellation. No automatic retry is introduced.

## Observable data and diagnostic disclosure

Show the provider/model snapshot, workflow, attempt/job identifier, recorded
timestamps, elapsed time, safe error code, stage and ready artifact links.
During a request, show elapsed time rather than invented progress percentages
or predicted completion time. A completed stage needs supporting recorded data;
missing stage-level timing must remain unavailable.

For the selected historical call at 12:21 on 2026-10-16, the available data is
91 turns, final recap job 6 / attempt 1, OpenRouter with
`nvidia/nemotron-3.5-super-vl:free`, started at 12:26:48, failed at 12:27:22,
approximately 33 seconds, and `invalid_evidence`. The failed provider response
was not retained: no exact mismatched quotation can be shown. Other call labels
on the boards illustrate list states; retry attempt 2 and its 12-second elapsed
time are illustrative, not an operation performed by this design task.

Attempts show immutable status metadata, including attempts without output
files. Do not imply that failed raw responses exist in Recap-history. Show an
existing valid recap separately from a newer failed generation attempt.

**Copy diagnostics…** opens an inline preview of metadata to copy. Exclude transcript
text, audio, credentials and tokens by default. Do not automatically upload a
support report. Read copied snapshots and refresh in the background, without
holding capture locks or running network/database work on the UI thread.

## Editable boards

- **Unified app / Failed recap overview**: shared sidebar, selected historical call, outcome,
  stage list, saved transcript/folder and Retry recap.
- **Unified app / Technical details and attempt history**: model snapshot,
  timing, safe error code and explicit limits of the available evidence.
- **Unified app / Retry in progress**: illustrative second attempt, elapsed
  time, pending validation/publication and cancellation.
- **Unified app / Failed recap · dark appearance**: dark version of the overview.
- **Unified app / Entry points and status language**: menu placement, Storage
  access, diagnostic disclosure and consistent status language.

Existing identity, settings, model selection and database boards are preserved.
All new boards use editable native text and shapes, with no screenshot layers.

## Implementation and verification

`scripts/ascolto_diagnostics.py` supplies a copied, metadata-only snapshot through
`ai_providers.py` action `diagnostics`. It includes every persisted session,
independent operation outcomes, existing ready artifacts and immutable attempt
metadata, including failures without files. List labels use timestamps, not app
or calendar titles. A newer empty session does not hide older results. The
archive recap shortcut independently selects the latest readable final recap.

AppKit embeds the view controller in `src/macos/activity.m` inside the existing
settings window; GTK embeds `src/linux/diagnostics.inc` in its settings page.
Both refresh through independent background workers while a retry is running.
The menu count refers to calls needing attention, including capture/storage
problems, rather than claiming every outstanding failure is a recap failure.
Switching sections preserves the call, filters, details and running requests.
Closing the shared window does not cancel an authorized diagnostics request.

Retry passes a per-session request with the displayed configured provider/model
snapshot. Changed preferences fail explicit validation rather than sending to a
surprise model. Cancellation has its own control worker and uses an owner-only
`.job-ID.cancel` marker. The workflow checks before and after provider requests
and before publication. A pending provider request may continue remotely; the UI
shows cancellation requested until local processing stops. Terminal outcomes
remain in SQLite and the marker is cleaned after completion or reconciliation.
No response text or secrets are included in the diagnostic report.

Verification uses temporary fixtures and mocked providers only:

- `make test-diagnostics`: 18 Python checks for status, independent artifacts,
  missing output, empty calls, guarded retry, cancellation and safe disclosure.
- `make test-activity` on macOS: native list/filter/retry/history assertions and
  AppKit overview/details/attempts previews in light and dark appearance;
  shared-window identity, inline copy preview and request-preserving navigation.
- `make test-tray` in an isolated Linux environment: real GTK callbacks under
  Xvfb, diagnostics fixture selection/filter/copy-preview assertions, plus light
  and dark previews. This does not validate a full GNOME Shell session.

No live provider request, microphone capture or production installation is part
of these checks. Native details display recorded events and elapsed time without
inventing stage completion, output retention or an estimated completion time.
