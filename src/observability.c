#define _POSIX_C_SOURCE 200809L
#include "ascolto/observability.h"
#include "ascolto/core.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

void ascolto_obs_emit(ascolto_obs_t *obs, const char *event_type, const char *json_payload_fields) {
    if (!obs || !event_type) return;
    pthread_mutex_lock(&obs->mutex);
    obs->event_seq++;
    uint64_t ts_ms = ascolto_monotonic_ms();
    char line[1024];
    if (json_payload_fields && *json_payload_fields) {
        snprintf(line, sizeof(line),
                 "{\"seq\":%llu,\"ts_ms\":%llu,\"event\":\"%s\",%s}",
                 (unsigned long long)obs->event_seq,
                 (unsigned long long)ts_ms,
                 event_type,
                 json_payload_fields);
    } else {
        snprintf(line, sizeof(line),
                 "{\"seq\":%llu,\"ts_ms\":%llu,\"event\":\"%s\"}",
                 (unsigned long long)obs->event_seq,
                 (unsigned long long)ts_ms,
                 event_type);
    }

    size_t idx = (obs->ring_head + obs->ring_count) % ASCOLTO_OBS_RING_SIZE;
    if (obs->ring_count == ASCOLTO_OBS_RING_SIZE) {
        obs->ring_head = (obs->ring_head + 1) % ASCOLTO_OBS_RING_SIZE;
    } else {
        obs->ring_count++;
    }
    snprintf(obs->ring[idx], sizeof(obs->ring[idx]), "%s", line);

    if (obs->log_fp) {
        fprintf(obs->log_fp, "%s\n", line);
        fflush(obs->log_fp);
    }
    pthread_mutex_unlock(&obs->mutex);
}

static void *obs_server_loop(void *arg) {
    ascolto_obs_t *obs = (ascolto_obs_t *)arg;
    char cmd[2048];
    char *reply = (char *)malloc(65536);
    if (!reply) return NULL;

    while (obs->running) {
        struct pollfd pfd;
        pfd.fd = obs->listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int prc = poll(&pfd, 1, 30);
        if (prc <= 0) continue;

        int cfd = accept(obs->listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        fcntl(cfd, F_SETFD, FD_CLOEXEC);

        struct pollfd cpfd;
        cpfd.fd = cfd;
        cpfd.events = POLLIN;
        cpfd.revents = 0;
        if (poll(&cpfd, 1, 1000) > 0) {
            ssize_t n = read(cfd, cmd, sizeof(cmd) - 1);
            if (n > 0) {
                cmd[n] = '\0';
                reply[0] = '\0';
                ascolto_obs_handle_command(obs, cmd, reply, 65536);
                size_t rlen = strlen(reply);
                size_t sent = 0;
                while (sent < rlen) {
                    ssize_t w = write(cfd, reply + sent, rlen - sent);
                    if (w <= 0) break;
                    sent += (size_t)w;
                }
            }
        }
        close(cfd);
    }

    free(reply);
    return NULL;
}

int ascolto_obs_init(ascolto_obs_t *obs, ascolto_core_t *core, const char *socket_path, const char *log_path) {
    if (!obs) return -1;
    memset(obs, 0, sizeof(*obs));
    obs->core = core;
    obs->listen_fd = -1;
    pthread_mutex_init(&obs->mutex, NULL);

    if (log_path && *log_path) {
        snprintf(obs->log_path, sizeof(obs->log_path), "%s", log_path);
        obs->log_fp = fopen(log_path, "a");
        if (obs->log_fp) {
            fcntl(fileno(obs->log_fp), F_SETFD, FD_CLOEXEC);
        }
    }

    if (socket_path && *socket_path) {
        snprintf(obs->socket_path, sizeof(obs->socket_path), "%s", socket_path);
        unlink(obs->socket_path);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", obs->socket_path);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(fd, 32) == 0) {
                obs->listen_fd = fd;
                obs->running = true;
                pthread_create(&obs->server_thread, NULL, obs_server_loop, obs);
            } else {
                close(fd);
            }
        }
    }
    return 0;
}

void ascolto_obs_destroy(ascolto_obs_t *obs) {
    if (!obs) return;
    if (obs->running) {
        obs->running = false;
        pthread_join(obs->server_thread, NULL);
    }
    if (obs->listen_fd >= 0) {
        close(obs->listen_fd);
        obs->listen_fd = -1;
    }
    if (obs->socket_path[0]) {
        unlink(obs->socket_path);
    }
    if (obs->log_fp) {
        fclose(obs->log_fp);
        obs->log_fp = NULL;
    }
    pthread_mutex_destroy(&obs->mutex);
}
