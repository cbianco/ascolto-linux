#ifndef ASCOLTO_JSON_H
#define ASCOLTO_JSON_H
#include <stdio.h>
/* Small string writer for transcript/config values, without a JSON dependency. */
static inline void ascolto_write_json_string(FILE *file, const char *value) {
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', file);
        if (*p < 32) fprintf(file, "\\u%04x", *p);
        else fputc(*p, file);
    }
    fputc('"', file);
}
#endif
