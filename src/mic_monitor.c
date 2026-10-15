#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <EventKit/EventKit.h>
#import <Foundation/Foundation.h>
#include "ascolto.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool query_device_has_streams(AudioDeviceID dev_id, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreams,
        scope,
        kAudioObjectPropertyElementMain
    };
    UInt32 data_size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(dev_id, &addr, 0, NULL, &data_size);
    return (status == noErr && data_size > 0);
}

static void fill_device_info(
    AudioDeviceID dev_id,
    AudioDeviceID def_in,
    AudioDeviceID def_out,
    ascolto_macos_device_info_t* info
) {
    memset(info, 0, sizeof(*info));
    info->device_id = (uint32_t)dev_id;
    info->is_default_input = (dev_id == def_in);
    info->is_default_output = (dev_id == def_out);
    info->is_input = query_device_has_streams(dev_id, kAudioObjectPropertyScopeInput);

    /* Device name */
    CFStringRef cf_name = NULL;
    UInt32 sz = sizeof(cf_name);
    AudioObjectPropertyAddress name_addr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(dev_id, &name_addr, 0, NULL, &sz, &cf_name) == noErr && cf_name) {
        CFStringGetCString(cf_name, info->device_name, sizeof(info->device_name), kCFStringEncodingUTF8);
        CFRelease(cf_name);
    } else {
        snprintf(info->device_name, sizeof(info->device_name), "Device-%u", (unsigned)dev_id);
    }

    /* Nominal sample rate */
    Float64 sr = 0.0;
    sz = sizeof(sr);
    AudioObjectPropertyAddress sr_addr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(dev_id, &sr_addr, 0, NULL, &sz, &sr) == noErr) {
        info->sample_rate = (double)sr;
    }

    /* Is running somewhere (key property for detecting when microphone is in use!) */
    UInt32 running = 0;
    sz = sizeof(running);
    AudioObjectPropertyAddress run_addr = {
        kAudioDevicePropertyDeviceIsRunningSomewhere,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(dev_id, &run_addr, 0, NULL, &sz, &running) == noErr) {
        info->is_running_somewhere = (running != 0);
    }
}

int ascolto_macos_list_devices(ascolto_macos_device_info_t* out_devices, int max_devices) {
    if (!out_devices || max_devices <= 0) return -1;

    /* The capture smoke test makes a slow inventory independent of startup. */
    const char* delay_env = getenv("ASCOLTO_DEVICE_TEST_DELAY_MS");
    int delay_ms = delay_env ? atoi(delay_env) : 0;
    if (delay_ms > 0 && delay_ms <= 10000) {
        if ([NSThread isMainThread]) fprintf(stderr, "[device-test] FAIL: inventory queried on main thread\n");
        usleep((useconds_t)delay_ms * 1000);
    }

    AudioDeviceID def_in = kAudioObjectUnknown;
    AudioDeviceID def_out = kAudioObjectUnknown;
    UInt32 sz = sizeof(AudioDeviceID);

    AudioObjectPropertyAddress def_in_addr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &def_in_addr, 0, NULL, &sz, &def_in);

    sz = sizeof(AudioDeviceID);
    AudioObjectPropertyAddress def_out_addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &def_out_addr, 0, NULL, &sz, &def_out);

    AudioObjectPropertyAddress devs_addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 data_size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devs_addr, 0, NULL, &data_size) != noErr) {
        return -2;
    }

    int count = (int)(data_size / sizeof(AudioDeviceID));
    if (count <= 0) return 0;

    AudioDeviceID* ids = (AudioDeviceID*)malloc(data_size);
    if (!ids) return -3;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devs_addr, 0, NULL, &data_size, ids) != noErr) {
        free(ids);
        return -4;
    }

    int out_count = (count < max_devices) ? count : max_devices;
    for (int i = 0; i < out_count; i++) {
        fill_device_info(ids[i], def_in, def_out, &out_devices[i]);
    }

    free(ids);
    return out_count;
}

int ascolto_macos_default_mic_status(ascolto_macos_device_info_t* out_info) {
    if (!out_info) return -1;
    AudioDeviceID def_in = kAudioObjectUnknown;
    UInt32 sz = sizeof(def_in);
    AudioObjectPropertyAddress def_in_addr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &def_in_addr, 0, NULL, &sz, &def_in);
    if (st != noErr || def_in == kAudioObjectUnknown) {
        return -2;
    }
    fill_device_info(def_in, def_in, kAudioObjectUnknown, out_info);
    return 0;
}

int ascolto_macos_device_status(uint32_t device_id, ascolto_macos_device_info_t* out_info) {
    if (!out_info) return -1;
    if (device_id == 0) {
        return ascolto_macos_default_mic_status(out_info);
    }
    fill_device_info((AudioDeviceID)device_id, kAudioObjectUnknown, kAudioObjectUnknown, out_info);
    return 0;
}

static bool is_ignored_system_daemon(const char* bundle, const char* pname) {
    if (strcmp(bundle, "com.cristianbianco.ascolto") == 0 ||
        strcmp(bundle, "com.apple.CoreSpeech") == 0 ||
        strcmp(bundle, "com.apple.replayd") == 0 ||
        strcmp(bundle, "com.apple.audiomxd") == 0 ||
        strcmp(bundle, "com.apple.controlcenter") == 0 ||
        strcmp(bundle, "com.apple.accessibility.heard") == 0 ||
        strcmp(bundle, "com.apple.SiriNCService") == 0 ||
        strcmp(bundle, "com.apple.PowerChime") == 0 ||
        strcmp(bundle, "com.apple.loginwindow") == 0 ||
        strcmp(bundle, "systemsoundserverd") == 0) {
        return true;
    }
    if (pname && (strcmp(pname, "ascolto") == 0 ||
                  strcmp(pname, "Ascolto") == 0 ||
                  strcmp(pname, "ascolto-harness") == 0 ||
                  strcmp(pname, "corespeechd") == 0 ||
                  strcmp(pname, "replayd") == 0)) {
        return true;
    }
    return false;
}

static void format_friendly_app_name(const char* bundle, const char* pname, int pid, char* out, size_t cap) {
    const char* friendly = NULL;
    if (strstr(bundle, "com.google.Chrome")) friendly = "Google Chrome";
    else if (strstr(bundle, "com.microsoft.teams")) friendly = "Microsoft Teams";
    else if (strstr(bundle, "com.tinyspeck.slackmacgap")) friendly = "Slack";
    else if (strstr(bundle, "us.zoom.xos")) friendly = "Zoom";
    else if (strstr(bundle, "com.apple.FaceTime") || strstr(bundle, "com.apple.avconferenced")) friendly = "FaceTime";
    else if (strstr(bundle, "com.apple.Safari") || strstr(bundle, "com.apple.WebKit.GPU")) friendly = "Safari / WebKit";
    else if (strstr(bundle, "company.thebrowser.Browser")) friendly = "Arc";
    else if (strstr(bundle, "org.mozilla.firefox")) friendly = "Firefox";
    else if (strstr(bundle, "com.brave.Browser")) friendly = "Brave";
    else if (strstr(bundle, "com.spotify.client")) friendly = "Spotify";
    else if (strstr(bundle, "com.hnc.Discord")) friendly = "Discord";
    else if (strstr(bundle, "ru.keepcoder.Telegram")) friendly = "Telegram";
    else if (strstr(bundle, "net.whatsapp.WhatsApp")) friendly = "WhatsApp";
    else if (strstr(bundle, "com.cisco.webexmeetingsapp")) friendly = "Webex";

    if (friendly) {
        snprintf(out, cap, "%s", friendly);
    } else if (pname && pname[0]) {
        snprintf(out, cap, "%s", pname);
    } else if (bundle && bundle[0]) {
        snprintf(out, cap, "%s", bundle);
    } else {
        snprintf(out, cap, "PID %d", pid);
    }
}

static void append_unique_app(char* list_buf, size_t cap, const char* app_label) {
    if (!list_buf || !app_label || !app_label[0]) return;
    if (strstr(list_buf, app_label) != NULL) return;
    size_t len = strlen(list_buf);
    if (len == 0) {
        snprintf(list_buf, cap, "%s", app_label);
    } else if (len + strlen(app_label) + 4 < cap) {
        snprintf(list_buf + len, cap - len, " + %s", app_label);
    }
}

bool ascolto_macos_query_audio_apps(
    char* out_in_apps,
    size_t in_cap,
    char* out_out_apps,
    size_t out_cap
) {
    if (out_in_apps && in_cap > 0) out_in_apps[0] = '\0';
    if (out_out_apps && out_cap > 0) out_out_apps[0] = '\0';

    AudioObjectPropertyAddress list_addr = {
        kAudioHardwarePropertyProcessObjectList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 data_size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &list_addr, 0, NULL, &data_size) != noErr || data_size == 0) {
        return false;
    }

    int count = (int)(data_size / sizeof(AudioObjectID));
    AudioObjectID* procs = (AudioObjectID*)malloc(data_size);
    if (!procs) return false;

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &list_addr, 0, NULL, &data_size, procs) != noErr) {
        free(procs);
        return false;
    }

    pid_t self_pid = getpid();
    bool any_in = false;

    for (int i = 0; i < count; i++) {
        AudioObjectID po = procs[i];
        pid_t pid = 0;
        UInt32 sz = sizeof(pid);
        AudioObjectPropertyAddress pid_addr = {
            kAudioProcessPropertyPID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(po, &pid_addr, 0, NULL, &sz, &pid) != noErr) continue;
        if (pid == self_pid || pid <= 0) continue;

        UInt32 running_in = 0, running_out = 0;
        sz = sizeof(UInt32);
        AudioObjectPropertyAddress in_addr = {
            kAudioProcessPropertyIsRunningInput,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(po, &in_addr, 0, NULL, &sz, &running_in);

        sz = sizeof(UInt32);
        AudioObjectPropertyAddress out_addr = {
            kAudioProcessPropertyIsRunningOutput,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(po, &out_addr, 0, NULL, &sz, &running_out);

        if (running_in == 0 && running_out == 0) continue;

        char bundle[128] = {0};
        CFStringRef bundle_ref = NULL;
        sz = sizeof(bundle_ref);
        AudioObjectPropertyAddress b_addr = {
            kAudioProcessPropertyBundleID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(po, &b_addr, 0, NULL, &sz, &bundle_ref) == noErr && bundle_ref) {
            CFStringGetCString(bundle_ref, bundle, sizeof(bundle), kCFStringEncodingUTF8);
            CFRelease(bundle_ref);
        }

        char pname[128] = {0};
        proc_name(pid, pname, sizeof(pname));

        if (is_ignored_system_daemon(bundle, pname)) continue;

        char friendly[128];
        format_friendly_app_name(bundle, pname, (int)pid, friendly, sizeof(friendly));

        if (running_in != 0) {
            any_in = true;
            if (out_in_apps && in_cap > 0) {
                append_unique_app(out_in_apps, in_cap, friendly);
            }
        }
        if (running_out != 0) {
            if (out_out_apps && out_cap > 0) {
                append_unique_app(out_out_apps, out_cap, friendly);
            }
        }
    }

    free(procs);
    return any_in;
}

bool ascolto_macos_external_app_using_mic(char* out_bundle_id, size_t bundle_cap, int* out_pid) {
    if (out_pid) *out_pid = 0;
    return ascolto_macos_query_audio_apps(out_bundle_id, bundle_cap, NULL, 0);
}

static EKEventStore* shared_event_store(void) {
    static EKEventStore* store = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        store = [[EKEventStore alloc] init];
    });
    return store;
}

static bool app_matches_call_target(NSRunningApplication* app, const char* active_app_name) {
    NSString* bid = app.bundleIdentifier ?: @"";
    NSString* loc = app.localizedName ?: @"";
    if (active_app_name && active_app_name[0] && strcmp(active_app_name, "Nessuna") != 0) {
        if (strstr(active_app_name, "Teams") && ([bid containsString:@"teams"] || [loc containsString:@"Teams"])) return true;
        if (strstr(active_app_name, "Chrome") && [bid isEqualToString:@"com.google.Chrome"]) return true;
        if (strstr(active_app_name, "Slack") && [bid containsString:@"slack"]) return true;
        if (strstr(active_app_name, "Zoom") && [bid containsString:@"zoom"]) return true;
        if (strstr(active_app_name, "FaceTime") && [bid containsString:@"FaceTime"]) return true;
        if (strstr(active_app_name, "Safari") && [bid containsString:@"Safari"]) return true;
        if (strstr(active_app_name, "Arc") && [bid containsString:@"thebrowser"]) return true;
        if (strstr(active_app_name, "Firefox") && [bid containsString:@"firefox"]) return true;
        if (strstr(active_app_name, "Brave") && [bid containsString:@"brave"]) return true;
        if (strstr(active_app_name, "Webex") && [bid containsString:@"webex"]) return true;
    }
    /* Fallback to common conferencing GUI apps */
    return ([bid isEqualToString:@"com.microsoft.teams2"] ||
            [bid isEqualToString:@"com.google.Chrome"] ||
            [bid isEqualToString:@"com.tinyspeck.slackmacgap"] ||
            [bid isEqualToString:@"us.zoom.xos"] ||
            [bid isEqualToString:@"com.apple.FaceTime"]);
}

static void sanitize_single_line(const char* src, char* dst, size_t cap) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t j = 0;
    bool prev_space = true;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        char c = src[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') {
            if (prev_space) continue;
            prev_space = true;
        } else {
            prev_space = false;
        }
        dst[j++] = c;
    }
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = '\0';
}

void ascolto_macos_query_call_metadata(
    const char* active_app_name,
    ascolto_call_metadata_t* out_meta
) {
    if (!out_meta) return;
    memset(out_meta, 0, sizeof(*out_meta));

    @autoreleasepool {
        /* 1. Query Window Title via Accessibility API (AXUIElement) */
        out_meta->ax_trusted = AXIsProcessTrusted();
        if (out_meta->ax_trusted) {
            NSString* bestTitle = nil;
            for (NSRunningApplication* rApp in [[NSWorkspace sharedWorkspace] runningApplications]) {
                if (rApp.activationPolicy != NSApplicationActivationPolicyRegular) continue;
                if (!app_matches_call_target(rApp, active_app_name)) continue;

                AXUIElementRef axApp = AXUIElementCreateApplication(rApp.processIdentifier);
                if (!axApp) continue;

                CFArrayRef axWins = NULL;
                if (AXUIElementCopyAttributeValue(axApp, kAXWindowsAttribute, (CFTypeRef*)&axWins) == kAXErrorSuccess && axWins) {
                    for (id w in (__bridge NSArray*)axWins) {
                        CFTypeRef titleRef = NULL;
                        if (AXUIElementCopyAttributeValue((__bridge AXUIElementRef)w, kAXTitleAttribute, &titleRef) == kAXErrorSuccess && titleRef) {
                            NSString* t = (__bridge_transfer NSString*)titleRef;
                            if (t.length > 0 &&
                                ![t containsString:@"Notification"] &&
                                ![t isEqualToString:@"Microsoft Teams"]) {
                                if (!bestTitle ||
                                    [t containsString:@"Meet -"] ||
                                    [t containsString:@"| Microsoft Teams"] ||
                                    [t containsString:@"Huddle"] ||
                                    [t containsString:@"Zoom"]) {
                                    bestTitle = t;
                                    break;
                                }
                            }
                        }
                    }
                    CFRelease(axWins);
                }
                CFRelease(axApp);
                if (bestTitle) break;
            }
            if (bestTitle) {
                sanitize_single_line([bestTitle UTF8String], out_meta->window_title, sizeof(out_meta->window_title));
            }
        }

        /* 2. Query Active Meeting from macOS Calendar (EventKit) */
        EKAuthorizationStatus calStatus = [EKEventStore authorizationStatusForEntityType:EKEntityTypeEvent];
        /* EKAuthorizationStatusFullAccess == 3, EKAuthorizationStatusAuthorized == 3 */
        out_meta->calendar_authorized = (calStatus >= 3);
        if (out_meta->calendar_authorized) {
            EKEventStore* store = shared_event_store();
            NSDate* now = [NSDate date];
            NSDate* startWin = [now dateByAddingTimeInterval:-300];
            NSDate* endWin = [now dateByAddingTimeInterval:300];
            NSPredicate* pred = [store predicateForEventsWithStartDate:startWin endDate:endWin calendars:nil];
            NSArray<EKEvent*>* events = [store eventsMatchingPredicate:pred];

            EKEvent* chosen = nil;
            for (EKEvent* ev in events) {
                if (ev.isAllDay) continue;
                chosen = ev;
                break;
            }
            if (!chosen && events.count > 0) {
                chosen = events.firstObject;
            }

            if (chosen) {
                if (chosen.title) {
                    sanitize_single_line([chosen.title UTF8String], out_meta->calendar_title, sizeof(out_meta->calendar_title));
                }
                if (chosen.organizer && chosen.organizer.name) {
                    sanitize_single_line([chosen.organizer.name UTF8String], out_meta->calendar_organizer, sizeof(out_meta->calendar_organizer));
                }
                if (chosen.attendees && chosen.attendees.count > 0) {
                    NSMutableArray<NSString*>* names = [NSMutableArray array];
                    for (EKParticipant* p in chosen.attendees) {
                        if (p.name.length > 0) {
                            [names addObject:p.name];
                        }
                    }
                    NSString* joined = [names componentsJoinedByString:@", "];
                    sanitize_single_line([joined UTF8String], out_meta->calendar_attendees, sizeof(out_meta->calendar_attendees));
                }
                if (chosen.notes && chosen.notes.length > 0) {
                    sanitize_single_line([chosen.notes UTF8String], out_meta->calendar_notes, sizeof(out_meta->calendar_notes));
                }
            }
        }
    }
}

void ascolto_macos_request_accessibility_permission(void) {
    @autoreleasepool {
        NSDictionary* opts = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt: @YES};
        AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts);
    }
}

void ascolto_macos_request_calendar_permission(void) {
    @autoreleasepool {
        EKEventStore* store = shared_event_store();
        if ([store respondsToSelector:@selector(requestFullAccessToEventsWithCompletion:)]) {
            [store requestFullAccessToEventsWithCompletion:^(BOOL granted, NSError* error) {
                (void)granted;
                (void)error;
            }];
        }
    }
}

/* --------------------------------------------------------------------------
 * Simulated Mic Monitor Backend
 * -------------------------------------------------------------------------- */
typedef struct {
    double active_start_s;
    double active_end_s;
    bool current_state;
} sim_mic_impl_t;

static bool sim_mic_is_active(ascolto_mic_monitor_t* self) {
    sim_mic_impl_t* impl = (sim_mic_impl_t*)self->impl;
    return impl ? impl->current_state : false;
}

static void sim_mic_step(ascolto_mic_monitor_t* self, double current_time_s) {
    sim_mic_impl_t* impl = (sim_mic_impl_t*)self->impl;
    if (!impl) return;
    bool next_state = (current_time_s >= impl->active_start_s && current_time_s < impl->active_end_s);
    if (next_state != impl->current_state) {
        impl->current_state = next_state;
        if (self->callback) {
            self->callback(next_state, current_time_s, self->userdata);
        }
    }
}

static void sim_mic_destroy(ascolto_mic_monitor_t* self) {
    if (!self) return;
    free(self->impl);
    free(self);
}

ascolto_mic_monitor_t* ascolto_mic_monitor_create_simulated(
    double active_start_s,
    double active_end_s,
    ascolto_mic_callback_t cb,
    void* userdata
) {
    ascolto_mic_monitor_t* m = (ascolto_mic_monitor_t*)calloc(1, sizeof(ascolto_mic_monitor_t));
    sim_mic_impl_t* impl = (sim_mic_impl_t*)calloc(1, sizeof(sim_mic_impl_t));
    if (!m || !impl) {
        free(m);
        free(impl);
        return NULL;
    }
    impl->active_start_s = active_start_s;
    impl->active_end_s = active_end_s;
    impl->current_state = false;

    m->backend_name = "simulated";
    m->is_active = sim_mic_is_active;
    m->step = sim_mic_step;
    m->destroy = sim_mic_destroy;
    m->callback = cb;
    m->userdata = userdata;
    m->impl = impl;
    return m;
}

/* --------------------------------------------------------------------------
 * macOS CoreAudio Mic Monitor Backend
 * -------------------------------------------------------------------------- */
typedef struct {
    bool last_state;
} macos_mic_impl_t;

static bool macos_mic_is_active(ascolto_mic_monitor_t* self) {
    ascolto_macos_device_info_t info;
    if (ascolto_macos_default_mic_status(&info) == 0) {
        macos_mic_impl_t* impl = (macos_mic_impl_t*)self->impl;
        if (impl) impl->last_state = info.is_running_somewhere;
        return info.is_running_somewhere;
    }
    return false;
}

static void macos_mic_step(ascolto_mic_monitor_t* self, double current_time_s) {
    macos_mic_impl_t* impl = (macos_mic_impl_t*)self->impl;
    if (!impl) return;
    ascolto_macos_device_info_t info;
    if (ascolto_macos_default_mic_status(&info) == 0) {
        if (info.is_running_somewhere != impl->last_state) {
            impl->last_state = info.is_running_somewhere;
            if (self->callback) {
                self->callback(impl->last_state, current_time_s, self->userdata);
            }
        }
    }
}

static void macos_mic_destroy(ascolto_mic_monitor_t* self) {
    if (!self) return;
    free(self->impl);
    free(self);
}

ascolto_mic_monitor_t* ascolto_mic_monitor_create_macos(
    ascolto_mic_callback_t cb,
    void* userdata
) {
    ascolto_mic_monitor_t* m = (ascolto_mic_monitor_t*)calloc(1, sizeof(ascolto_mic_monitor_t));
    macos_mic_impl_t* impl = (macos_mic_impl_t*)calloc(1, sizeof(macos_mic_impl_t));
    if (!m || !impl) {
        free(m);
        free(impl);
        return NULL;
    }
    m->backend_name = "macos-coreaudio";
    m->is_active = macos_mic_is_active;
    m->step = macos_mic_step;
    m->destroy = macos_mic_destroy;
    m->callback = cb;
    m->userdata = userdata;
    m->impl = impl;
    return m;
}
