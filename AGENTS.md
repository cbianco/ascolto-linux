# Ascolto

Native C/Obj-C call transcription app for Linux (PipeWire/GNOME) and macOS
(CoreAudio/AppKit), forked from `Voloire/ascolto-linux`.

- Converse in Italian; write new persisted text in English.
- Preserve both builds and upstream Linux source paths. Keep platform APIs native.
- Capture only enqueues PCM; processing runs on the consumer. Drain before closing
  artifacts and represent overflow as equal-duration silence on both channels.
- Keep UI work responsive through copied snapshots and background operations.

Read the relevant linked instructions before changing that area:

- [Architecture and source map](docs/ARCHITECTURE.md)
- [macOS capture, lifecycle and UI rules](docs/MACOS.md)
- [Build, tests and offline tools](docs/DEVELOPMENT.md)
- [SQLite, settings and archive persistence](docs/DATABASE.md)
- [AI providers and workflows](docs/AI_WORKFLOWS.md)
- [Native UI and design](docs/UI.md)
- [Linux/macOS integration boundaries](docs/INTEGRATION.md)
