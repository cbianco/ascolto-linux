#include "ascolto/capture_buffer.h"
#include <string.h>

int ascolto_capture_buffer_verify(void) {
    ascolto_capture_buffer_t* b = ascolto_capture_buffer_create(16000 * 30);
    if (!b) return 1;
    float source[3200], mic[320], sys[320];
    for (int i = 0; i < 3200; i++) source[i] = (float)(i % 320 + 1);
    /* A slow ASR consumer pauses while both producer callbacks continue. */
    for (int i = 0; i < 100; i++) {
        ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_SYS, source, 3200, i * 3200);
        ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_MIC, source, 3200, i * 3200);
    }
    int failures = 0;
    for (int i = 0; i < 1000; i++) {
        int n = ascolto_capture_buffer_pop(b, mic, sys, 320, false);
        if (n != 320 || memcmp(mic, sys, sizeof(mic)) || memcmp(mic, source, sizeof(mic))) failures++;
    }
    if (ascolto_capture_buffer_dropped(b)) failures++;
    ascolto_capture_buffer_reset(b);
    /* SYS starts 200ms before MIC; timestamps preserve the startup offset. */
    ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_SYS, source, 3200, 0);
    ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_MIC, source, 3200, 3200);
    if (ascolto_capture_buffer_pop(b, mic, sys, 320, false) != 320 || mic[0] != 0 || sys[0] != 1) failures++;
    ascolto_capture_buffer_reset(b);
    /* Missing SYS cannot stall MIC; a late buffer must not shift subsequent audio. */
    ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_MIC, source, 320, 0);
    if (ascolto_capture_buffer_pop(b, mic, sys, 320, true) != 320 || sys[0] != 0) failures++;
    ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_SYS, source, 640, 0);
    ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_MIC, source, 320, 320);
    if (ascolto_capture_buffer_pop(b, mic, sys, 320, false) != 320 || memcmp(mic, sys, sizeof(mic))) failures++;
    ascolto_capture_buffer_reset(b);
    for (int i = 0; i < 160; i++) {
        ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_SYS, source, 3200, i * 3200);
        ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_MIC, source, 3200, i * 3200);
    }
    if (ascolto_capture_buffer_dropped(b) != 32000) failures++;
    int64_t delivered = 0, silent = 0;
    int n;
    while ((n = ascolto_capture_buffer_pop(b, mic, sys, 320, false)) > 0) {
        delivered += n;
        if (memcmp(mic, sys, (size_t)n * sizeof(float))) failures++;
        if (mic[0] == 0) silent += n;
    }
    if (delivered != 512000 || silent != 32000) failures++;
    ascolto_capture_buffer_free(b);
    /* New overflows can arrive while a previous gap is being consumed. Encode
       absolute sample positions to detect shortened time or double cursor jumps. */
    b = ascolto_capture_buffer_create(6400);
    if (!b) return failures + 1;
    delivered = silent = 0;
    for (int chunk = 0; chunk < 40; chunk++) {
        for (int i = 0; i < 3200; i++) source[i] = (float)(chunk * 3200 + i + 1);
        ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_SYS, source, 3200, chunk * 3200);
        ascolto_capture_buffer_push(b, ASCOLTO_CHANNEL_MIC, source, 3200, chunk * 3200);
        if (chunk % 5 == 4) {
            for (int frame = 0; frame < 10; frame++) {
                n = ascolto_capture_buffer_pop(b, mic, sys, 320, false);
                if (n != 320) failures++;
                for (int i = 0; i < n; i++) {
                    if (mic[i] != sys[i] || (mic[i] != 0 && mic[i] != (float)(delivered + i + 1))) failures++;
                    if (mic[i] == 0) silent++;
                }
                delivered += n;
            }
        }
    }
    while ((n = ascolto_capture_buffer_pop(b, mic, sys, 320, false)) > 0) {
        for (int i = 0; i < n; i++) {
            if (mic[i] != sys[i] || (mic[i] != 0 && mic[i] != (float)(delivered + i + 1))) failures++;
            if (mic[i] == 0) silent++;
        }
        delivered += n;
    }
    if (delivered != 128000 || silent != ascolto_capture_buffer_dropped(b)) failures++;
    ascolto_capture_buffer_free(b);
    return failures;
}
