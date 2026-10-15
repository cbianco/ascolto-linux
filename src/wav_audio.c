#include "ascolto.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* ascolto_channel_name(ascolto_channel_t ch) {
    return (ch == ASCOLTO_CHANNEL_MIC) ? "MIC" : "SYS";
}

int ascolto_wav_load(const char* path, ascolto_audio_buffer_t* out_buf) {
    if (!path || !out_buf) return -1;
    memset(out_buf, 0, sizeof(*out_buf));

    FILE* f = fopen(path, "rb");
    if (!f) return -2;

    char riff_hdr[12];
    if (fread(riff_hdr, 1, 12, f) != 12 ||
        memcmp(riff_hdr, "RIFF", 4) != 0 ||
        memcmp(riff_hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return -3;
    }

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    float* samples = NULL;
    int num_samples = 0;

    while (!feof(f)) {
        char chunk_id[4];
        uint32_t chunk_size = 0;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, sizeof(uint32_t), 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint8_t fmt_buf[16];
            if (chunk_size < 16 || fread(fmt_buf, 1, 16, f) != 16) {
                fclose(f);
                return -4;
            }
            memcpy(&audio_format, fmt_buf + 0, 2);
            memcpy(&num_channels, fmt_buf + 2, 2);
            memcpy(&sample_rate, fmt_buf + 4, 4);
            memcpy(&bits_per_sample, fmt_buf + 14, 2);
            if (chunk_size > 16) {
                fseek(f, (long)(chunk_size - 16), SEEK_CUR);
            }
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            if (num_channels == 0 || bits_per_sample == 0) {
                fclose(f);
                return -5;
            }
            int bytes_per_sample = bits_per_sample / 8;
            int total_frames = (int)(chunk_size / (bytes_per_sample * num_channels));
            samples = (float*)calloc((size_t)total_frames, sizeof(float));
            if (!samples) {
                fclose(f);
                return -6;
            }
            if (audio_format == 1 && bits_per_sample == 16) {
                int16_t* raw = (int16_t*)malloc(chunk_size);
                if (!raw || fread(raw, 1, chunk_size, f) != chunk_size) {
                    free(raw);
                    free(samples);
                    fclose(f);
                    return -7;
                }
                for (int i = 0; i < total_frames; i++) {
                    double acc = 0.0;
                    for (int c = 0; c < num_channels; c++) {
                        acc += (double)raw[i * num_channels + c] / 32768.0;
                    }
                    samples[i] = (float)(acc / (double)num_channels);
                }
                free(raw);
            } else if (audio_format == 3 && bits_per_sample == 32) {
                float* raw = (float*)malloc(chunk_size);
                if (!raw || fread(raw, 1, chunk_size, f) != chunk_size) {
                    free(raw);
                    free(samples);
                    fclose(f);
                    return -8;
                }
                for (int i = 0; i < total_frames; i++) {
                    double acc = 0.0;
                    for (int c = 0; c < num_channels; c++) {
                        acc += (double)raw[i * num_channels + c];
                    }
                    samples[i] = (float)(acc / (double)num_channels);
                }
                free(raw);
            } else {
                free(samples);
                fclose(f);
                return -9;
            }
            num_samples = total_frames;
            break;
        } else {
            fseek(f, (long)chunk_size, SEEK_CUR);
        }
        if (chunk_size & 1) {
            fseek(f, 1, SEEK_CUR);
        }
    }

    fclose(f);
    if (!samples) return -10;

    out_buf->samples = samples;
    out_buf->num_samples = num_samples;
    out_buf->sample_rate = (int)sample_rate;
    return 0;
}

int ascolto_wav_save(const char* path, const float* samples, int num_samples, int sample_rate) {
    if (!path || !samples || num_samples < 0 || sample_rate <= 0) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -2;

    uint32_t data_size = (uint32_t)(num_samples * sizeof(int16_t));
    uint32_t riff_size = 36 + data_size;
    uint16_t audio_fmt = 1;
    uint16_t num_ch = 1;
    uint32_t sr = (uint32_t)sample_rate;
    uint32_t byte_rate = sr * sizeof(int16_t);
    uint16_t block_align = sizeof(int16_t);
    uint16_t bps = 16;
    uint32_t fmt_size = 16;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&num_ch, 2, 1, f);
    fwrite(&sr, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);

    for (int i = 0; i < num_samples; i++) {
        float v = samples[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)lrintf(v * 32767.0f);
        fwrite(&s, sizeof(int16_t), 1, f);
    }

    fclose(f);
    return 0;
}

void ascolto_audio_free(ascolto_audio_buffer_t* buf) {
    if (!buf) return;
    free(buf->samples);
    buf->samples = NULL;
    buf->num_samples = 0;
    buf->sample_rate = 0;
}
