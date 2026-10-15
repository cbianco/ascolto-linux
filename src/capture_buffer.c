#include "ascolto.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct ascolto_capture_buffer_s {
    float* samples[2];
    int capacity;
    int64_t read_sample;
    int64_t write_sample[2];
    int64_t dropped_samples;
    int64_t pending_gap_samples;
    int64_t padded_samples[2];
};

ascolto_capture_buffer_t* ascolto_capture_buffer_create(int capacity) {
    if (capacity < 320 || capacity % 320 != 0) return NULL;
    ascolto_capture_buffer_t* b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->capacity = capacity;
    b->samples[0] = calloc((size_t)capacity, sizeof(float));
    b->samples[1] = calloc((size_t)capacity, sizeof(float));
    if (!b->samples[0] || !b->samples[1]) { ascolto_capture_buffer_free(b); return NULL; }
    return b;
}

void ascolto_capture_buffer_free(ascolto_capture_buffer_t* b) {
    if (!b) return;
    free(b->samples[0]); free(b->samples[1]); free(b);
}

void ascolto_capture_buffer_reset(ascolto_capture_buffer_t* b) {
    if (!b) return;
    b->read_sample = b->write_sample[0] = b->write_sample[1] = 0;
    b->dropped_samples = 0;
    b->pending_gap_samples = 0;
    b->padded_samples[0] = b->padded_samples[1] = 0;
    memset(b->samples[0], 0, (size_t)b->capacity * sizeof(float));
    memset(b->samples[1], 0, (size_t)b->capacity * sizeof(float));
}

void ascolto_capture_buffer_push(ascolto_capture_buffer_t* b, ascolto_channel_t ch,
                                 const float* samples, int count, int64_t start) {
    if (!b || ch < 0 || ch > 1 || !samples || count <= 0) return;
    if (start < 0) start = b->write_sample[ch];
    /* Ignore sub-sample timestamp rounding at callback boundaries. */
    if (llabs(start - b->write_sample[ch]) <= 2) start = b->write_sample[ch];
    int64_t end = start + count;
    if (end > b->read_sample + b->capacity) {
        int64_t next = ((end - b->capacity + 319) / 320) * 320;
        b->dropped_samples += next - b->read_sample;
        b->pending_gap_samples += next - b->read_sample;
        b->read_sample = next; /* Advance BOTH channels together on overload. */
    }
    int64_t write = b->write_sample[ch];
    if (write < b->read_sample) write = b->read_sample;
    for (int64_t p = write; p < start; p++) {
        b->samples[ch][p % b->capacity] = 0;
        b->padded_samples[ch]++;
    }
    if (write < start) write = start;
    for (int64_t p = write; p < end; p++) {
        b->samples[ch][p % b->capacity] = samples[p - start];
    }
    if (end > b->write_sample[ch]) b->write_sample[ch] = end;
}

int ascolto_capture_buffer_available(const ascolto_capture_buffer_t* b, ascolto_channel_t ch) {
    if (!b || ch < 0 || ch > 1) return 0;
    int64_t n = b->write_sample[ch] - b->read_sample;
    n = b->pending_gap_samples + (n > 0 ? n : 0);
    return n > INT_MAX ? INT_MAX : (int)n;
}

int ascolto_capture_buffer_pop(ascolto_capture_buffer_t* b, float* mic, float* sys,
                               int count, bool pad_missing) {
    if (!b || !mic || !sys || count <= 0) return 0;
    /* Preserve discarded time before delivering retained audio. The producer has
       already advanced read_sample; consuming this gap must not advance it twice. */
    if (b->pending_gap_samples > 0) {
        if (count > b->pending_gap_samples) count = (int)b->pending_gap_samples;
        memset(mic, 0, (size_t)count * sizeof(float));
        memset(sys, 0, (size_t)count * sizeof(float));
        b->pending_gap_samples -= count;
        b->padded_samples[0] += count;
        b->padded_samples[1] += count;
        return count;
    }
    int m = ascolto_capture_buffer_available(b, ASCOLTO_CHANNEL_MIC);
    int s = ascolto_capture_buffer_available(b, ASCOLTO_CHANNEL_SYS);
    int available = pad_missing ? (m > s ? m : s) : (m < s ? m : s);
    if (!pad_missing && available < count) return 0;
    if (count > available) count = available;
    for (int i = 0; i < count; i++) {
        int64_t p = b->read_sample + i;
        mic[i] = (p < b->write_sample[0]) ? b->samples[0][p % b->capacity] : 0;
        sys[i] = (p < b->write_sample[1]) ? b->samples[1][p % b->capacity] : 0;
        if (p >= b->write_sample[0]) b->padded_samples[0]++;
        if (p >= b->write_sample[1]) b->padded_samples[1]++;
    }
    b->read_sample += count;
    return count;
}

int64_t ascolto_capture_buffer_dropped(const ascolto_capture_buffer_t* b) {
    return b ? b->dropped_samples : 0;
}

int64_t ascolto_capture_buffer_padded(const ascolto_capture_buffer_t* b, ascolto_channel_t ch) {
    return b && ch >= 0 && ch <= 1 ? b->padded_samples[ch] : 0;
}

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
