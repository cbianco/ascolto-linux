# Qwen native C benchmark

This harness uses the pinned `antirez/qwen-asr` CPU C implementation and official
Qwen BF16 model files. Apple Accelerate supplies BLAS; no Python code performs
inference. Setup and result orchestration use Python's standard library.

Preparation does not run inference:

```sh
git clone https://github.com/antirez/qwen-asr.git build/stt-bench/runtimes/qwen-asr
git -C build/stt-bench/runtimes/qwen-asr checkout 924694251d9e0f18e5d86bbd06aa3ab5f870002d
sh tools/stt-bench/qwen/build.sh
python3 tools/stt-bench/qwen/prepare.py
```

Run each model only when the machine is reserved for benchmarking, with no other
inference, compilation, or model downloads in progress:

```sh
python3 tools/stt-bench/qwen/run.py --model qwen3-asr-0.6b
python3 tools/stt-bench/qwen/run.py --model qwen3-asr-1.7b
```

The default manifest is `build/stt-bench/corpus/manifest.tsv`. Each line contains a
clip identifier and absolute float32 mono 16 kHz PCM path separated by a tab.
All PCM is read before model initialization. Each process loads its model once,
runs the first clip as warmup (`repeat=-1`), and runs all clips in manifest order
for three measured passes (`repeat=0,1,2`). Italian is forced, decoding is greedy,
segmentation and silence removal are disabled, and Qwen's thread pool and
Accelerate thread limit are both set to eight.

JSONL output and diagnostics remain in `build/stt-bench/results/`. Metadata
records the runtime commit, compiler, linked libraries, model revisions, exact
file hashes and sizes, configuration, command, status, and errors. Existing
result files are never overwritten by the runner.

`first_token_ms` measures the first nonempty text callback while decoding an
already complete clip. It does not measure incremental audio latency. Model
load is timed separately; `total_ms` encloses the complete raw-PCM ASR API call.
CPU time includes user and system time across threads. Peak RSS is the process
high-water mark, including loaded corpus PCM and model/workspace allocations.
