# Implemented design reference

`ascolto.figcraft` is the editable reference for the implemented Ascolto UI and
SQLite model. It contains approved identity vectors, light/dark menu states,
General, Transcription, Recording and Storage controls, the three AI provider
profiles, and three database diagrams. Obsolete proposals, duplicate studies,
hidden iterations and speculative OAuth screens have been removed.

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
