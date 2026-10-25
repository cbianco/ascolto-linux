/* Match upstream CLI teardown after all model contexts have been released. */
#include <cstddef>
#include "ggml_graph.hpp"

extern "C" void bench_parakeet_shutdown(void) noexcept {
    pk::shutdown_backend();
}
