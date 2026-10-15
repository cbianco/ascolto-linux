#include "ascolto/filesystem.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int ascolto_mkdir_p(const char *directory) {
    if (!directory || !*directory) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s", directory);
    size_t length = strlen(path);
    if (length > 1 && path[length - 1] == '/') path[length - 1] = '\0';
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(path, 0755) && errno != EEXIST ? -1 : 0;
}

int ascolto_mkdir_parent(const char *filename) {
    if (!filename || !*filename) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s", filename);
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return 0;
    *slash = '\0';
    return ascolto_mkdir_p(path);
}
