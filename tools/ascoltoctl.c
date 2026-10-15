#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static void default_socket_path(char *buf, size_t cap) {
#ifdef __APPLE__
    const char *runtime = getenv("TMPDIR");
    if (runtime && *runtime) snprintf(buf, cap, "%s/ascolto.sock", runtime);
    else snprintf(buf, cap, "/tmp/ascolto-%d.sock", (int)getuid());
#else
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        snprintf(buf, cap, "%s/ascolto.sock", xdg);
    } else {
        snprintf(buf, cap, "/tmp/ascolto-%d.sock", (int)getuid());
    }
#endif
}

int main(int argc, char **argv) {
    char sock_path[512];
    default_socket_path(sock_path, sizeof(sock_path));

    int arg_idx = 1;
    if (arg_idx + 1 < argc && (strcmp(argv[arg_idx], "-s") == 0 || strcmp(argv[arg_idx], "--socket") == 0)) {
        snprintf(sock_path, sizeof(sock_path), "%s", argv[arg_idx + 1]);
        arg_idx += 2;
    }

    if (arg_idx >= argc || strcmp(argv[arg_idx], "-h") == 0 || strcmp(argv[arg_idx], "--help") == 0) {
        printf("Usage: %s [-s <socket_path>] <COMMAND> [ARGS...]\n\n"
#ifdef __APPLE__
               "Commands (macOS):\n"
               "  STATUS | GET_TRAY_STATE\n"
               "  START | STOP | PAUSE | RESUME\n"
               "  SET_AUTO_MIC | SET_SAVE_AUDIO | SET_CLOUD_RECAP | SET_RECAP_TTS <on|off>\n"
               "  SET_LANG <auto|it|en|es|fr|de|nl|pl>\n"
               "  RECAP | MAKE_MP3 | PREPARE_RECAP [transcript.md]\n",
#else
               "Commands:\n"
               "  STATUS\n"
               "  START | STOP | TOGGLE | PAUSE | RESUME\n"
               "  SET_AUTO_MIC <0|1>\n"
               "  SET_LANG <auto|it|en|es|fr|de|nl|pl>\n"
               "  SET_MIC <device_name>\n"
               "  SET_SYS <device_name>\n"
               "  SET_SENSITIVITY <low|medium|high>\n"
               "  SET_KEYWORDS <comma_separated_keywords>\n"
               "  SIM_MIC_ACTIVE <0|1> [app_name]\n"
               "  INJECT_WAV <MIC|SYS> <wav_path> [hint_text]\n"
               "  INJECT_SYNTH_SPEECH <MIC|SYS> <f0_hz> <f1_hz> <f2_hz> <dur_ms> <text>\n"
               "  LIST_DEVICES | LIST_SPEAKERS | LIST_SEGMENTS | GET_TRAY_STATE | GET_EVENTS\n"
               "  QUIT\n",
#endif
               argv[0]);
        return (arg_idx >= argc) ? 1 : 0;
    }

    char cmd[2048] = {0};
    size_t off = 0;
    for (int i = arg_idx; i < argc && off + 2 < sizeof(cmd); i++) {
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, "%s%s", (i > arg_idx) ? " " : "", argv[i]);
    }
    snprintf(cmd + off, sizeof(cmd) - off, "\n");

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "ascoltoctl: cannot connect to %s: %s\n", sock_path, strerror(errno));
        close(fd);
        return 1;
    }

    if (write(fd, cmd, strlen(cmd)) < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    char buf[8192];
    char response[8192] = {0};
    size_t response_size = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        size_t copied = (size_t)n < sizeof(response) - 1 - response_size ? (size_t)n : sizeof(response) - 1 - response_size;
        memcpy(response + response_size, buf, copied);
        response_size += copied;
        fputs(buf, stdout);
    }
    close(fd);
    if (n < 0 || !response_size) return 1;
    const char *ok = strstr(response, "\"ok\"");
    if (ok && (ok = strchr(ok + 4, ':'))) {
        ok++;
        while (isspace((unsigned char)*ok)) ok++;
        if (!strncmp(ok, "false", 5)) return 1;
    }
    return 0;
}
