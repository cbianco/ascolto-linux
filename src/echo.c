#include "ascolto/echo.h"
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

struct ascolto_echo {
    SpeexEchoState *filter;
    SpeexPreprocessState *residual;
    int pending;
    bool tail, finished;
    float original[ASCOLTO_FRAME_SAMPLES];
    double input_energy, residual_energy;
    float pending_mix, mix;
};

ascolto_echo_t *ascolto_echo_create(void) {
    ascolto_echo_t *echo = calloc(1, sizeof(*echo));
    if (!echo) return NULL;
    /* A 256 ms acoustic tail covers speaker/device delay and room reflections.
     * The adaptive filter handles double talk; no energy or text gate is used. */
    echo->filter = speex_echo_state_init(ASCOLTO_FRAME_SAMPLES, 4096);
    echo->residual = speex_preprocess_state_init(ASCOLTO_FRAME_SAMPLES, ASCOLTO_SAMPLE_RATE);
    if (!echo->filter || !echo->residual) { ascolto_echo_destroy(echo); return NULL; }
    int rate = ASCOLTO_SAMPLE_RATE, zero = 0, suppress = -40, active = -12;
    if (speex_echo_ctl(echo->filter, SPEEX_ECHO_SET_SAMPLING_RATE, &rate) ||
        speex_preprocess_ctl(echo->residual, SPEEX_PREPROCESS_SET_ECHO_STATE, echo->filter) ||
        speex_preprocess_ctl(echo->residual, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &zero) ||
        speex_preprocess_ctl(echo->residual, SPEEX_PREPROCESS_SET_AGC, &zero) ||
        speex_preprocess_ctl(echo->residual, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &suppress) ||
        speex_preprocess_ctl(echo->residual, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &active)) {
        ascolto_echo_destroy(echo);
        return NULL;
    }
    return echo;
}

void ascolto_echo_destroy(ascolto_echo_t *echo) {
    if (!echo) return;
    if (echo->residual) speex_preprocess_state_destroy(echo->residual);
    if (echo->filter) speex_echo_state_destroy(echo->filter);
    free(echo);
}

static spx_int16_t pcm16(float sample) {
    return (spx_int16_t)lrintf(fmaxf(-32768, fminf(32767, sample * 32768)));
}

int ascolto_echo_process(ascolto_echo_t *echo, const float *mic, const float *sys,
                        int count, float output[ASCOLTO_FRAME_SAMPLES]) {
    if (!echo || !mic || !sys || !output || echo->tail || echo->finished ||
        count <= 0 || count > ASCOLTO_FRAME_SAMPLES) return -1;
    spx_int16_t near[ASCOLTO_FRAME_SAMPLES] = {0}, far[ASCOLTO_FRAME_SAMPLES] = {0};
    spx_int16_t clean[ASCOLTO_FRAME_SAMPLES];
    for (int i = 0; i < count; i++) {
        if (!isfinite(mic[i]) || !isfinite(sys[i])) return -1;
        near[i] = pcm16(mic[i]);
        far[i] = pcm16(sys[i]);
    }
    speex_echo_cancellation(echo->filter, near, far, clean);
    double input_energy = 0, residual_energy = 0;
    for (int i = 0; i < count; i++) {
        input_energy += (double)near[i] * near[i];
        residual_energy += (double)clean[i] * clean[i];
    }
    echo->input_energy = .8 * echo->input_energy + .2 * input_energy;
    echo->residual_energy = .8 * echo->residual_energy + .2 * residual_energy;
    /* An uncorrelated headphone reference must not add adaptive-filter noise
     * to a quiet local voice. Use the processed signal only while the linear
     * canceller demonstrates at least 3 dB of removal over roughly 100 ms.
     * This selects dry vs cancelled PCM; it never gates speech or emits silence.
     * Speex still adapts continuously, including during the dry fallback. */
    float next_mix = echo->input_energy > 0 &&
        echo->residual_energy < .5 * echo->input_energy ? 1 : 0;
    /* Keep denoise processing enabled for residual echo, but with zero noise
     * suppression and no AGC/VAD. Ignore the speech-probability return value. */
    speex_preprocess_run(echo->residual, clean);
    int produced = echo->pending;
    float previous_mix = echo->mix;
    for (int i = 0; i < produced; i++) {
        float mix = previous_mix + (echo->pending_mix - previous_mix) * (i + 1) / produced;
        output[i] = echo->original[i] + mix * ((float)clean[i] / 32768 - echo->original[i]);
    }
    echo->mix = echo->pending_mix;
    echo->pending_mix = next_mix;
    for (int i = 0; i < count; i++) echo->original[i] = mic[i];
    echo->pending = count;
    echo->tail = count < ASCOLTO_FRAME_SAMPLES;
    return produced;
}

int ascolto_echo_finish(ascolto_echo_t *echo, float output[ASCOLTO_FRAME_SAMPLES]) {
    if (!echo || !output) return -1;
    if (echo->finished) return 0;
    float silence[ASCOLTO_FRAME_SAMPLES] = {0};
    echo->tail = false;
    int count = ascolto_echo_process(echo, silence, silence, ASCOLTO_FRAME_SAMPLES, output);
    echo->finished = true;
    echo->pending = 0;
    return count;
}
