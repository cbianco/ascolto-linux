#include "ggml-backend.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <unistd.h>
extern "C" void ascolto_metal_map_fault_for_test(int);
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error("check failed: " #x); } while (0)
int main() {
    try {
        ggml_backend_load_all();
        auto device=ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);CHECK(device);
        size_t page=size_t(sysconf(_SC_PAGESIZE)),bytes=4*page;
        void *storage=nullptr;CHECK(!posix_memalign(&storage,page,bytes));std::memset(storage,0,bytes);
        auto allocated=[&]{size_t available=0,total=0;ggml_backend_dev_memory(device,&available,&total);return total>available?total-available:0;};
        @autoreleasepool {
            auto warm=ggml_backend_dev_buffer_from_host_ptr(device,storage,bytes,bytes);CHECK(warm);
            ggml_backend_buffer_free(warm);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        size_t before=allocated();
        for(int cycle=0;cycle<20;++cycle)for(int stage=1;stage<=3;++stage){
            @autoreleasepool {
                ascolto_metal_map_fault_for_test(stage);
                auto failed=ggml_backend_dev_buffer_from_host_ptr(device,storage,bytes,bytes);
                CHECK(!failed);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        size_t after=allocated();CHECK(after<=before+page);
        @autoreleasepool {
            auto recovered=ggml_backend_dev_buffer_from_host_ptr(device,storage,bytes,bytes);CHECK(recovered);
            CHECK(ggml_backend_buffer_get_base(recovered)==storage);
            ggml_backend_buffer_free(recovered);
        }
        free(storage);
        std::printf("PASS %d checks; real Metal map failure cleanup (before/after %zu/%zu bytes)\n",checks,before,after);
        return 0;
    }catch(const std::exception &error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
