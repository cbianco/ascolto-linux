# Ascolto design reference

`ascolto.figcraft` is the editable reference for the Ascolto UI and SQLite model.
It contains approved identity vectors, light/dark menu states,
General, Transcription, Recording and Storage controls, the three AI provider
profiles, and three database diagrams. Obsolete proposals, duplicate studies,
hidden iterations and speculative OAuth screens have been removed.

**Identity / App icon · listening without recording badge** defines the Dock
and Command-Tab identity. It reuses the approved cupped-hand/ear vector without
the recording dot or its circular knockout, with deep green (`#17372f`) strokes
on a warm ivory (`#f5f6ed`) rounded tile. The editable board includes a master,
Dock preview and 32/48/64/128 px checks. The app icon stays neutral regardless of
recording state; menu-bar state symbols remain independent. An editable SVG
export is available in [ascolto-app-icon.svg](ascolto-app-icon.svg). The macOS
bundle uses `assets/Ascolto.icns`, generated from the approved transparent
1024px master `assets/ascolto-app-icon.png`. The runtime renders that known
resource on a transparent 1024px canvas with a 100px optical inset (824px tile),
matching the measured footprint of native macOS icons. It applies this image
after activation rather than caching a potentially generic Workspace icon.
Menu state symbols remain independent.

The Transcription boards contain the implemented local-model interface,
labeled **IMPLEMENTED**. Native widget spacing follows the host platform.
Whistle stays the included default; Whisper large-v3-turbo Q5, Parakeet TDT v3
and Qwen3-ASR 1.7B are optional downloads. The boards show the initial catalog,
download progress, installed/selected and retry states, a dark appearance, and
the separate Cloud after calls tab. See
[Local transcription model design](local-transcription-models.md) for behavior,
copy, evidence limits and the distinction between downloading and activation.

Five **Unified app /** boards record the implemented Activity & diagnostics design,
including a historical failed recap, technical details and attempt history,
an illustrative running retry, dark appearance and menu/Storage entry points.
Activity and settings share one window and navigation; the diagnostic copy
preview stays inline. The workflow uses native AppKit/GTK controls and adaptive layout. See
[Activity and diagnostics](activity-diagnostics.md) for status semantics,
available evidence, recovery behavior and diagnostic disclosure.

The settings boards are compact control inventories shared by AppKit and GTK,
with platform differences written on the boards. Their spacing and combined
control rows are explanatory; native system widgets own the shipped layout,
appearance and accessibility. Session values are examples, not captured data.
The ChatGPT sign-in button retains its white logo and text on black.

The diagrams summarize selected columns and relationships. The authoritative
schema is `scripts/database_schema.sql`; `docs/DATABASE.md` describes persistence.
The implementation references are `src/macos/settings.m`,
`src/menubar_macos.m`, `src/linux/settings.inc` and `docs/UI.md`.

Open the document in FigCraft to edit its native text, shapes and vectors. It
contains no flattened screenshot layers or hidden alternate boards. Runtime
native previews remain generated artifacts in `build/settings-previews`.
