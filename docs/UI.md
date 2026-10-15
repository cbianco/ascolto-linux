# Native menu design

The approved FigCraft studies define a shared menu hierarchy for macOS and GNOME:

1. Session status and start/stop action.
2. Session details, including the active app and latest phrase.
3. Automatic monitoring and the platform's pause action.
4. Settings, with General, AI providers, Transcription, Recording and Storage.
5. Immediate monitoring shortcuts and archive access.
6. Transcript, folder, keyword, and quit actions.

The panel uses the approved ear symbol while idle, the cupped-hand gesture with
an ear-mounted recording dot while recording, and ear badges for pause and saving.
Elapsed time stays inside the menu; the panel reserves one compact icon slot.
GNOME loads bundled symbolic SVGs from `assets/` through Ayatana AppIndicator's
icon theme path. Keep that directory beside the repository's `bin/` directory.
The desktop launcher artwork remains separate from the panel state symbols.

AppKit and GTK/GNOME own menu selection, font rendering, contrast, and light/dark
appearance. macOS adds a two-line session header; GNOME uses the native exported
menu status row. Permission dialogs remain owned by the operating system.

## Platform behavior

- macOS pauses automatic monitoring; manual start remains available. Audio is
  archived as stereo M4A. Optional Accessibility and Calendar actions live under
  session details. A missing call-audio permission is also exposed while recording.
- Linux pauses and resumes the current transcription. Its automatic-microphone
  setting remains a separate toggle. Audio is archived as separate FLAC channels;
  latest-session folder and spoken-recap generation actions remain available.
- Capture/session/settings actions use each adapter's serial background queue.
  Linux joins outstanding menu actions before final session teardown. Device
  enumeration runs in the background and exposes a default while loading.
- Saving is an explicit state, and new starts and recap actions stay unavailable
  until the current session finishes.

## Verification

On macOS, run `make test` and the opt-in `make test-capture` hardware check.
On Linux, run `make test`. With GTK 3, Ayatana AppIndicator, Xvfb, a session bus
and a GdkPixbuf SVG loader (`librsvg2-common` on Debian/Ubuntu) installed,
`make test-tray` exercises real GTK menu callbacks against the mock
capture backend. It checks selected settings, compact state icons, asynchronous
start/pause/resume/stop, and shutdown during a queued start. Xvfb verifies the
native GTK adapter, not a complete live GNOME Shell session.

## Settings and AI providers

The menu bar and GNOME panel now keep session actions, monitoring shortcuts,
Settings, archive access, and Quit. Configuration lives in a native window with
General, AI providers, Transcription, Recording, and Storage. The first category
is AI providers; reopening the window retains the current category. macOS also
remembers the category across launches. Existing language, speaker, keywords,
source-device, audio archive, and legacy cloud controls remain available.

AI provider setup is shared by both native adapters through
`scripts/ai_providers.py`, with one JSON request on stdin and a sanitized JSON
result on stdout. Network, keyring prompts, OAuth, and model discovery never run
on the UI thread. API keys are masked, can be revealed temporarily, and are not
passed in process arguments. Replacements are verified before storage; a failed
metadata commit restores the previous key. Disconnect keeps local artifacts.

macOS uses Keychain Services. Linux uses libsecret and an unlocked Secret Service
keyring, normally provided by GNOME Keyring. Missing or locked storage produces
an actionable error; there is no plaintext fallback. The config-adjacent SQLite database contains preferences, model routing metadata,
opaque host identity, and account/client registrations. Former JSON/INI metadata
is adopted once with immutable backups. Credentials and retained OAuth
ID/access/refresh tokens live only in the OS store. A process lock serializes
settings changes, credential rotation, and recap requests. The persistence boundary is `ProviderManager.load/save`; native settings, custom
keywords and AI metadata use the same SQLite store. The secure-store namespace
continues to use the original canonical `ai-providers.json` path.

OpenCode Zen and Go keep distinct credentials. Connection verification reads the
[authenticated workspace configuration](https://opencode.ai/v2/docs/console/api/),
rather than treating the anonymous model endpoint as proof of key validity.
Model API families and base URLs come from the workspace catalog, with an HTTPS
OpenCode host allowlist. Go is intended for coding agents; the settings explain
that call-recap support must be confirmed with the provider. Ascolto sends an
honest app user agent and a stable per-call session identifier. It does not
impersonate a coding client. OpenRouter validates the key through its
[key endpoint](https://openrouter.ai/docs/api-reference/api-keys/get-current-key)
and loads text models from the provider catalog.

OpenAI uses the official
[Sign in with ChatGPT flow for local open-source apps](https://developers.openai.com/siwc/token-sharing-open-source/sign-in):
loopback callback, dynamic client registration, stable host identity, state,
PKCE S256, and signed ID-token validation of issuer, audience, expiry, and nonce.
Returning accounts keep their client registration. ID tokens remain in the
secure store and are omitted from browser launch URLs because some Linux
launchers expose URL arguments in process listings. Account model order and
display names come from the account-specific catalog. Text inference uses the
public Responses API with `store: false`, `stream: true`, and a required terminal
completion event. ChatGPT plan credentials never authorize audio transcription
or speech generation in Ascolto. The UI includes the official ChatGPT logo,
Continue with ChatGPT, first-use confirmation, account selection, usage access,
and disconnect/revocation recovery. Personal plan eligibility is determined by
OpenAI; the UI does not promise access for every account or workspace.

A verified connection alone does not enable recaps. The user chooses a model,
selects Use for recaps, and separately enables automatic recaps, which default
to off. Choosing the new provider workflow disables legacy automatic cloud
processing. Finishing a session sends only the existing local transcript through
the existing scrubber, including when audio archives are disabled. No audio
format conversion or speech API is involved. Each call folder keeps `transcript.md` and the latest final `recap.md` together.
`Audio/` contains recordings, `Data/` contains JSON/JSONL, and `Recap-history/`
retains immutable AI attempts. Storage provides a native folder chooser and
shortcuts to the latest call, transcript and recap. Changing the archive folder
applies to new calls and preserves existing references. Cloud transcript attempts
use `Data/Transcription-history/` inside the same call folder.
Provisional and final artifacts remain distinct; local Markdown, JSONL, M4A,
and FLAC remain intact. Truncated, failed,
or interrupted responses preserve an existing recap. There is no automatic
provider or legacy fallback.

`make recap-deps` installs Python dependencies. macOS packaging copies the
prepared environment into `Ascolto.app/Contents/Resources/python`, so an installed
app does not need the repository checkout to locate its dependencies. This local
bundle still uses the host Python installation referenced by the virtual
environment; it is not a standalone redistributable Python distribution. Rebuild
the bundle after dependency setup. Linux runs the workers and shared `ascolto_files.py` in place from the repository
`scripts/` directory; its desktop targets install launchers, not a separate Python
copy. It uses the executable-adjacent virtual environment, or system Python with
an actionable missing-dependency error.

Verification: `make test-ai` covers mock provider and OAuth contracts without
real credentials or paid requests. `make test-settings` renders every AppKit
category and provider profile in light/dark with fixture state and no network.
`make test-tray` covers GTK settings, saved keyword edits, serial session actions,
and light/dark PNG exports under Xvfb. Native preview files are written under
`build/settings-previews` and are not source artifacts. The official ChatGPT logo
assets in `assets/chatgpt-logo*.svg` come from the
[OpenAI sign-in button guidelines](https://developers.openai.com/siwc/website).

## Integrated task controls and storage feedback

Transcription presents a dedicated OpenRouter transcription catalog, an explicit
saved MIC/SYS audio upload checkbox, and an independent post-call automatic
switch. Saving a model without consent never authorizes audio upload. Preview
latest audio upload reports complete-channel duration and request count locally.
The execution plan uses a calculated limit up to 4,096 requests and fails instead
of truncating a longer archive. Whistle remains the live engine for every choice.

Storage exposes provisional recap, final recap and refreshable job status. Manual
menu recap shortcuts open this same native workflow surface so failures and result
locations are visible. Cloud requests run in worker processes; closing the window
cancels its worker. Startup and status refresh fail orphaned jobs only after a
nonblocking session/task lock proves the original worker is gone. They do not
retry provider calls. A completed provisional job never suppresses final notes.

Native transcript storage errors appear in menu/settings snapshots, prevent
automatic AI work, and mark the session failed. Uncommitted turns remain queued
for retry within the running application; committed exports use a durable outbox.
Settings commit failures retain the previous config. GTK persists settings outside
the core mutex; menu callbacks and copied snapshots remain responsive with a
busy SQLite writer. AppKit continues to read only its separate copied snapshot.

Archive menu shortcuts use the newest readable ready local transcript from SQLite
on startup, including imported history and archives outside the current output
directory. Restoring these paths does not restore live recording counters. The
AppKit `make test-history` and GTK `make test-tray` regressions verify actual menu
actions after restart. GTK preview tests assert the official ChatGPT SVG exists
and decodes successfully, and select built-in Adwaita light/dark variants.
