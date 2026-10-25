# Cohere native benchmark

`prepare.sh` obtains the official sherpa-onnx v1.13.8 arm64 shared libraries,
matching public C header, and the project's INT8 ONNX export of Cohere Transcribe
2B dated 2026-04-01 from its export author's pinned Hugging Face mirror. The C
harness calls sherpa-onnx's C ABI. Both sherpa-onnx
and ONNX Runtime have C++ inference implementations. Python is not used for
inference. Runtime and model weights use Apache-2.0 licenses.

Run from the repository root after preparation:

```sh
build/stt-bench/cohere-bench \
  "$PWD/build/stt-bench/models/cohere-transcribe-int8-hf" \
  "$PWD/build/stt-bench/corpus/manifest.tsv" 3 \
  "$PWD/build/stt-bench/results/cohere-extra.jsonl" 8 \
  > build/stt-bench/results/cohere.jsonl \
  2> build/stt-bench/results/cohere.log
```

The process loads once, warms up with the first clip (`repeat=-1`), then runs
three measured corpus passes (`repeat=0,1,2`). Recognition uses the ONNX Runtime
CPU provider, 8 threads, greedy search, explicit Italian, punctuation and inverse
text normalization. A new independent offline stream is created per clip.
Total call time includes stream creation, waveform acceptance, feature extraction,
decode, final result adaptation and stream destruction. PCM files are read before
model loading and timing.

The offline C API returns a final result and has no token callback. Consequently
`first_token_ms` is null; the sidecar's `first_observed_result_ms` is explicitly
the final-result latency. The backend exposes no GPU memory counter in this
configuration. Token timestamps are not provided by this model path. This
benchmark does not test microphone capture, segmentation, or speaker embeddings.

Sources: [runtime release](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.8),
[export instructions](https://k2-fsa.github.io/sherpa/onnx/cohere_transcribe/pretrained.html),
[pinned export](https://huggingface.co/csukuangfj2/sherpa-onnx-cohere-transcribe-14-lang-int8-2026-04-01/tree/156a470cf08eefe706a0004f3c52d9ee567ca7a0),
[official checkpoint](https://huggingface.co/CohereLabs/cohere-transcribe-03-2026).
