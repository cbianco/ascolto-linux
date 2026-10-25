# Native Parakeet benchmark

`bench_parakeet.c` calls the persistent public PCM C ABI for v3, Ultra and Redux.
It loads one model, warms the first corpus clip, then runs three corpus passes.
The selected API exposes one final string with no token callback; first-token
latency is null and unavailable token counts are `-1`.

The runtime was built at `5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f` with Release,
shared library and Metal enabled; server, CED, voice detection and OpenMP were
disabled. Upstream CMake applies four in-tree ggml patches. The default compute
thread budget is eight. Set `PARAKEET_DEVICE=MTL0` for v3/Ultra Q8 and
`PARAKEET_DEVICE=cpu PARAKEET_REPACK_THREADS=8` for packed ternary Redux.
Backend logs must confirm the actual device before reporting GPU execution.
The literal selector `Metal` is not the registry device name on this Mac and
silently falls back to CPU after logging that the requested device was not found.

The public C ABI frees model contexts but does not expose a process backend
shutdown hook. `shutdown.cpp` calls the same internal `pk::shutdown_backend()`
used by the upstream CLI, after `parakeet_capi_free` releases the model. This
releases the global backend before Metal's device registry static destructors.
The first Metal attempts emitted all 25 rows but aborted during process exit
because this explicit shutdown was missing. Their raw evidence is retained in
`build/stt-bench/results/archive/teardown-crash/`; it is excluded from the final
configuration table when a successful rerun replaces it.

`build.sh` links the harness against this prepared library.
`../nemo/write_metadata.py` writes pinned provenance and exact command arguments.
The measurement harness is C with a small C++ teardown shim; model inference is
the runtime's native C++/ggml implementation.
