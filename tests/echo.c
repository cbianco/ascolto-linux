#include "ascolto/echo.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Echo check failed at %d: %s\n", __LINE__, #x); exit(1); } checks++; } while (0)

static void process(const float *mic, const float *sys, int count, float *output) {
    ascolto_echo_t *echo = ascolto_echo_create();
    CHECK(echo);
    int written = 0;
    for (int position = 0; position < count; position += ASCOLTO_FRAME_SAMPLES) {
        int n = count - position;
        if (n > ASCOLTO_FRAME_SAMPLES) n = ASCOLTO_FRAME_SAMPLES;
        float frame[ASCOLTO_FRAME_SAMPLES];
        int ready = ascolto_echo_process(echo, mic + position, sys + position, n, frame);
        CHECK(ready >= 0 && written + ready <= count);
        memcpy(output + written, frame, (size_t)ready * sizeof(float));
        written += ready;
    }
    float tail[ASCOLTO_FRAME_SAMPLES];
    int ready = ascolto_echo_finish(echo, tail);
    CHECK(ready >= 0 && written + ready == count);
    memcpy(output + written, tail, (size_t)ready * sizeof(float));
    CHECK(ascolto_echo_finish(echo, tail) == 0);
    CHECK(ascolto_echo_process(echo, mic, sys, 1, tail) < 0);
    ascolto_echo_destroy(echo);
}

static double energy(const float *pcm, int start, int end) {
    double value = 0;
    for (int i = start; i < end; i++) value += (double)pcm[i] * pcm[i];
    return value;
}

int main(void) {
    const int rate = ASCOLTO_SAMPLE_RATE, count = rate * 8 + 13;
    float *sys = calloc((size_t)count, sizeof(float));
    float *mic = calloc((size_t)count, sizeof(float));
    float *local = calloc((size_t)count, sizeof(float));
    float *clean = calloc((size_t)count, sizeof(float));
    float *again = calloc((size_t)count, sizeof(float));
    CHECK(sys && mic && local && clean && again);
    uint32_t random = 314159;
    for (int i = 0; i < count; i++) {
        random = random * 1664525u + 1013904223u;
        sys[i] = i < rate * 5 ? ((float)(random >> 8) / 16777216 - .5f) * .3f : 0;
        /* Independent local speech, including a quiet simultaneous reply. */
        if (i >= rate * 2)
            local[i] = (i >= rate * 3 && i < rate * 4 ? .004f : .04f) * sinf(i * .31f);
        mic[i] = local[i];
        if (i >= 480) mic[i] += .6f * sys[i - 480];
        if (i >= 661) mic[i] += .2f * sys[i - 661];
    }
    memcpy(again, mic, (size_t)count * sizeof(float));
    process(mic, sys, count, clean);
    CHECK(!memcmp(again, mic, (size_t)count * sizeof(float)));
    double reduction = 10 * log10(energy(mic, rate, 2 * rate) / energy(clean, rate, 2 * rate));
    CHECK(reduction > 20);
    for (int second = 2; second < 8; second++) {
        double reference = energy(local, second * rate, (second + 1) * rate);
        double dot = 0;
        for (int i = second * rate; i < (second + 1) * rate; i++) dot += clean[i] * local[i];
        double gain = dot / reference;
        printf("Local preservation at %d s: %.3f\n", second, gain);
        CHECK(gain > .65 && gain < 1.1);
    }
    process(mic, sys, count, again);
    CHECK(!memcmp(clean, again, (size_t)count * sizeof(float)));
    /* Playback in headphones is present in SYS but absent from MIC. */
    process(local, sys, count, clean);
    double headphones_dot = 0, headphones_error = 0;
    for (int i = rate * 3; i < rate * 4; i++) {
        headphones_dot += clean[i] * local[i];
        headphones_error += (clean[i] - local[i]) * (clean[i] - local[i]);
    }
    CHECK(headphones_dot / energy(local, rate * 3, rate * 4) > .8);
    printf("Headphones relative error: %.3f\n", headphones_error / energy(local, rate * 3, rate * 4));
    CHECK(headphones_error / energy(local, rate * 3, rate * 4) < .1);
    /* Headphones, no playback and partial final frames retain quiet input and
     * the exact timeline. No reference means no cancellation target. */
    memset(sys, 0, (size_t)count * sizeof(float));
    process(local, sys, count, clean);
    CHECK(energy(clean, 3 * rate, 4 * rate) > .8 * energy(local, 3 * rate, 4 * rate));
    CHECK(energy(clean, count - 13, count) > .5 * energy(local, count - 13, count));
    int lengths[] = {1, 13, 319, 320, 321, 640, 653};
    for (size_t j = 0; j < sizeof(lengths) / sizeof(lengths[0]); j++) {
        int n = lengths[j];
        memset(mic, 0, (size_t)n * sizeof(float));
        mic[n - 1] = .25f;
        process(mic, sys, n, clean);
        CHECK(fabsf(clean[n - 1]) > .1f);
    }
    ascolto_echo_t *echo = ascolto_echo_create();
    CHECK(echo);
    mic[0] = NAN;
    CHECK(ascolto_echo_process(echo, mic, sys, 1, clean) < 0);
    CHECK(ascolto_echo_finish(echo, clean) == 0);
    ascolto_echo_destroy(echo);
    free(sys); free(mic); free(local); free(clean); free(again);
    printf("Echo: %d checks passed, %.1f dB remote reduction\n", checks, reduction);
    return 0;
}
