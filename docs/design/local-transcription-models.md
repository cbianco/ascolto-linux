# Local transcription models

Status: native implementation, 2026-10-16. The editable reference is
`ascolto.figcraft`. AppKit and GTK implement the catalog and independent cloud
tab. The C manager and in-process adapters implement downloads, integrity checks
and next-session selection. See [implementation](../LOCAL_STT.md) for build and
verification details.

## Placement

Keep the existing Settings sidebar and Transcription category. Add two tabs
inside Transcription: **On device** and **Cloud after calls**. The first tab
owns the single live engine choice, its local downloads, language, speaker
recognition and keywords. The second retains the independent existing
post-call cloud workflow, model selection, audio upload consent and manual
preview/run controls. No new top-level sidebar category is required.

Use a four-row list rather than an additional model-management window. Each
row has a model name, a concise reliability/quality indication, a resource
hint, a single-engine selection control and a state-dependent action. The
included Whistle row stays first and selected by default. The three optional
engines are Whisper large-v3-turbo Q5, Parakeet TDT v3 and Qwen3-ASR 1.7B.

## Model copy and download sizes

| Engine | Short description | Secondary indication | Approximate weights |
| --- | --- | --- | --- |
| Whistle | Current baseline. Lower agreement in our sample; very lightweight. | Default; included | 17 MB, bundled |
| Whisper large-v3-turbo Q5 | Closest to the cloud reference in our Italian sample. | Best test agreement | 574 MB |
| Parakeet TDT v3 | Good agreement in our sample, with very low processing delay. | Speed / quality balance | 941 MB |
| Qwen3-ASR 1.7B | Good agreement in our sample. Uses more memory than the other options. | CPU option | 4.70 GB |

Sizes are decimal, rounded weight-file sizes from this machine's benchmark;
they are not peak RAM, complete installed footprint or a promise that every
platform downloads the same package. Actual implementation must show the
selected artifact manifest's complete download size and available disk space.

Avoid stars, invented reliability percentages or an unqualified “most accurate”
badge. The evidence is a small Italian corpus with an automatic cloud reference,
not human ground truth. Whisper shares that reference's model family. A brief
note and **View comparison…** make this limitation discoverable without turning
each model row into a benchmark table. Speed, peak memory and confidence are
different quantities and must not be merged into one quality score.

## Download, verification and selection

| State | Row/action | Selection behavior |
| --- | --- | --- |
| Included default | Whistle; Included; Selected | Selected on a fresh install |
| Not installed | Download; approximate package size | Unavailable for selection |
| Downloading | Real progress, transferred/total bytes, Cancel | Current engine stays selected |
| Verifying | Verifying download… | Cannot activate before integrity succeeds |
| Ready | Downloaded; Use model; Remove… | Explicit selection required |
| Selected | Selected; Downloaded or Included | Exactly one engine selected |
| Interrupted/failed | Actionable error; Retry download | Existing selection is retained |
| Unsupported device/platform | Specific explanation; no usable Download action | Cannot select that package |

The progress and retry boards contain illustrative state values, not an actual
network operation or machine error. Download completion must never implicitly
activate a model. Cancel leaves the current engine intact. Retry applies only
to the chosen package and does not select a substitute. Removal is available
for inactive optional models; the bundled default is not removable. To remove
the selected optional model, first choose another installed engine.

Selecting a ready model saves the next-session preference. A running session
keeps the engine with which it started. During recording, label the action
**Use next session** and distinguish **Recording with** from **Next session**;
never indicate that a new engine has already replaced an active recording's
engine. Pending downloads, network work, model load and unload run in the
background with copied UI snapshots.

Whistle remains available as an explicit choice. If a selected optional model
is missing, corrupt or incompatible, explain the problem and offer **Use
Whistle** or **Retry download**. Do not silently change the saved engine. Local
model selection does not grant cloud audio upload permission or alter cloud
task preferences.

## Native implementation

Keep AppKit/GTK widgets, platform appearance, keyboard navigation and accessible
labels. The shared design defines hierarchy and behavior; it does not prescribe
web controls or one fixed pixel layout. Use the actual downloaded artifact's
availability on each platform rather than assuming Metal exists on Linux.

The selected engine initially replaces text recognition only. Preserve the
existing speaker embedding path and channel/timeline invariants. Do not expose
internal C/C++ or runtime wiring in the primary product flow. The implementation
includes the explicit Parakeet backend shutdown identified in the benchmark.

## Boards in the existing document

- **Transcription / On device · implemented**: Whistle active, three downloads.
- **Transcription / Download in progress**: Whistle active, Whisper downloading,
  Parakeet ready for explicit selection.
- **Transcription / Installed and retry states**: Whisper selected for the next
  session, Parakeet ready, Qwen download interrupted with Retry download.
- **Transcription / On device · dark**: the same initial catalog in dark colors.
- **Transcription / Cloud after calls · implemented**: existing cloud controls under
  the separate tab; “Local transcription only” no longer hardcodes Whistle.

Identity, menu, other settings and SQLite boards remain unchanged. The five
reference boards contain editable FigCraft text, rectangles and ellipses, with
no flattened screenshot layers or hidden alternatives.
