# Optional local speech models

The app includes native runtime adapters, not the optional weights. Downloading
weights is separate from selecting an engine. Public downloads use the exact
URLs, complete file sizes and SHA-256 digests in `local-stt-manifest.json`.

| Component | Pinned source | Runtime license | Weights and attribution |
| --- | --- | --- | --- |
| Whisper | [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp/tree/d1be6fde11ac6e0407606b4e42fe72d34add8037), v1.9.5 | MIT, including its bundled ggml | OpenAI Whisper large-v3-turbo, Q5_0 conversion from [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp/tree/5359861c739e955e79d9a303bcbc70fb988958b1), MIT |
| Parakeet | [mudler/parakeet.cpp](https://github.com/mudler/parakeet.cpp/tree/5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f), ggml e705c5fed490514458bdd2eaddc43bd098fcce9b and upstream's in-tree patches | MIT | NVIDIA [parakeet-tdt-0.6b-v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3), [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); Q8_0 GGUF conversion by [mudler](https://huggingface.co/mudler/parakeet-cpp-gguf/tree/741158ae71e64ef5c89385862c18f777d07a97a1) |
| Qwen | [antirez/qwen-asr](https://github.com/antirez/qwen-asr/tree/924694251d9e0f18e5d86bbd06aa3ab5f870002d) | MIT | Qwen team / Alibaba Cloud [Qwen3-ASR-1.7B](https://huggingface.co/Qwen/Qwen3-ASR-1.7B/tree/7278e1e70fe206f11671096ffdd38061171dd6e5), [Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0) |
| Nemotron 3.5 Transcribe | [NVIDIA/NeMo-Speech.cpp](https://github.com/NVIDIA/NeMo-Speech.cpp/tree/66d50d91b75a406becd95fbaf39d2c9c13e95d7c), its patch series over llama.cpp `bd4f514db14d87fded667787a7a963bfbaa98e89`, SentencePiece `31646a467d2051eb904e0b45de3a73e91fe1c1e3` | Apache-2.0; MIT llama.cpp/ggml; Apache-2.0 SentencePiece and retained bundled dependency notices | NVIDIA [Nemotron 3.5 Transcribe 30B-A4B](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Transcribe-30B-A4B); community Q8_0 decoder and F16 encoder by [AtomicChat](https://huggingface.co/AtomicChat/NVIDIA-Nemotron-3.5-Transcribe-30B-A4B-GGUF/tree/dbd06c5cad7a2f33c8e06be2fb3ef814dbd9aa59), [OpenMDW-1.1](https://openmdw.ai/license/1-1/). The decoder uses Q8_0 block quantization; it is distinct from NVIDIA's FP8 safetensors export. |

Runtime license texts are copied into `licenses/` by the native runtime build.
Whisper and Parakeet each statically include their own ggml revision and expose
only Ascolto's versioned adapter entry point. Parakeet calls its upstream backend
shutdown after releasing its model context. Qwen uses Accelerate on macOS and
OpenBLAS on Linux; it exposes no word timestamps or word probabilities.

Nemotron statically includes NeMo's ASR and SALM implementation, llama.cpp/ggml,
and SentencePiece. The private build overlay renames the ASR formatting helper
and removes its standalone llama logging shim to avoid collisions inside one
adapter. The overlay also releases raw decoder resources if construction throws.
No dependency symbols cross the adapter ABI. Its model download also
includes the model license. CUDA is excluded because upstream allocation OOM
can terminate the host process. Ascolto implements bounded BF16 and Q8_0 expert streaming; see the
[SSD investigation](../docs/design/nemotron-ssd-streaming.md).

The short quality descriptions refer to a small Italian sample with an automatic
cloud reference, not a human-labeled accuracy evaluation. Whisper shares the
reference model family. They are not a ranking for every language or workload.

The optional `nemotron-3.5-bf16-ssd` package uses NVIDIA's original checkpoint
at revision `a99e33f698257b8b69ca7834f19c73582eb1fa7e` (safetensors SHA-256
`03142dde5176c1d4df21ee829642745a1d670ea4d79e1a4684c64d53195acff2`),
under OpenMDW-1.1. Its decoder keeps BF16 weights; the perception encoder uses
lossless BF16-to-F32 expansion. Unused MTP and timestamp heads are excluded.
`nemotron-bf16-preparation/` contains tokenizer/model metadata and 58,624 bytes
of reference-transformed constants, with the model license included. The native
repacker and SSD cache are Ascolto code. The design was informed by antirez/ds4;
no DS4 source is linked into the app. Native runtime revisions and retained
third-party notices are the same as the Q8_0 adapter above.

The optional `nemotron-3.5-q8` package uses AtomicChat's Q8_0 decoder
(SHA-256 `3d99075b58f15153d87e8922f54e12dda818bbb505c2e0d7d50cd4f30beac9f2`)
and the same F16 encoder, revision and OpenMDW-1.1 license as the retired community
NVFP4 package. Q8_0 is an 8-bit GGUF block quantization, distinct from NVIDIA's
FP8 export. Ascolto's native adapter streams packed expert weights through a
bounded SSD cache; it does not dequantize the complete model into RAM.
