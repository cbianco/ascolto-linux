#pragma once
#include <cstddef>

/* Private preparation ABI v1. Runs synchronously on the model worker, before
 * publication. The callback remains valid until return. No inference/backends
 * are initialized. Recipe SHA is compiled into the model-specific adapter.
 * The caller retains staging_fd until return. verified_input_fd, when >= 0,
 * must be the same regular descriptor whose full manifest digest was verified;
 * -1 makes the standalone tool open and verify the input itself. */
using ascolto_prepare_cancel = bool (*)(void *);
int ascolto_prepare_bf16(int staging_fd, int verified_input_fd, const char *resources, const char *recipe_sha256,
                        ascolto_prepare_cancel, void *, char *error, size_t capacity) noexcept;
