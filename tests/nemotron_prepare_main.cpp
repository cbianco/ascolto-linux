#include "prepare_bf16.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>
struct State { long count = 0, limit = 0; };
static bool cancel(void *p) { auto &s = *static_cast<State *>(p); return s.limit && ++s.count >= s.limit; }
int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) return 2;
    bool verified = argc == 5 && !std::strcmp(argv[4], "verified-replaced");
    State state; if (argc == 5 && !verified) state.limit = std::atol(argv[4]);
    char error[256]{};
    int fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    int input = -1;
    if (verified && fd >= 0) {
        input = openat(fd, "model.safetensors", O_RDONLY | O_NOFOLLOW);
        if (input < 0 || renameat(fd, "model.safetensors", fd, "verified-input")) return 2;
        int replacement = openat(fd, "model.safetensors", O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (replacement < 0 || write(replacement, "unverified replacement", 22) != 22) return 2;
        close(replacement);
    }
    int rc = ascolto_prepare_bf16(fd, input, argv[2], argv[3], cancel, &state, error, sizeof(error));
    if (input >= 0) close(input);
    if (fd >= 0) close(fd);
    if (rc) std::fprintf(stderr, "%s\n", error);
    return rc ? 1 : 0;
}
