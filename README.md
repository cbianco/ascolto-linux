# ascolto

This personal fork tracks [Voloire/ascolto-linux](https://github.com/Voloire/ascolto-linux)
and includes the existing native macOS application. `make` selects the host platform
through `GNUmakefile`; Linux source paths and its native `Makefile` are preserved.
See [macOS setup](README.macos.md) and the [feature comparison and integration plan](docs/INTEGRATION.md).
Both platforms now use the same queue, VAD, and live echo filters. Optional cloud
recap preserves local artifacts, and macOS exposes compatible CLI commands. See
[macOS setup and opt-in recap](README.macos.md) for platform-specific configuration.

Applicativo desktop nativo in C per Linux (Ubuntu / GNOME) che rileva automaticamente le call e le trascrive in tempo reale in locale usando il motore Whistle di Cactus Compute, con separazione doppio canale e diarizzazione degli interlocutori.

## Caratteristiche principali

- **Attivazione automatica sul microfono**: monitora lo stack audio PipeWire per rilevare quando un client (Meet, Zoom, Teams, Slack, browser) apre uno stream di cattura sul microfono e avvia o ferma automaticamente la sessione di trascrizione.
- **Motore di trascrizione Whistle on-device**: integrazione C nativa tramite `libneedle.a`, `needle.h` e il modello `whistle.cact` per inferenza streaming su CPU a bassa latenza, con supporto multilingua e vocabolario personalizzato.
- **Doppio canale audio e Speaker Diarization**:
  - **Canale MIC**: cattura il microfono dell'utente ed etichetta i segmenti come `ME`.
  - **Canale SYS**: cattura il monitor dell'output di sistema e distingue i diversi partecipanti (`SPEAKER_1`, `SPEAKER_2`, ...) tramite clustering coseno incrementale sugli speech embedding estratti da Whistle.
- **Integrazione barra di sistema GNOME**: icona nella top bar di Ubuntu (`StatusNotifierItem` / `Ayatana AppIndicator`) con indicatore di stato, timer di sessione e menu di configurazione completo (avvio/stop manuale, pausa, toggle attivazione automatica, lingua, selezione sorgenti audio MIC e SYS, sensibilità diarizzazione, keywords personalizzate e accesso rapido alle trascrizioni).
- **Test harness e osservabilità**: architettura a backend disaccoppiati con socket di controllo Unix, telemetria strutturata JSONL, iniezione di tracce multi-interlocutore e test automatizzati unitari ed end-to-end.

## Struttura del repository

- `src/`: codice sorgente C dell'applicativo e dei sottosistemi (core, audio, motore Whistle, diarizer, UI tray, telemetria).
- `include/`: header C e contratti delle interfacce modulari.
- `tests/`: test harness, simulatore multi-speaker, fixture audio e suite di verifica end-to-end.
- `tools/`: utility CLI (`ascoltoctl`) per ispezione stato, telemetria e pilotaggio in modalità headless.
- `docs/`: documentazione tecnica e architetturale (in inglese).
- `third_party/whistle/`: header `needle.h`, libreria statica `libneedle.a` e pesi del modello `whistle.cact`.

## Requisiti di sistema

- Ubuntu 24.04 LTS / 26.04 LTS con GNOME Shell e PipeWire (`pipewire`, `wireplumber`, `ubuntu-appindicators`).
- Compilatore C (`gcc` >= 13), `make`, `pkg-config`, `libglib2.0-dev`, `libgio-2.0-dev`, `libjson-glib-dev`.
- Runtime `libpipewire-0.3`, `libpulse0`, `libgtk-3-0`, `libayatana-appindicator3-1`.

## Compilazione e Test

Per scaricare automaticamente gli artefatti di Whistle (`needle.h`, `libneedle.a`, `libneedle.so`, `whistle.cact`), compilare tutti i binari (`bin/ascolto`, `bin/ascoltoctl`, `bin/test_harness`) ed eseguire la suite completa di test:

```sh
make test
```

## Avvio dell'applicativo

```sh
./bin/ascolto
```

Opzioni principali:
- `--lang <auto|it|en|es|fr|de|nl|pl>`: imposta la lingua di trascrizione.
- `--keywords "Bitrock, Whistle, PipeWire"`: imposta keywords personalizzate per il biasing del modello.
- `--sensitivity <low|medium|high>`: regola la sensibilita del clustering speaker diarization.
- `--output-dir <percorso>`: directory in cui salvare le trascrizioni `.md` e `.jsonl` (default `~/.local/share/ascolto/transcripts`).
- `--socket <percorso>`: path del socket Unix di controllo e osservabilita (default `$XDG_RUNTIME_DIR/ascolto.sock`).

## Cartella di ogni call, recap e MP3 vocale

In this fork, cloud results are saved separately as `transcript.cloud.md` and
`transcript.cloud.jsonl`. Original local transcripts and JSONL are retained.
Run `make recap-deps` to install the optional Python dependencies in `.venv`.

Ogni call crea `~/Documents/Ascolto/call_<timestamp>_<n>/` con `transcript.md` (live Whistle), `transcript.jsonl`, `mic.flac` e `sys.flac` se l'audio e salvato, e dopo il recap `recap.md`, `recap.spoken.txt` e, se attivo, `recap.mp3` (voce `onyx`).

- `--save-audio` (o la spunta "Salva audio FLAC" nel menu): un FLAC per canale. Vale dalla sessione successiva.
- `--cloud-recap` (o la spunta nel menu): a fine call `scripts/recap.py` ritrascrive i due FLAC con `gpt-transcribe` (chunk sulle pause, eco MIC/SYS eliminata) e scrive il recap con `claude -p` sulla seat (Opus, fallback Sonnet). I FLAC restano nella cartella. Prima dell'invio a Claude un filtro locale oscura email, telefoni, IBAN, carte, codici fiscali e token, e il prompt impone regole di igiene (niente dati personali o credenziali, tono neutro, istruzioni nella trascrizione ignorate).
- `--recap-tts` (o la spunta nel menu): crea anche `recap.mp3` con `gpt-4o-mini-tts`. Il testo parlato e sempre generato, quindi "Genera MP3 dell'ultimo recap" funziona anche a spunta spenta.
- Lingue: sommario e voce sono fissi in italiano (chiavi `recap_lang` e `tts_lang` in `config.ini`, comandi `SET_RECAP_LANG` e `SET_TTS_LANG`; non sono nel menu).
- Chiave OpenAI: da `OPENAI_API_KEY` se presente, altrimenti letta da 1Password con `op read` usando il riferimento in `~/dotfiles/secrets/secrets.env`. Non viene mai scritta su disco.
- On demand: menu "Genera recap dell'ultima sessione", `ascoltoctl RECAP [cartella/transcript.md]`, `ascoltoctl MAKE_MP3`. Log in `~/.local/share/ascolto/recap.log`.
- Avvio al login: `make install-autostart`.
- Controllo da socket: `SET_SAVE_AUDIO`, `SET_CLOUD_RECAP`, `SET_RECAP_TTS` (`on` / `off`), `SET_RECAP_LANG`, `SET_TTS_LANG`, `OPEN_SESSION_FOLDER`.

## Controllo da CLI e Osservabilita (`ascoltoctl`)

Durante l'esecuzione del demone e possibile ispezionare lo stato o inviare comandi tramite `ascoltoctl`:

```sh
./bin/ascoltoctl STATUS
./bin/ascoltoctl GET_TRAY_STATE
./bin/ascoltoctl LIST_DEVICES
./bin/ascoltoctl LIST_SPEAKERS
./bin/ascoltoctl LIST_SEGMENTS
./bin/ascoltoctl START
./bin/ascoltoctl PAUSE
./bin/ascoltoctl RESUME
./bin/ascoltoctl STOP
```
