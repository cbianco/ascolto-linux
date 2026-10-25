# Native NeMo benchmark

`bench_nemo.c` consumes the shared raw-float manifest through the public C ABI.
The recognizer loads once. It warms the first clip once and runs the corpus
three times. Whole-utterance recognition exposes no token callback, so its
first-token field stays null. An optional `--streaming` mode feeds 20 ms blocks
without pacing, uses right context 3 (320 ms encoder step), drains each update,
flushes the tail, and records the first nonempty returned text. This is compute
delay from already available PCM, not acoustic latency during live capture.

`build.sh` links the harness against the prepared Metal library.
`write_metadata.py` records all five NeMo/Parakeet configurations without
performing inference. Do not rerun it after measurements without preserving
the measured status fields.

The runtime was built at `083861c241061562b8ec29eb06d7f6cdc3af8775` with
`scripts/configure.sh metal-asr -G 'Unix Makefiles'`, CLI, diarization and OpenMP
disabled, and a local SentencePiece v0.2.0 archive. Metal's unpatched ggml path
is the upstream preset. All model inference runs in native C++/ggml.
