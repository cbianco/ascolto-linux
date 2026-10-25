/* Synthetic artifacts: no speech weights or network access in this suite. */
#ifdef ASCOLTO_LOCAL_STT_TEST_HTTPS
static const artifact_t fixture_files[] = {{"model.bin", "https://huggingface.co/Qwen/Qwen3-ASR-1.7B/resolve/7278e1e70fe206f11671096ffdd38061171dd6e5/config.json", "2e74a751548b8ad7d7526d29365ad8144c345d8b412b1152d25dc6698452712f", 6194}};
#else
static const artifact_t fixture_files[] = {{"model.bin", "file:///tmp/ascolto-local-stt-download-fixture.bin", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 3}};
#endif
static const package_t packages[] = {{NULL,0},{fixture_files,1},{fixture_files,1},{fixture_files,1}};
