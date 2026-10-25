"""Record local native runtime provenance without running model inference."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "build/stt-bench"


def revision(path):
    return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()


def artifact(path, digest=None):
    path = Path(path)
    if digest is None:
        with path.open("rb") as file:
            digest = hashlib.file_digest(file, "sha256").hexdigest()
    return {"path": str(path), "size_bytes": path.stat().st_size, "sha256": digest}


SPECS = [
    ("nemo-nemotron-3.5-q8_0-metal", "nemo", "nemotron-3.5-q8_0.gguf", "metal",
     "nvidia/nemotron-3.5-asr-streaming-0.6b", "1c8deaecc64b91f034d73e08dd8b64625eb3395d",
     "nemotron-3.5-asr-streaming-0.6b.q8_0.gguf",
     "a5c435f294eea8f88ce68dd27b8c3bfea7f777cb2fbba04fcd30eaa555f429ae", False),
    ("nemo-nemotron-3.5-q8_0-metal-streaming", "nemo", "nemotron-3.5-q8_0.gguf", "metal",
     "nvidia/nemotron-3.5-asr-streaming-0.6b", "1c8deaecc64b91f034d73e08dd8b64625eb3395d",
     "nemotron-3.5-asr-streaming-0.6b.q8_0.gguf",
     "a5c435f294eea8f88ce68dd27b8c3bfea7f777cb2fbba04fcd30eaa555f429ae", True),
    ("parakeet-v3-q8_0-metal", "parakeet", "parakeet-v3-q8_0.gguf", "metal",
     "mudler/parakeet-cpp-gguf", "741158ae71e64ef5c89385862c18f777d07a97a1",
     "tdt-0.6b-v3-q8_0.gguf",
     "4d69a4a6683f4f2d952bad794c1357ca6eb628027695b4699c5a9ad4cd07d757", False),
    ("parakeet-ultra-q8_0-metal", "parakeet", "parakeet-ultra-q8_0.gguf", "metal",
     "mudler/parakeet-cpp-gguf", "741158ae71e64ef5c89385862c18f777d07a97a1",
     "ultra-q8_0.gguf",
     "c2fb452a9df468a141012b01c8c168a25ce93f710897c7de6e353c6cc250986a", False),
    ("parakeet-redux-packed-cpu", "parakeet", "parakeet-redux-packed.gguf", "cpu",
     "mudler/parakeet-cpp-gguf", "741158ae71e64ef5c89385862c18f777d07a97a1",
     "redux-packed.gguf",
     "574614b9a4d9f72ab202877a7ad6a1f4bf819dd1d27b9d42dfe8cd429fcebdd5", False),
]

for name, runtime_name, model_file, backend, repo, rev, remote_file, digest, streaming in SPECS:
    runtime = BASE / "runtimes" / runtime_name
    nemo = runtime_name == "nemo"
    executable = runtime / ("bench-nemo" if nemo else "bench-parakeet")
    command = [str(executable), name, backend, str(BASE / "models" / model_file),
               str(BASE / "corpus/manifest.tsv")]
    if streaming:
        command.append("--streaming")
    ggml_source = runtime / ("llama.cpp" if nemo else "third_party/ggml")
    data = {
        "model": name,
        "status": "prepared_inference_not_run",
        "runtime": "NVIDIA/NeMo-Speech.cpp" if nemo else "mudler/parakeet.cpp",
        "runtime_commit": revision(runtime),
        "ggml_source_commit": revision(ggml_source),
        "implementation": ("C harness, public C ABI, C++/ggml native inference; no Python inference" if nemo else
                           "C harness with C++ lifecycle shim, public PCM C ABI, C++/ggml native inference; no Python inference"),
        "backend_requested": backend,
        "backend_observed": None,
        "model_artifact": artifact(BASE / "models" / model_file, digest),
        "model_repository": repo,
        "model_repository_revision": rev,
        "model_url": f"https://huggingface.co/{repo}/resolve/{rev}/{remote_file}",
        "original_checkpoint": "nvidia/nemotron-3.5-asr-streaming-0.6b" if nemo else
            ("moondream/parakeet-redux" if "redux" in name else
             "moondream/parakeet-ultra" if "ultra" in name else "nvidia/parakeet-tdt-0.6b-v3"),
        "runtime_license": "Apache-2.0" if nemo else "MIT",
        "weights_license": "OpenMDW-1.1" if nemo else "CC-BY-4.0",
        "quantization": "packed ternary encoder" if "redux" in name else "Q8_0 mixed precision GGUF",
        "harness": artifact(ROOT / "tools/stt-bench" / runtime_name /
                            ("bench_nemo.c" if nemo else "bench_parakeet.c")),
        "executable": artifact(executable),
        "command_argv": command,
        "environment": {} if nemo else {"PARAKEET_DEVICE": "MTL0" if backend == "metal" else "cpu",
                                        "PARAKEET_REPACK_THREADS": "8"},
        "config": {"mode": "streaming_unpaced_pcm" if streaming else "offline_completed_utterance",
                   "language": "it-IT" if nemo else "automatic (model has no language prompt)",
                   "sample_rate": 16000, "channels": 1, "pcm": "float32",
                   "decoder": "greedy transducer", "threads": 8,
                   "word_timestamps": False, "vad": False, "external_itn": False,
                   "external_punctuation_model": False, "model_self_punctuation": True,
                   "endpointing": False, "concurrency": 1,
                   "load_once": True, "warmup": "first corpus clip once, repeat=-1",
                   "measured_passes": 3},
        "measurements": {"load_wall_ms": "BENCH_LOAD_JSON.wall_ms in stderr",
                         "load_cpu_ms": "BENCH_LOAD_JSON.cpu_ms in stderr",
                         "decode_wall_ms": "total_ms around complete native API call",
                         "decode_cpu_ms": "cpu_ms from process user+system rusage",
                         "peak_rss_mb": "process lifetime maximum RSS; includes corpus and prior clips",
                         "first_token_ms": "first nonempty returned streaming text" if streaming else
                            "null: native utterance API provides no token callback",
                         "tokens": "-1: token count unavailable from selected API"},
        "limitations": ["Only one warmup and three measured corpus passes; warm file cache.",
                        "Native API selection does not establish production integration or accuracy.",
                        "Actual accelerator use must be confirmed from runtime initialization logs."],
        "errors": [],
    }
    if nemo:
        data["library"] = artifact(runtime / "build/metal-asr/bin/libnemo_speech_asr_c.1.dylib")
        data["sentencepiece_commit"] = revision(BASE / "runtimes/nemo-sentencepiece")
        data["build_config"] = {"preset": "metal-asr", "generator": "Unix Makefiles",
                                "GGML_METAL": True, "NEMO_SPEECH_GGML_PATCHED": False,
                                "NEMO_SPEECH_BUILD_CLI": False, "NEMO_SPEECH_BUILD_DIAR": False,
                                "GGML_OPENMP": False, "CMAKE_BUILD_TYPE": "Release"}
        data["config"]["rnnt_right_context"] = 3
        if streaming:
            data["config"].update({"pcm_feed_ms": 20, "encoder_step_ms": 320})
            data["limitations"].append(
                "Streaming input is not wall-clock paced. First text is compute delay from available PCM, "
                "not live acoustic latency; first_kind distinguishes incremental_text from final_result.")
        data["preparation_notes"] = [
            "Built local SentencePiece v0.2.0 static archive with CMAKE_POLICY_VERSION_MINIMUM=3.5 "
            "because the installed CMake rejects its old minimum policy version.",
            "Official GGUF is pinned to the runtime model catalog revision and verified by SHA-256."]
    else:
        data["library"] = artifact(runtime / "build/libparakeet.dylib")
        data["shutdown_shim"] = artifact(ROOT / "tools/stt-bench/parakeet/shutdown.cpp")
        data["config"]["explicit_backend_shutdown_after_model_free"] = True
        data["lifecycle_shim_note"] = (
            "The C ABI exposes no global backend shutdown hook. A C++ shim calls the upstream CLI's "
            "pk::shutdown_backend after model context destruction, before GPU registry static destruction.")
        diff = subprocess.check_output(["git", "-C", str(ggml_source), "diff", "--binary"])
        data["ggml_upstream_patch_diff_sha256"] = hashlib.sha256(diff).hexdigest()
        data["build_config"] = {"PARAKEET_SHARED": True, "PARAKEET_GGML_METAL": True,
                                "PARAKEET_BUILD_SERVER": False, "PARAKEET_WITH_CED": False,
                                "PARAKEET_WITH_VOICEDETECT": False, "GGML_OPENMP": False,
                                "GGML_NATIVE": True, "CMAKE_BUILD_TYPE": "Release"}
        data["preparation_notes"] = [
            "Upstream CMake applied four in-tree ggml patches automatically; no benchmark source patches.",
            "GGUF downloaded at a pinned converter repository revision and verified by SHA-256."]
        if "redux" in name:
            data["limitations"].append(
                "Packed Redux runs CPU only; resident repacked planes add memory beyond the 213 MB file.")
        if "redux" in name or "ultra" in name:
            data["limitations"].append(
                "Moondream derivative converted by mudler; converter transcript checks do not establish "
                "parity with an independent official inference implementation.")
    path = BASE / "results" / f"{name}.metadata.json"
    path.write_text(json.dumps(data, indent=2) + "\n")
    print(path)
