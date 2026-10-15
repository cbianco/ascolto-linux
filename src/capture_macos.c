#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#include <AudioToolbox/AudioQueue.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdatomic.h>
#include <CoreAudio/HostTime.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ascolto.h"

#define NUM_AQ_BUFFERS 3
#define AQ_BUFFER_SAMPLES 3200 /* 200 ms @ 16 kHz */
#define CAP_RING_CAPACITY (ASCOLTO_SAMPLE_RATE * 30) /* 30 seconds ring buffer per channel */

/* Record the exact HAL operation that stalls, without holding a log/file lock. */
#define TRACE_HAL(status, operation) do { \
    CFAbsoluteTime hal_started = CFAbsoluteTimeGetCurrent(); \
    ascolto_diagnostic("HAL begin %s", #operation); \
    (status) = (operation); \
    ascolto_diagnostic("HAL end %s status=%d duration_ms=%.0f", #operation, (int)(status), \
                      (CFAbsoluteTimeGetCurrent() - hal_started) * 1000.0); \
} while (0)

typedef struct ascolto_macos_capture_s ascolto_macos_capture_t;

typedef struct {
    ascolto_live_app_t* app;
    ascolto_macos_capture_t* cap;
    ascolto_channel_t channel;
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[NUM_AQ_BUFFERS];
    _Atomic bool running;
} aq_stream_t;

/* Pure CoreAudio System Output Tap (NO ScreenCaptureKit, NO Screen Sharing indicator!) */
typedef struct {
    ascolto_live_app_t* app;
    ascolto_macos_capture_t* cap;
    AudioObjectID tap_id;
    AudioDeviceID agg_device_id;
    AudioDeviceIOProcID io_proc_id;
    double tap_sample_rate;
    double resample_phase;
    uint32_t tap_channels;
    _Atomic bool running;
} ca_sys_tap_t;

struct ascolto_macos_capture_s {
    ascolto_live_app_t* app;
    aq_stream_t mic_stream;
    aq_stream_t sys_aq_stream;
    ca_sys_tap_t sys_tap;

    pthread_mutex_t stream_lock;
    uint64_t suspended_generation;
    pthread_mutex_t ring_lock;
    pthread_cond_t ring_cond;
    ascolto_capture_buffer_t* buffer;
    uint64_t capture_epoch_ns;
    bool producers_stopped;
    bool session_draining;
    bool worker_busy;

    pthread_t monitor_thread;
    pthread_t worker_thread;
    _Atomic bool stop_requested;
    bool mic_permission_requested;
};

static int64_t timestamp_sample(ascolto_macos_capture_t* cap, const AudioTimeStamp* timestamp) {
    if (!timestamp || !(timestamp->mFlags & kAudioTimeStampHostTimeValid)) return -1;
    uint64_t ns = AudioConvertHostTimeToNanos(timestamp->mHostTime);
    double delta = ns >= cap->capture_epoch_ns ? (double)(ns - cap->capture_epoch_ns)
                                              : -(double)(cap->capture_epoch_ns - ns);
    int64_t sample = (int64_t)llround(delta * ASCOLTO_SAMPLE_RATE / 1e9);
    return sample >= 0 ? sample : 0;
}

static void ring_push_samples(ascolto_macos_capture_t* cap, ascolto_channel_t channel,
                              const float* samples, int num_samples, int64_t start_sample) {
    if (!cap || !samples || num_samples <= 0) return;
    pthread_mutex_lock(&cap->ring_lock);
    ascolto_capture_buffer_push(cap->buffer, channel, samples, num_samples, start_sample);
    pthread_cond_signal(&cap->ring_cond);
    pthread_mutex_unlock(&cap->ring_lock);
}

static void ring_reset(ascolto_macos_capture_t* cap) {
    pthread_mutex_lock(&cap->ring_lock);
    ascolto_capture_buffer_reset(cap->buffer);
    cap->capture_epoch_ns = AudioConvertHostTimeToNanos(AudioGetCurrentHostTime());
    pthread_mutex_unlock(&cap->ring_lock);
}

static OSStatus sys_tap_io_proc(
    AudioObjectID inDevice,
    const AudioTimeStamp* inNow,
    const AudioBufferList* inInputData,
    const AudioTimeStamp* inInputTime,
    AudioBufferList* outOutputData,
    const AudioTimeStamp* inOutputTime,
    void* inClientData
) {
    (void)inDevice;
    (void)inNow;
    (void)outOutputData;
    (void)inOutputTime;

    ca_sys_tap_t* tap = (ca_sys_tap_t*)inClientData;
    if (!tap || !tap->running || !tap->app || !inInputData || inInputData->mNumberBuffers == 0) {
        return noErr;
    }

    const AudioBuffer* buf = &inInputData->mBuffers[0];
    if (!buf->mData || buf->mDataByteSize == 0) return noErr;

    uint32_t n_ch = (buf->mNumberChannels > 0) ? buf->mNumberChannels : 1;
    int total_floats = (int)(buf->mDataByteSize / sizeof(float));
    int in_frames = total_floats / (int)n_ch;
    if (in_frames <= 0) return noErr;

    const float* src = (const float*)buf->mData;
    double sr = (tap->tap_sample_rate > 8000.0) ? tap->tap_sample_rate : 48000.0;
    double ratio = sr / (double)ASCOLTO_SAMPLE_RATE;

    float out_16k[2048];
    int out_frames = 0;
    double pos = tap->resample_phase;

    int64_t start_sample = timestamp_sample(tap->cap, inInputTime);
    if (start_sample >= 0) start_sample += (int64_t)llround(pos / ratio);
    while (pos < (double)in_frames) {
        int s0 = (int)pos;
        int s1 = (int)(pos + ratio);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > in_frames) s1 = in_frames;
        double acc = 0.0;
        int cnt = 0;
        for (int s = s0; s < s1; s++) {
            for (uint32_t c = 0; c < n_ch; c++) {
                acc += src[s * (int)n_ch + (int)c];
                cnt++;
            }
        }
        out_16k[out_frames++] = (cnt > 0) ? (float)(acc / (double)cnt) : 0.0f;
        pos += ratio;
        if (out_frames == 2048) {
            ring_push_samples(tap->cap, ASCOLTO_CHANNEL_SYS, out_16k, out_frames, start_sample);
            if (start_sample >= 0) start_sample += out_frames;
            out_frames = 0;
        }
    }
    tap->resample_phase = pos - (double)in_frames;
    if (tap->resample_phase < 0.0) tap->resample_phase = 0.0;

    if (out_frames > 0) {
        ring_push_samples(tap->cap, ASCOLTO_CHANNEL_SYS, out_16k, out_frames, start_sample);
    }

    return noErr;
}

static int start_ca_sys_tap(ca_sys_tap_t* tap, ascolto_live_app_t* app, ascolto_macos_capture_t* cap) {
    if (!tap || tap->running) return 0;
    @autoreleasepool {
        memset(tap, 0, sizeof(*tap));
        tap->app = app;
        tap->cap = cap;
        tap->tap_id = kAudioObjectUnknown;
        tap->agg_device_id = kAudioObjectUnknown;
        tap->resample_phase = 0.0;

        /* Get Default Output Device UID so the aggregate clock locks onto the active speaker/headphone */
        AudioDeviceID def_out = kAudioObjectUnknown;
        UInt32 sz = sizeof(def_out);
        AudioObjectPropertyAddress out_addr = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &out_addr, 0, NULL, &sz, &def_out);

        CFStringRef out_uid_ref = NULL;
        sz = sizeof(out_uid_ref);
        AudioObjectPropertyAddress uid_addr = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(def_out, &uid_addr, 0, NULL, &sz, &out_uid_ref);
        NSString* out_uid = (__bridge_transfer NSString*)out_uid_ref;

        /* Create CoreAudio Global Mono Tap (excludes nothing, captures all system output audio) */
        CATapDescription* desc = [[CATapDescription alloc] initMonoGlobalTapButExcludeProcesses:@[]];
        desc.UUID = [NSUUID UUID];
        desc.name = @"Ascolto System Call Audio Tap";
        desc.privateTap = YES;
        desc.muteBehavior = CATapUnmuted;

        OSStatus st;
        TRACE_HAL(st, AudioHardwareCreateProcessTap(desc, &tap->tap_id));
        if (st != noErr || tap->tap_id == kAudioObjectUnknown) {
            return -1;
        }

        AudioStreamBasicDescription tap_fmt = {0};
        sz = sizeof(tap_fmt);
        AudioObjectPropertyAddress fmt_addr = {
            kAudioTapPropertyFormat,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(tap->tap_id, &fmt_addr, 0, NULL, &sz, &tap_fmt) == noErr) {
            tap->tap_sample_rate = tap_fmt.mSampleRate;
            tap->tap_channels = tap_fmt.mChannelsPerFrame;
        } else {
            tap->tap_sample_rate = 48000.0;
            tap->tap_channels = 1;
        }

        NSString* agg_uid = [NSString stringWithFormat:@"com.cristianbianco.ascolto.tap.%@", [[NSUUID UUID] UUIDString]];
        NSMutableDictionary* agg_dict = [NSMutableDictionary dictionaryWithDictionary:@{
            @kAudioAggregateDeviceNameKey: @"Ascolto System Audio Tap",
            @kAudioAggregateDeviceUIDKey: agg_uid,
            @kAudioAggregateDeviceIsPrivateKey: @YES,
            @kAudioAggregateDeviceIsStackedKey: @NO,
            @kAudioAggregateDeviceTapAutoStartKey: @YES,
            @kAudioAggregateDeviceTapListKey: @[
                @{
                    @kAudioSubTapUIDKey: [desc.UUID UUIDString],
                    @kAudioSubTapDriftCompensationKey: @YES
                }
            ]
        }];
        if (out_uid) {
            agg_dict[@kAudioAggregateDeviceMainSubDeviceKey] = out_uid;
            agg_dict[@kAudioAggregateDeviceSubDeviceListKey] = @[
                @{ @kAudioSubDeviceUIDKey: out_uid }
            ];
        }

        TRACE_HAL(st, AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)agg_dict, &tap->agg_device_id));
        if (st != noErr || tap->agg_device_id == kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(tap->tap_id);
            tap->tap_id = kAudioObjectUnknown;
            return -2;
        }

        TRACE_HAL(st, AudioDeviceCreateIOProcID(tap->agg_device_id, sys_tap_io_proc, tap, &tap->io_proc_id));
        if (st != noErr) {
            AudioHardwareDestroyAggregateDevice(tap->agg_device_id);
            AudioHardwareDestroyProcessTap(tap->tap_id);
            tap->agg_device_id = kAudioObjectUnknown;
            tap->tap_id = kAudioObjectUnknown;
            return -3;
        }

        tap->running = true;
        TRACE_HAL(st, AudioDeviceStart(tap->agg_device_id, tap->io_proc_id));
        if (st != noErr) {
            tap->running = false;
            AudioDeviceDestroyIOProcID(tap->agg_device_id, tap->io_proc_id);
            AudioHardwareDestroyAggregateDevice(tap->agg_device_id);
            AudioHardwareDestroyProcessTap(tap->tap_id);
            tap->io_proc_id = NULL;
            tap->agg_device_id = kAudioObjectUnknown;
            tap->tap_id = kAudioObjectUnknown;
            return -4;
        }

        pthread_mutex_lock(&app->lock);
        app->sys_audio_permission_ok = true;
        pthread_mutex_unlock(&app->lock);
        return 0;
    }
}

static void stop_ca_sys_tap(ca_sys_tap_t* tap) {
    if (!tap || !tap->running) return;
    tap->running = false;
    OSStatus status;
    if (tap->agg_device_id != kAudioObjectUnknown && tap->io_proc_id != NULL) {
        TRACE_HAL(status, AudioDeviceStop(tap->agg_device_id, tap->io_proc_id));
        TRACE_HAL(status, AudioDeviceDestroyIOProcID(tap->agg_device_id, tap->io_proc_id));
        tap->io_proc_id = NULL;
    }
    if (tap->agg_device_id != kAudioObjectUnknown) {
        TRACE_HAL(status, AudioHardwareDestroyAggregateDevice(tap->agg_device_id));
        tap->agg_device_id = kAudioObjectUnknown;
    }
    if (tap->tap_id != kAudioObjectUnknown) {
        TRACE_HAL(status, AudioHardwareDestroyProcessTap(tap->tap_id));
        tap->tap_id = kAudioObjectUnknown;
    }
}

static void aq_input_callback(
    void* inUserData,
    AudioQueueRef inAQ,
    AudioQueueBufferRef inBuffer,
    const AudioTimeStamp* inStartTime,
    UInt32 inNumberPacketDescriptions,
    const AudioStreamPacketDescription* inPacketDescs
) {
    (void)inNumberPacketDescriptions;
    (void)inPacketDescs;

    aq_stream_t* st = (aq_stream_t*)inUserData;
    if (!st || !st->running || !st->app || !st->cap) return;

    int num_samples = (int)(inBuffer->mAudioDataByteSize / sizeof(float));
    const float* pcm = (const float*)inBuffer->mAudioData;
    if (num_samples > 0 && pcm) {
        ring_push_samples(st->cap, st->channel, pcm, num_samples, timestamp_sample(st->cap, inStartTime));
    }

    if (st->running) {
        AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
    }
}

static CFStringRef copy_device_uid(AudioDeviceID dev_id) {
    if (dev_id == 0 || dev_id == kAudioObjectUnknown) return NULL;
    CFStringRef uid = NULL;
    UInt32 sz = sizeof(uid);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &sz, &uid) == noErr) {
        return uid;
    }
    return NULL;
}

static int start_aq_stream(
    aq_stream_t* st,
    ascolto_live_app_t* app,
    ascolto_macos_capture_t* cap,
    ascolto_channel_t ch,
    AudioDeviceID dev_id
) {
    if (st->running) return 0;
    memset(st, 0, sizeof(*st));
    st->app = app;
    st->cap = cap;
    st->channel = ch;

    /* Deterministic regression hook for a slow hardware startup; opt-in tests only. */
    const char* delay_env = getenv("ASCOLTO_CAPTURE_TEST_START_DELAY_MS");
    int delay_ms = delay_env ? atoi(delay_env) : 0;
    if (ch == ASCOLTO_CHANNEL_MIC && delay_ms > 0 && delay_ms <= 10000) {
        usleep((useconds_t)delay_ms * 1000);
    }

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = (Float64)ASCOLTO_SAMPLE_RATE;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mBytesPerFrame = sizeof(float);

    OSStatus status;
    TRACE_HAL(status, AudioQueueNewInput(&fmt, aq_input_callback, st, NULL, kCFRunLoopCommonModes, 0, &st->queue));
    if (status != noErr || !st->queue) {
        return -1;
    }

    if (dev_id != 0 && dev_id != kAudioObjectUnknown) {
        CFStringRef uid = copy_device_uid(dev_id);
        if (uid) {
            AudioQueueSetProperty(st->queue, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
            CFRelease(uid);
        }
    }

    UInt32 buf_bytes = AQ_BUFFER_SAMPLES * sizeof(float);
    for (int i = 0; i < NUM_AQ_BUFFERS; i++) {
        AudioQueueAllocateBuffer(st->queue, buf_bytes, &st->buffers[i]);
        AudioQueueEnqueueBuffer(st->queue, st->buffers[i], 0, NULL);
    }

    st->running = true;
    TRACE_HAL(status, AudioQueueStart(st->queue, NULL));
    if (status != noErr) {
        st->running = false;
        AudioQueueDispose(st->queue, true);
        st->queue = NULL;
        return -2;
    }
    return 0;
}

static void stop_aq_stream(aq_stream_t* st) {
    if (!st || !st->running || !st->queue) return;
    st->running = false;
    OSStatus status;
    TRACE_HAL(status, AudioQueueStop(st->queue, true));
    TRACE_HAL(status, AudioQueueDispose(st->queue, true));
    st->queue = NULL;
}

static void* capture_worker_thread_fn(void* arg) {
    @autoreleasepool {
        ascolto_macos_capture_t* cap = arg;
        float sys_chunk[320], mic_chunk[320];
        double missing_since = 0;
        for (;;) {
            pthread_mutex_lock(&cap->ring_lock);
            int n = 0;
            for (;;) {
                int m = ascolto_capture_buffer_available(cap->buffer, ASCOLTO_CHANNEL_MIC);
                int s = ascolto_capture_buffer_available(cap->buffer, ASCOLTO_CHANNEL_SYS);
                if (cap->producers_stopped && m == 0 && s == 0) {
                    pthread_mutex_unlock(&cap->ring_lock);
                    return NULL;
                }
                struct timespec monotonic;
                clock_gettime(CLOCK_MONOTONIC, &monotonic);
                double now = monotonic.tv_sec + monotonic.tv_nsec / 1e9;
                bool have_pair = m >= 320 && s >= 320;
                if (have_pair || (m == 0 && s == 0)) missing_since = 0;
                else if (missing_since == 0) missing_since = now;
                bool pad = cap->producers_stopped || cap->session_draining || (missing_since > 0 && now - missing_since >= 0.30);
                n = ascolto_capture_buffer_pop(cap->buffer, mic_chunk, sys_chunk, 320, pad);
                if (n > 0) break;
                struct timespec timeout;
                clock_gettime(CLOCK_REALTIME, &timeout);
                timeout.tv_nsec += 50000000;
                if (timeout.tv_nsec >= 1000000000) { timeout.tv_sec++; timeout.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&cap->ring_cond, &cap->ring_lock, &timeout);
            }
            cap->worker_busy = true;
            int64_t dropped = ascolto_capture_buffer_dropped(cap->buffer);
            int64_t padded_mic = ascolto_capture_buffer_padded(cap->buffer, ASCOLTO_CHANNEL_MIC);
            int64_t padded_sys = ascolto_capture_buffer_padded(cap->buffer, ASCOLTO_CHANNEL_SYS);
            pthread_mutex_unlock(&cap->ring_lock);
            pthread_mutex_lock(&cap->app->lock);
            cap->app->capture_dropped_samples = dropped;
            cap->app->capture_padded_samples[0] = padded_mic;
            cap->app->capture_padded_samples[1] = padded_sys;
            pthread_mutex_unlock(&cap->app->lock);
            /* Feed matching 20ms intervals, SYS first, outside the callback queue lock. */
            ascolto_live_app_feed_audio(cap->app, ASCOLTO_CHANNEL_SYS, sys_chunk, n);
            ascolto_live_app_feed_audio(cap->app, ASCOLTO_CHANNEL_MIC, mic_chunk, n);
            pthread_mutex_lock(&cap->ring_lock);
            cap->worker_busy = false;
            pthread_cond_broadcast(&cap->ring_cond);
            pthread_mutex_unlock(&cap->ring_lock);
        }
    }
}

static void* capture_monitor_thread_fn(void* arg) {
    @autoreleasepool {
        ascolto_macos_capture_t* cap = (ascolto_macos_capture_t*)arg;
        ascolto_live_app_t* app = cap->app;

        while (!cap->stop_requested) {
            pthread_mutex_lock(&app->lock);
            uint32_t mic_dev = app->config.mic_device_id;
            pthread_mutex_unlock(&app->lock);
            ascolto_macos_device_info_t info;
            const char* dev_name = "Microfono di sistema";
            if (ascolto_macos_device_status(mic_dev, &info) == 0) {
                dev_name = info.device_name;
            }

            char in_apps[256] = {0};
            char out_apps[256] = {0};
            bool ext_mic_active = ascolto_macos_query_audio_apps(
                in_apps, sizeof(in_apps),
                out_apps, sizeof(out_apps)
            );

            ascolto_call_metadata_t meta;
            ascolto_macos_query_call_metadata(in_apps[0] ? in_apps : out_apps, &meta);

            pthread_mutex_lock(&app->lock);
            app->call_meta = meta;
            if (in_apps[0]) {
                snprintf(app->active_in_apps, sizeof(app->active_in_apps), "%s", in_apps);
                snprintf(app->active_call_app, sizeof(app->active_call_app), "%s", in_apps);
            } else {
                snprintf(app->active_in_apps, sizeof(app->active_in_apps), "Nessuna");
                snprintf(app->active_call_app, sizeof(app->active_call_app), "Nessuna call rilevata");
            }
            if (out_apps[0]) {
                snprintf(app->active_out_apps, sizeof(app->active_out_apps), "%s", out_apps);
            } else if (!ext_mic_active) {
                snprintf(app->active_out_apps, sizeof(app->active_out_apps), "Nessuna");
            }
            pthread_mutex_unlock(&app->lock);

            ascolto_live_app_on_mic_hardware_change(app, ext_mic_active, dev_name);

            pthread_mutex_lock(&app->lock);
            ascolto_app_state_t st = app->state;
            uint32_t cfg_mic = app->config.mic_device_id;
            uint32_t cfg_sys = app->config.sys_device_id;
            uint64_t generation = app->session_generation;
            pthread_mutex_unlock(&app->lock);

            pthread_mutex_lock(&cap->stream_lock);
            if (st == ASCOLTO_STATE_RECORDING && generation != cap->suspended_generation && !cap->stop_requested) {
                if (!cap->mic_stream.running && !cap->sys_tap.running && !cap->sys_aq_stream.running) ring_reset(cap);
                /* Request microphone access before starting CoreAudio. An unresolved
                   permission must not leave AudioQueueStart waiting for the UI thread. */
                AVAuthorizationStatus authorization = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
                if (authorization == AVAuthorizationStatusNotDetermined && !cap->mic_permission_requested) {
                    cap->mic_permission_requested = true;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
                            fprintf(stderr, "[capture] Microphone permission: %s\n", granted ? "granted" : "denied");
                        }];
                    });
                }
                /* Both producers share host timestamps; MIC can start before the SYS tap. */
                if (authorization == AVAuthorizationStatusAuthorized && !cap->mic_stream.running) {
                    fprintf(stderr, "[capture] Starting microphone\n");
                    int rc = start_aq_stream(&cap->mic_stream, app, cap, ASCOLTO_CHANNEL_MIC, (AudioDeviceID)cfg_mic);
                    fprintf(stderr, "[capture] Microphone start result: %d\n", rc);
                }
                if (cfg_sys == 0) {
                    if (!cap->sys_tap.running) {
                        start_ca_sys_tap(&cap->sys_tap, app, cap);
                    }
                } else {
                    if (!cap->sys_aq_stream.running) {
                        start_aq_stream(&cap->sys_aq_stream, app, cap, ASCOLTO_CHANNEL_SYS, (AudioDeviceID)cfg_sys);
                    }
                }
            } else if (st != ASCOLTO_STATE_RECORDING && st != ASCOLTO_STATE_STOPPING) {
                bool was_running = (cap->mic_stream.running || cap->sys_aq_stream.running || cap->sys_tap.running);
                if (cap->mic_stream.running) stop_aq_stream(&cap->mic_stream);
                if (cap->sys_aq_stream.running) stop_aq_stream(&cap->sys_aq_stream);
                if (cap->sys_tap.running) stop_ca_sys_tap(&cap->sys_tap);
                if (was_running) ring_reset(cap);
            }

            pthread_mutex_unlock(&cap->stream_lock);
            usleep(250000);
        }

        pthread_mutex_lock(&cap->stream_lock);
        stop_aq_stream(&cap->mic_stream);
        stop_aq_stream(&cap->sys_aq_stream);
        stop_ca_sys_tap(&cap->sys_tap);
        pthread_mutex_unlock(&cap->stream_lock);
    }
    return NULL;
}

ascolto_macos_capture_t* ascolto_macos_capture_start(ascolto_live_app_t* app) {
    if (!app) return NULL;
    ascolto_macos_capture_t* cap = (ascolto_macos_capture_t*)calloc(1, sizeof(ascolto_macos_capture_t));
    if (!cap) return NULL;
    cap->app = app;
    cap->stop_requested = false;
    pthread_mutex_init(&cap->stream_lock, NULL);
    pthread_mutex_init(&cap->ring_lock, NULL);
    pthread_cond_init(&cap->ring_cond, NULL);
    cap->buffer = ascolto_capture_buffer_create(CAP_RING_CAPACITY);
    if (!cap->buffer) {
        ascolto_capture_buffer_free(cap->buffer);
        pthread_cond_destroy(&cap->ring_cond);
        pthread_mutex_destroy(&cap->ring_lock);
        pthread_mutex_destroy(&cap->stream_lock);
        free(cap);
        return NULL;
    }

    if (pthread_create(&cap->worker_thread, NULL, capture_worker_thread_fn, cap) != 0) {
        ascolto_capture_buffer_free(cap->buffer);
        pthread_cond_destroy(&cap->ring_cond);
        pthread_mutex_destroy(&cap->ring_lock);
        pthread_mutex_destroy(&cap->stream_lock);
        free(cap);
        return NULL;
    }
    if (pthread_create(&cap->monitor_thread, NULL, capture_monitor_thread_fn, cap) != 0) {
        cap->stop_requested = true;
        pthread_mutex_lock(&cap->ring_lock);
        cap->producers_stopped = true;
        pthread_cond_broadcast(&cap->ring_cond);
        pthread_mutex_unlock(&cap->ring_lock);
        pthread_join(cap->worker_thread, NULL);
        ascolto_capture_buffer_free(cap->buffer);
        pthread_cond_destroy(&cap->ring_cond);
        pthread_mutex_destroy(&cap->ring_lock);
        pthread_mutex_destroy(&cap->stream_lock);
        free(cap);
        return NULL;
    }
    pthread_mutex_lock(&app->lock);
    app->capture = cap;
    pthread_mutex_unlock(&app->lock);
    return cap;
}

void ascolto_macos_capture_stop(ascolto_macos_capture_t* cap) {
    if (!cap) return;
    cap->stop_requested = true;
    pthread_join(cap->monitor_thread, NULL);
    pthread_mutex_lock(&cap->ring_lock);
    cap->producers_stopped = true;
    pthread_cond_broadcast(&cap->ring_cond);
    pthread_mutex_unlock(&cap->ring_lock);
    pthread_join(cap->worker_thread, NULL);
    ascolto_capture_buffer_free(cap->buffer);
    pthread_cond_destroy(&cap->ring_cond);
    pthread_mutex_destroy(&cap->ring_lock);
    pthread_mutex_destroy(&cap->stream_lock);
    pthread_mutex_lock(&cap->app->lock);
    cap->app->capture = NULL;
    pthread_mutex_unlock(&cap->app->lock);
    free(cap);
}

void ascolto_macos_capture_finish_session(ascolto_macos_capture_t* cap, uint64_t generation) {
    if (!cap) return;
    /* Stop producers, then let ASR finish the queued tail before files are closed. */
    CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
    ascolto_diagnostic("Capture drain begin generation=%llu; waiting for stream lock", (unsigned long long)generation);
    pthread_mutex_lock(&cap->stream_lock);
    ascolto_diagnostic("Capture drain acquired stream lock duration_ms=%.0f", (CFAbsoluteTimeGetCurrent() - started) * 1000.0);
    pthread_mutex_lock(&cap->app->lock);
    bool current_stop = cap->app->session_generation == generation && cap->app->state == ASCOLTO_STATE_STOPPING;
    pthread_mutex_unlock(&cap->app->lock);
    if (!current_stop) {
        pthread_mutex_unlock(&cap->stream_lock);
        return;
    }
    cap->suspended_generation = generation;
    stop_aq_stream(&cap->mic_stream);
    stop_aq_stream(&cap->sys_aq_stream);
    stop_ca_sys_tap(&cap->sys_tap);
    pthread_mutex_unlock(&cap->stream_lock);
    pthread_mutex_lock(&cap->ring_lock);
    cap->session_draining = true;
    pthread_cond_broadcast(&cap->ring_cond);
    while (cap->worker_busy || ascolto_capture_buffer_available(cap->buffer, ASCOLTO_CHANNEL_MIC) > 0 ||
           ascolto_capture_buffer_available(cap->buffer, ASCOLTO_CHANNEL_SYS) > 0) {
        pthread_cond_wait(&cap->ring_cond, &cap->ring_lock);
    }
    cap->session_draining = false;
    pthread_mutex_unlock(&cap->ring_lock);
    ascolto_diagnostic("Capture drain end duration_ms=%.0f", (CFAbsoluteTimeGetCurrent() - started) * 1000.0);
}
