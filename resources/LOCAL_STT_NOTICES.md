# Optional local speech models

The app includes native runtime adapters, not the optional weights. Downloading
weights is separate from selecting an engine. Public downloads use the exact
URLs, complete file sizes and SHA-256 digests in `local-stt-manifest.json`.

| Component | Pinned source | Runtime license | Weights and attribution |
| --- | --- | --- | --- |
| Whisper | [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp/tree/d1be6fde11ac6e0407606b4e42fe72d34add8037), v1.9.5 | MIT, including its bundled ggml | OpenAI Whisper large-v3-turbo, Q5_0 conversion from [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp/tree/5359861c739e955e79d9a303bcbc70fb988958b1), MIT |
| Parakeet | [mudler/parakeet.cpp](https://github.com/mudler/parakeet.cpp/tree/5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f), ggml e705c5fed490514458bdd2eaddc43bd098fcce9b and upstream's in-tree patches | MIT | NVIDIA [parakeet-tdt-0.6b-v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3), [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); Q8_0 GGUF conversion by [mudler](https://huggingface.co/mudler/parakeet-cpp-gguf/tree/741158ae71e64ef5c89385862c18f777d07a97a1) |
| Qwen | [antirez/qwen-asr](https://github.com/antirez/qwen-asr/tree/924694251d9e0f18e5d86bbd06aa3ab5f870002d) | MIT | Qwen team / Alibaba Cloud [Qwen3-ASR-1.7B](https://huggingface.co/Qwen/Qwen3-ASR-1.7B/tree/7278e1e70fe206f11671096ffdd38061171dd6e5), [Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0) |

Runtime license texts are copied into `licenses/` by the native runtime build.
Whisper and Parakeet each statically include their own ggml revision and expose
only Ascolto's versioned adapter entry point. Parakeet calls its upstream backend
shutdown after releasing its model context. Qwen uses Accelerate on macOS and
OpenBLAS on Linux; it exposes no word timestamps or word probabilities.

The short quality descriptions refer to a small Italian sample with an automatic
cloud reference, not a human-labeled accuracy evaluation. Whisper shares the
reference model family. They are not a ranking for every language or workload.
