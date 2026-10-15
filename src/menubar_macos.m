#include <signal.h>
#include <time.h>
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#include "ascolto.h"
#include "ascolto/macos_control.h"
#import "macos/status_icon.h"
#import "macos/settings.h"
#include "ascolto/ai_settings.h"

static NSString* menu_text(const char* text) {
    return text ? ([NSString stringWithUTF8String:text] ?: @"") : @"";
}

@interface AscoltoMenuController : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (nonatomic, assign) ascolto_live_app_t* app;
@property (nonatomic, assign) ascolto_macos_capture_t** capture;
@property (nonatomic, strong) NSStatusItem* statusItem;
@property (nonatomic, strong) AscoltoRecordingBadge* recordingBadge;
@property (nonatomic, strong) NSMenu* statusMenu;
@property (nonatomic, strong) NSTimer* refreshTimer;
@property (nonatomic, strong) dispatch_queue_t sessionQueue;
@property (nonatomic, assign) ascolto_control_t* control;
@property (nonatomic, strong) dispatch_queue_t deviceQueue;
@property (nonatomic, strong) NSData* deviceInventory;
@property (nonatomic, assign) BOOL deviceRefreshPending;
@property (nonatomic, assign) CFAbsoluteTime lastDeviceRefresh;
@property (nonatomic, assign) CFAbsoluteTime sessionActionStarted;
@property (nonatomic, assign) CFAbsoluteTime lastPendingLog;
@property (nonatomic, assign) BOOL sessionTransitionPending;
@property (nonatomic, assign) BOOL recordingTransitionPending;
@property (nonatomic, assign) BOOL terminationPending;
@property (nonatomic, assign) int smokeMainThreadTicks;
@property (nonatomic, assign) BOOL captureSmokeFailed;
@property (nonatomic, strong) AscoltoSettingsWindow* settingsWindow;
@property (nonatomic, strong) NSDictionary* settingsMenus;
@property (nonatomic, assign) BOOL statusMenuTracking;

@end

@implementation AscoltoMenuController

- (instancetype)initWithApp:(ascolto_live_app_t*)app {
    self = [super init];
    if (self) {
        _app = app;
        _sessionQueue = dispatch_queue_create("com.cristianbianco.ascolto.session-actions", DISPATCH_QUEUE_SERIAL);
        _deviceQueue = dispatch_queue_create("com.cristianbianco.ascolto.device-inventory", DISPATCH_QUEUE_SERIAL);
        /* A clock/title can overflow the menu bar's safe area on notched displays.
           Reserve one standard icon slot for every session state. */
        _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
        _statusMenu = [[NSMenu alloc] initWithTitle:@"Ascolto"];
        _statusMenu.delegate = self;
        _statusItem.menu = _statusMenu;
        _recordingBadge = [[AscoltoRecordingBadge alloc] initWithFrame:_statusItem.button.bounds];
        _recordingBadge.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _recordingBadge.hidden = YES;
        [_statusItem.button addSubview:_recordingBadge];

        [self rebuildMenu];
        [self updateStatusBarTitle];
        [self refreshDeviceInventory];

        _refreshTimer = [NSTimer timerWithTimeInterval:0.5
                                                         target:self
                                                       selector:@selector(onTimerTick:)
                                                       userInfo:nil
                                                        repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:_refreshTimer forMode:NSRunLoopCommonModes];
    }
    if (self) _control = ascolto_control_start(_app, _sessionQueue);
    return self;
}

- (void)onTimerTick:(NSTimer*)timer {
    (void)timer;
    _smokeMainThreadTicks++;
    if (_settingsWindow.window.visible && !_statusMenuTracking) [self rebuildMenu];
    [_settingsWindow refreshNativeValues];
    [self updateStatusBarTitle];
    if (_smokeMainThreadTicks == 1) {
        NSRect frame = _statusItem.button.window.frame;
        ascolto_diagnostic("Status item visible=%d image=%d frame=%.0f,%.0f,%.0f,%.0f",
                          _statusItem.isVisible, _statusItem.button.image != nil,
                          frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
    }
    if (CFAbsoluteTimeGetCurrent() - _lastDeviceRefresh >= 5.0) [self refreshDeviceInventory];
    if (_sessionTransitionPending && CFAbsoluteTimeGetCurrent() - _lastPendingLog >= 5.0) {
        _lastPendingLog = CFAbsoluteTimeGetCurrent();
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(_app, &snapshot);
        ascolto_diagnostic("UI action pending age_ms=%.0f state=%d mic_samples=%lld",
                          (_lastPendingLog - _sessionActionStarted) * 1000.0,
                          snapshot.state, (long long)snapshot.mic_samples);
    }
}

- (void)refreshDeviceInventory {
    if (_deviceRefreshPending || _terminationPending) return;
    _deviceRefreshPending = YES;
    _lastDeviceRefresh = CFAbsoluteTimeGetCurrent();
    /* The main thread owns the cache. A stalled HAL query occupies only this
       queue; it cannot delay session actions, menu construction, or shutdown. */
    __weak AscoltoMenuController* weakSelf = self;
    dispatch_async(_deviceQueue, ^{
        CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
        ascolto_diagnostic("Device inventory begin");
        ascolto_macos_device_info_t devices[32];
        int count = ascolto_macos_list_devices(devices, 32);
        NSData* inventory = count >= 0 ? [NSData dataWithBytes:devices length:(NSUInteger)count * sizeof(devices[0])] : nil;
        ascolto_diagnostic("Device inventory end duration_ms=%.0f count=%d",
                          (CFAbsoluteTimeGetCurrent() - started) * 1000.0, count);
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            AscoltoMenuController* controller = weakSelf;
            if (!controller || controller.terminationPending) return;
            if (inventory) controller.deviceInventory = inventory;
            controller.deviceRefreshPending = NO;
            /* The next menu opening reads the new cache. Do not replace menu
               items while AppKit is tracking a user's selection. */
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
}

- (void)updateStatusBarTitle {
    if (!_app || !_statusItem.button) return;
    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(_app, &snapshot);
    BOOL pending = _recordingTransitionPending || snapshot.state == ASCOLTO_STATE_STOPPING;
    BOOL recording = !pending && snapshot.state == ASCOLTO_STATE_RECORDING;
    NSImage* icon = ascolto_status_icon(recording, snapshot.state == ASCOLTO_STATE_PAUSED, pending);
    _statusItem.button.image = icon;
    _statusItem.button.imagePosition = NSImageOnly;
    _statusItem.button.title = @"";
    _recordingBadge.hidden = !recording;
    _recordingBadge.needsDisplay = YES;
    char title_buf[64];
    ascolto_live_app_format_statusbar(_app, title_buf, sizeof(title_buf));
    NSString* status = _sessionTransitionPending ? @"Ascolto…" : [NSString stringWithUTF8String:title_buf];
    _statusItem.button.toolTip = [NSString stringWithFormat:@"Ascolto · %@", status];
    [_statusItem.button setAccessibilityLabel:@"Ascolto"];
    [_statusItem.button setAccessibilityValue:status];
}

- (void)menuWillOpen:(NSMenu*)menu {
    (void)menu;
    _statusMenuTracking = YES;
    [self rebuildMenu];
    if (CFAbsoluteTimeGetCurrent() - _lastDeviceRefresh >= 2.0) [self refreshDeviceInventory];
}

- (void)menuDidClose:(NSMenu*)menu { (void)menu; _statusMenuTracking = NO; }

- (NSMenuItem*)itemWithTitle:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.target = self;
    return item;
}

- (void)rebuildMenu {
    [_statusMenu removeAllItems];
    _statusMenu.minimumWidth = 312;

    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(_app, &snapshot);
    ascolto_app_state_t st = snapshot.state;
    BOOL pending = _sessionTransitionPending || st == ASCOLTO_STATE_STOPPING;
    BOOL recording = st == ASCOLTO_STATE_RECORDING;
    NSString* heading = @"Ready for a call";
    NSString* subtitle = snapshot.config.auto_trigger_mic ? @"Automatic monitoring is on" : @"Start a session manually";
    if (st == ASCOLTO_STATE_PAUSED) {
        heading = @"Monitoring paused";
        subtitle = @"Manual transcription is available";
    }
    if (recording) {
        heading = @"Recording";
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)now.tv_sec + (double)now.tv_nsec / 1e9 - snapshot.session_start_s;
        unsigned long seconds = (unsigned long)MAX(0.0, elapsed);
        subtitle = [NSString stringWithFormat:@"%02lu:%02lu · %d phrases · %d speakers",
                    seconds / 60, seconds % 60, snapshot.utterances, snapshot.remote_speakers];
    }
    if (pending) {
        if (_recordingTransitionPending || st == ASCOLTO_STATE_STOPPING) {
            heading = st == ASCOLTO_STATE_STOPPING || recording ? @"Saving session…" : @"Starting transcription…";
            subtitle = st == ASCOLTO_STATE_STOPPING || recording ? @"Finishing transcription and audio" : @"Waiting for permission or audio device";
        } else {
            heading = @"Updating settings…";
        }
    }
    if (snapshot.settings_error && !pending) {
        heading = @"Settings could not be saved";
        subtitle = @"Previous settings kept. Review database permissions.";
    }
    if (snapshot.storage_error && !pending) {
        heading = @"Transcript storage failed";
        subtitle = @"Local files retained. Review storage before starting another call.";
    }
    NSMenuItem* header = [[NSMenuItem alloc] initWithTitle:heading action:nil keyEquivalent:@""];
    NSView* headerView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 312, 64)];
    NSButton* mark = [[NSButton alloc] initWithFrame:NSMakeRect(12, 20, 30, 30)];
    mark.bordered = NO;
    mark.title = @"";
    mark.image = ascolto_status_icon(recording && !pending, st == ASCOLTO_STATE_PAUSED, pending);
    mark.imagePosition = NSImageOnly;
    mark.enabled = NO;
    [headerView addSubview:mark];
    if (recording && !pending) {
        AscoltoRecordingBadge* badge = [[AscoltoRecordingBadge alloc] initWithFrame:mark.bounds];
        [mark addSubview:badge];
    }
    NSTextField* title = [NSTextField labelWithString:heading];
    title.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    title.frame = NSMakeRect(52, 34, 248, 21);
    [headerView addSubview:title];
    NSTextField* caption = [NSTextField labelWithString:subtitle];
    caption.font = [NSFont systemFontOfSize:11];
    caption.textColor = NSColor.secondaryLabelColor;
    caption.lineBreakMode = NSLineBreakByTruncatingTail;
    caption.frame = NSMakeRect(52, 13, 248, 18);
    caption.toolTip = subtitle;
    [headerView addSubview:caption];
    header.view = headerView;
    [_statusMenu addItem:header];
    [_statusMenu addItem:[NSMenuItem separatorItem]];
    NSMenu* details = [[NSMenu alloc] initWithTitle:@"Session details"];
    NSString* inStr = [NSString stringWithFormat:@"Microphone app: %@", menu_text(snapshot.active_in_apps[0] ? snapshot.active_in_apps : "None")];
    NSMenuItem* inItem = [self itemWithTitle:inStr action:nil key:@""];
    inItem.enabled = NO;
    [details addItem:inItem];

    NSString* outStr = [NSString stringWithFormat:@"Call audio app: %@", menu_text(snapshot.active_out_apps[0] ? snapshot.active_out_apps : "None")];
    NSMenuItem* outItem = [self itemWithTitle:outStr action:nil key:@""];
    outItem.enabled = NO;
    [details addItem:outItem];

    if (snapshot.call_meta.window_title[0] != '\0') {
        NSString* wtStr = [NSString stringWithFormat:@"Call title: %@", menu_text(snapshot.call_meta.window_title)];
        NSMenuItem* wtItem = [self itemWithTitle:wtStr action:nil key:@""];
        wtItem.enabled = NO;
        [details addItem:wtItem];
    } else if (!snapshot.call_meta.ax_trusted) {
        NSMenuItem* axItem = [self itemWithTitle:@"Allow Accessibility access…" action:@selector(onRequestAccessibility:) key:@""];
        [details addItem:axItem];
    }

    if (snapshot.call_meta.calendar_title[0] != '\0') {
        NSString* calStr = [NSString stringWithFormat:@"Meeting: %@", menu_text(snapshot.call_meta.calendar_title)];
        NSMenuItem* calItem = [self itemWithTitle:calStr action:nil key:@""];
        calItem.enabled = NO;
        [details addItem:calItem];
    } else if (!snapshot.call_meta.calendar_authorized) {
        NSMenuItem* calPermItem = [self itemWithTitle:@"Allow Calendar access…" action:@selector(onRequestCalendar:) key:@""];
        [details addItem:calPermItem];
    }

    NSString* mStr = [NSString stringWithFormat:@"Microphone: %@ (MIC %.0f dB | SYS %.0f dB)", menu_text(snapshot.active_mic_name), snapshot.last_mic_rms_db, snapshot.last_sys_rms_db];
    NSMenuItem* mItem = [self itemWithTitle:mStr action:nil key:@""];
    mItem.enabled = NO;
    [details addItem:mItem];

    NSString* sysPermStr = snapshot.sys_audio_permission_ok
        ? @"Call audio capture: active"
        : @"Allow call audio capture…";
    NSMenuItem* permItem = [self itemWithTitle:sysPermStr action:@selector(onRequestSysAudioPermission:) key:@""];
    [details addItem:permItem];

    NSString* prevStr = nil;
    if (snapshot.last_speaker[0] != '\0') {
        prevStr = [NSString stringWithFormat:@"Latest phrase [%@]: %@", menu_text(snapshot.last_speaker), menu_text(snapshot.last_utterance_text)];
    } else {
        prevStr = [NSString stringWithFormat:@"Latest phrase: %@", menu_text(snapshot.last_utterance_text)];
    }
    if (prevStr.length > 68) {
        prevStr = [[prevStr substringToIndex:65] stringByAppendingString:@"..."];
    }
    NSMenuItem* pItem = [self itemWithTitle:prevStr action:nil key:@""];
    pItem.enabled = NO;
    [details addItem:pItem];


    /* 2. Manual Start / Stop & Auto-Trigger Toggles */
    if (st == ASCOLTO_STATE_RECORDING || st == ASCOLTO_STATE_STOPPING) {
        NSMenuItem* stopItem = [self itemWithTitle:@"Stop transcription" action:@selector(onToggleRecording:) key:@"s"];
        if (st == ASCOLTO_STATE_STOPPING) stopItem.title = @"Finishing session…";
        stopItem.attributedTitle = [[NSAttributedString alloc] initWithString:stopItem.title attributes:@{NSForegroundColorAttributeName: NSColor.systemRedColor}];
        stopItem.enabled = !_sessionTransitionPending && st != ASCOLTO_STATE_STOPPING;
        [_statusMenu addItem:stopItem];
    } else {
        NSMenuItem* startItem = [self itemWithTitle:@"Start transcription" action:@selector(onToggleRecording:) key:@"r"];
        if (_sessionTransitionPending) startItem.title = @"Starting transcription…";
        startItem.enabled = !_sessionTransitionPending;
        [_statusMenu addItem:startItem];
    }

    NSMenuItem* detailRoot = [self itemWithTitle:@"Session details" action:nil key:@""];
    [details addItem:[NSMenuItem separatorItem]];
    NSMenuItem* aiRecap = [self itemWithTitle:@"Generate latest recap" action:@selector(onRunRecap:) key:@""];
    aiRecap.enabled = snapshot.current_md_path[0] && !recording && st != ASCOLTO_STATE_STOPPING;
    [details addItem:aiRecap];
    detailRoot.submenu = details;
    [_statusMenu addItem:detailRoot];
    if (recording && !snapshot.sys_audio_permission_ok) {
        [_statusMenu addItem:[self itemWithTitle:@"Allow call audio capture…" action:@selector(onRequestSysAudioPermission:) key:@""]];
    }
    [_statusMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* autoItem = [self itemWithTitle:@"Automatic monitoring" action:@selector(onToggleAutoTrigger:) key:@""];
    autoItem.state = snapshot.config.auto_trigger_mic ? NSControlStateValueOn : NSControlStateValueOff;
    [_statusMenu addItem:autoItem];

    NSMenuItem* pauseMenu = [self itemWithTitle:@"Pause monitoring" action:@selector(onTogglePause:) key:@""];
    pauseMenu.title = snapshot.config.paused ? @"Resume monitoring" : @"Pause monitoring";
    [_statusMenu addItem:pauseMenu];

    [_statusMenu addItem:[NSMenuItem separatorItem]];

    /* 3. Language Submenu */
    NSMenu* langMenu = [[NSMenu alloc] initWithTitle:@"Lingua"];
    struct { const char* code; const char* label; } langs[] = {
        {"auto", "Detect automatically"},
        {"it",   "Italiano (it)"},
        {"en",   "English (en)"},
        {"de",   "Deutsch (de)"},
        {"fr",   "Français (fr)"},
        {"es",   "Español (es)"},
        {"nl",   "Nederlands (nl)"},
        {"pl",   "Polski (pl)"}
    };
    for (size_t i = 0; i < sizeof(langs)/sizeof(langs[0]); i++) {
        NSMenuItem* li = [self itemWithTitle:[NSString stringWithUTF8String:langs[i].label] action:@selector(onSelectLanguage:) key:@""];
        li.representedObject = [NSString stringWithUTF8String:langs[i].code];
        if (strcmp(snapshot.config.language, langs[i].code) == 0) {
            li.state = NSControlStateValueOn;
        }
        [langMenu addItem:li];
    }
    /* Configuration menus now belong to the Settings window. */

    /* 4. CoreAudio Devices Submenus (MIC & SYS) */
    const ascolto_macos_device_info_t* devs = _deviceInventory.bytes;
    int n_devs = (int)(_deviceInventory.length / sizeof(*devs));

    NSMenu* micSub = [[NSMenu alloc] initWithTitle:@"Microfono"];
    NSMenuItem* defMicItem = [self itemWithTitle:@"System default" action:@selector(onSelectMicDevice:) key:@""];
    defMicItem.tag = 0;
    if (snapshot.config.mic_device_id == 0) defMicItem.state = NSControlStateValueOn;
    [micSub addItem:defMicItem];
    [micSub addItem:[NSMenuItem separatorItem]];

    for (int i = 0; i < n_devs; i++) {
        if (!devs[i].is_input) continue;
        NSString* dTitle = [NSString stringWithUTF8String:devs[i].device_name];
        NSMenuItem* di = [self itemWithTitle:dTitle action:@selector(onSelectMicDevice:) key:@""];
        di.tag = (NSInteger)devs[i].device_id;
        if (snapshot.config.mic_device_id == devs[i].device_id) di.state = NSControlStateValueOn;
        [micSub addItem:di];
    }
    if (!_deviceInventory && _deviceRefreshPending) {
        NSMenuItem* loading = [self itemWithTitle:@"Loading audio devices…" action:nil key:@""];
        loading.enabled = NO;
        [micSub addItem:loading];
    }


    NSMenu* sysSub = [[NSMenu alloc] initWithTitle:@"Audio di sistema"];
    NSMenuItem* autoSysItem = [self itemWithTitle:@"Automatic" action:@selector(onSelectSysDevice:) key:@""];
    autoSysItem.tag = 0;
    if (snapshot.config.sys_device_id == 0) autoSysItem.state = NSControlStateValueOn;
    [sysSub addItem:autoSysItem];
    [sysSub addItem:[NSMenuItem separatorItem]];

    for (int i = 0; i < n_devs; i++) {
        if (!devs[i].is_input) continue;
        NSString* dTitle = [NSString stringWithUTF8String:devs[i].device_name];
        NSMenuItem* di = [self itemWithTitle:dTitle action:@selector(onSelectSysDevice:) key:@""];
        di.tag = (NSInteger)devs[i].device_id;
        if (snapshot.config.sys_device_id == devs[i].device_id) di.state = NSControlStateValueOn;
        [sysSub addItem:di];
    }
    if (!_deviceInventory && _deviceRefreshPending) {
        NSMenuItem* loading = [self itemWithTitle:@"Loading audio devices…" action:nil key:@""];
        loading.enabled = NO;
        [sysSub addItem:loading];
    }


    /* 5. Speaker Diarization Sensitivity Submenu */
    NSMenu* diarSub = [[NSMenu alloc] initWithTitle:@"Riconoscimento degli interlocutori"];
    struct { float val; const char* label; } presets[] = {
        {0.72f, "High sensitivity"},
        {0.60f, "Balanced · recommended"},
        {0.48f, "Conservative"}
    };
    for (size_t i = 0; i < 3; i++) {
        NSMenuItem* pi = [self itemWithTitle:[NSString stringWithUTF8String:presets[i].label] action:@selector(onSelectDiarizerThreshold:) key:@""];
        pi.representedObject = @(presets[i].val);
        if (fabsf(snapshot.config.diarizer_threshold - presets[i].val) < 0.04f) {
            pi.state = NSControlStateValueOn;
        }
        [diarSub addItem:pi];
    }


    _settingsMenus = @{@"languageMenu": langMenu, @"micMenu": micSub, @"sysMenu": sysSub, @"speakersMenu": diarSub};
    [_statusMenu addItem:[self itemWithTitle:@"Settings…" action:@selector(onSettings:) key:@","]];
    [_statusMenu addItem:[NSMenuItem separatorItem]];

    /* Archive shortcuts remain in the menu bar. */
    NSMenuItem* openLastItem = [self itemWithTitle:@"Open latest transcript" action:@selector(onOpenLastTranscript:) key:@"o"];
    openLastItem.enabled = snapshot.current_md_path[0] != 0;
    [_statusMenu addItem:openLastItem];

    NSMenuItem* openDirItem = [self itemWithTitle:@"Open archive folder…" action:@selector(onOpenOutputFolder:) key:@""];
    [_statusMenu addItem:openDirItem];


    [_statusMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quitItem = [self itemWithTitle:@"Quit Ascolto" action:@selector(onQuit:) key:@"q"];
    [_statusMenu addItem:quitItem];
}

- (void)onSettings:(id)sender {
    (void)sender;
    if (!_settingsWindow) {
        __weak AscoltoMenuController* weakSelf = self;
        _settingsWindow = [[AscoltoSettingsWindow alloc] initWithOptions:^NSDictionary* {
            AscoltoMenuController* owner = weakSelf;
            if (!owner) return @{};
            ascolto_live_snapshot_t snapshot;
            ascolto_live_app_read_snapshot(owner.app, &snapshot);
            NSMutableDictionary* values = [owner.settingsMenus mutableCopy];
            values[@"config"] = menu_text(snapshot.config.config_path);
            values[@"output"] = menu_text(snapshot.config.output_dir);
            values[@"automatic"] = @(snapshot.config.auto_trigger_mic);
            values[@"saveAudio"] = @(snapshot.config.save_audio);
            values[@"cloudRecap"] = @(snapshot.config.cloud_recap);
            values[@"tts"] = @(snapshot.config.recap_tts);
            values[@"busy"] = @(owner.sessionTransitionPending || owner.terminationPending);
            return values;
        } target:self];
    }
    [_settingsWindow showSettings];
}

- (void)onSelectAIRecap:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) { ascolto_live_app_set_cloud_recap(app, false); }];
}

- (void)onMicrophonePrivacy:(id)sender {
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"]];
}

- (void)onRunRecap:(id)sender {
    [self onSettings:sender]; [self.settingsWindow performAITask:@"recap"];
}

- (void)performSessionAction:(void (^)(ascolto_live_app_t*))action {
    if (_sessionTransitionPending || _terminationPending) return;
    _sessionTransitionPending = YES;
    _sessionActionStarted = CFAbsoluteTimeGetCurrent();
    _lastPendingLog = _sessionActionStarted;
    ascolto_diagnostic("UI session action queued");
    [self updateStatusBarTitle];
    dispatch_async(_sessionQueue, ^{
        CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
        ascolto_diagnostic("UI session action begin");
        action(self.app);
        ascolto_diagnostic("UI session action end duration_ms=%.0f",
                          (CFAbsoluteTimeGetCurrent() - started) * 1000.0);
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            self.sessionTransitionPending = NO;
            self.recordingTransitionPending = NO;
            [self updateStatusBarTitle];
            ascolto_diagnostic("UI session action presented");
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
}

- (void)onToggleRecording:(id)sender {
    (void)sender;
    if (_sessionTransitionPending || _terminationPending) return;
    _recordingTransitionPending = YES;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        if (snapshot.state == ASCOLTO_STATE_RECORDING || snapshot.state == ASCOLTO_STATE_STOPPING) {
            ascolto_live_app_stop_session(app);
        } else {
            ascolto_live_app_start_session(app, true);
        }
    }];
}

- (void)onToggleAutoTrigger:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        ascolto_live_app_set_auto_trigger(app, !snapshot.config.auto_trigger_mic);
    }];
}

- (void)onTogglePause:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        ascolto_live_app_set_paused(app, !snapshot.config.paused);
    }];
}

- (void)onToggleSaveAudio:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        ascolto_live_app_set_save_audio(app, !snapshot.config.save_audio);
    }];
}

- (void)onToggleCloudRecap:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        ascolto_live_app_set_cloud_recap(app, !snapshot.config.cloud_recap);
    }];
}

- (void)onToggleRecapTTS:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        ascolto_live_app_set_recap_tts(app, !snapshot.config.recap_tts);
    }];
}

- (void)onRunLegacyRecap:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        if (ascolto_recap_start(&snapshot.config, snapshot.current_md_path, false, false)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                NSAlert* alert = [NSAlert new];
                alert.messageText = @"Could not generate the recap";
                alert.informativeText = @"Wait for audio saving to finish and check the Python setup described in README.macos.md.";
                [alert addButtonWithTitle:@"Close"];
                [alert runModal];
            });
        }
    }];
}

- (void)onSelectLanguage:(NSMenuItem*)sender {
    NSString* code = sender.representedObject;
    if (code) [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_app_set_language(app, code.UTF8String);
    }];
}

- (void)onSelectMicDevice:(NSMenuItem*)sender {
    uint32_t mic_id = (uint32_t)sender.tag;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        ascolto_live_app_set_devices(app, mic_id, snapshot.config.sys_device_id);
    }];
}

- (void)onSelectSysDevice:(NSMenuItem*)sender {
    uint32_t sys_id = (uint32_t)sender.tag;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        ascolto_live_app_set_devices(app, snapshot.config.mic_device_id, sys_id);
    }];
}

- (void)onSelectDiarizerThreshold:(NSMenuItem*)sender {
    NSNumber* number = sender.representedObject;
    if (number) [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_app_set_diarizer_threshold(app, number.floatValue);
    }];
}

- (void)onOpenLastTranscript:(id)sender {
    (void)sender;
    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(_app, &snapshot);
    if (snapshot.current_md_path[0]) {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:snapshot.current_md_path]]];
    } else {
        [self onOpenOutputFolder:nil];
    }
}

- (void)onOpenOutputFolder:(id)sender {
    (void)sender;
    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(_app, &snapshot);
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:snapshot.config.output_dir]]];
}

- (void)onChangeOutputFolder:(id)sender {
    (void)sender;
    NSOpenPanel* panel=[NSOpenPanel openPanel];
    panel.canChooseDirectories=YES;panel.canChooseFiles=NO;panel.canCreateDirectories=YES;
    panel.message=@"Choose where new calls are saved. Existing calls keep their current paths.";
    [panel beginWithCompletionHandler:^(NSModalResponse result){
        if(result!=NSModalResponseOK)return;
        NSString* path=panel.URL.path;
        [self performSessionAction:^(ascolto_live_app_t* app){
            if(ascolto_live_app_set_output_directory(app,path.fileSystemRepresentation))
                dispatch_async(dispatch_get_main_queue(), ^{NSAlert* alert=[NSAlert new];alert.messageText=@"Could not save the archive folder";alert.informativeText=@"Check the folder path and write permissions.";[alert runModal];});
        }];
    }];
}

- (void)onEditKeywords:(id)sender {
    [self onSettings:sender];
    [self.settingsWindow setValue:@"Transcription" forKey:@"category"];
    [self.settingsWindow performSelector:@selector(buildPage)];
}

- (void)onRequestSysAudioPermission:(id)sender {
    (void)sender;
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"];
    if (url) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

- (void)onRequestAccessibility:(id)sender {
    (void)sender;
    ascolto_macos_request_accessibility_permission();
}

- (void)onRequestCalendar:(id)sender {
    (void)sender;
    ascolto_macos_request_calendar_permission();
}

- (void)onQuit:(id)sender {
    [_settingsWindow cancelWorker];
    (void)sender;
    [NSApp terminate:nil];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    [_settingsWindow cancelWorker];
    if (_terminationPending) return NSTerminateLater;
    _terminationPending = YES;
    [_refreshTimer invalidate];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      ascolto_control_stop(self.control);
      self.control = NULL;
      dispatch_async(self.sessionQueue, ^{
        if (self.capture && *self.capture) {
            ascolto_macos_capture_stop(*self.capture);
            *self.capture = NULL;
        }
        ascolto_live_app_stop_session(self.app);
        ascolto_archive_wait();
        /* NSTerminateLater may run a nested modal loop inside a main dispatch
           callback. Schedule its reply on the run loop so that callback can finish. */
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            if (getenv("ASCOLTO_CAPTURE_SMOKE_TEST")) {
                fflush(NULL);
                exit(self.captureSmokeFailed ? 1 : 0);
            }
            [sender replyToApplicationShouldTerminate:YES];
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
    });
    return NSTerminateLater;
}

@end

int ascolto_run_menubar_app(ascolto_live_app_t* app, ascolto_macos_capture_t** capture) {
    if (!app) return -1;
    @autoreleasepool {
        NSApplication* nsApp = [NSApplication sharedApplication];
        [nsApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        AscoltoMenuController* controller = [[AscoltoMenuController alloc] initWithApp:app];
        nsApp.delegate = controller;
        controller.capture = capture;
        /* Finalize on the main queue; mutexes and ffmpeg are unsafe in POSIX signal handlers. */
        signal(SIGTERM, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        dispatch_source_t term_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
        dispatch_source_t int_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(term_source, ^{ [nsApp terminate:nil]; });
        dispatch_source_set_event_handler(int_source, ^{ [nsApp terminate:nil]; });
        dispatch_resume(term_source);
        dispatch_resume(int_source);

        if (getenv("ASCOLTO_CAPTURE_SMOKE_TEST")) {
            /* Drive the same menu action that the user clicked and check the real MIC.
               Use temporary artifacts; a parent runner bounds hardware API waits. */
            const char* rounds_env = getenv("ASCOLTO_CAPTURE_TEST_ROUNDS");
            int rounds = rounds_env ? atoi(rounds_env) : 1;
            if (rounds < 1 || rounds > 3) rounds = 1;
            for (int round = 0; round < rounds; round++) {
                __block int starting_ticks = 0;
                int offset = round * 7;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((offset + 0.25) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                    starting_ticks = controller.smokeMainThreadTicks;
                    if (getenv("ASCOLTO_DEVICE_TEST_DELAY_MS")) {
                        double menu_start = CFAbsoluteTimeGetCurrent();
                        [controller menuWillOpen:controller.statusMenu];
                        double menu_ms = (CFAbsoluteTimeGetCurrent() - menu_start) * 1000.0;
                        BOOL passed = menu_ms < 100.0 && controller.deviceRefreshPending;
                        if (!passed) controller.captureSmokeFailed = YES;
                        printf("[capture-smoke] Menu during slow inventory: %s | duration_ms=%.2f\n", passed ? "PASS" : "FAIL", menu_ms);
                    }
                    [controller onToggleRecording:nil];
                    if (getenv("ASCOLTO_DEVICE_TEST_DELAY_MS")) {
                        /* Simulate menu tracking while the initiating main-dispatch
                           callback is still active. Its completion must run here. */
                        CFRunLoopRunInMode((__bridge CFStringRef)NSEventTrackingRunLoopMode, 0.5, false);
                        BOOL passed = !controller.sessionTransitionPending;
                        if (!passed) controller.captureSmokeFailed = YES;
                        printf("[capture-smoke] Start completion during menu tracking: %s\n", passed ? "PASS" : "FAIL");
                    }
                });
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (offset + 4) * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                    ascolto_live_snapshot_t snapshot;
                    ascolto_live_app_read_snapshot(app, &snapshot);
                    long long mic_samples = snapshot.mic_samples;
                    long long mic_padding = snapshot.mic_padding;
                    const char* delay_env = getenv("ASCOLTO_CAPTURE_TEST_START_DELAY_MS");
                    BOOL delayed_start = delay_env && atoi(delay_env) >= 4000;
                    BOOL pending_permission = getenv("ASCOLTO_CAPTURE_TEST_PERMISSION_DELAY_MS") != NULL;
                    BOOL passed = controller.smokeMainThreadTicks - starting_ticks >= 4 &&
                                  !controller.sessionTransitionPending &&
                                  (delayed_start || pending_permission || mic_samples - mic_padding >= ASCOLTO_SAMPLE_RATE);
                    if (!passed) controller.captureSmokeFailed = YES;
                    printf("[capture-smoke] Start: %s | main_ticks=%d | MIC samples=%lld | MIC padding=%lld\n",
                           passed ? "PASS" : "FAIL", controller.smokeMainThreadTicks, mic_samples, mic_padding);
                    double action_start = CFAbsoluteTimeGetCurrent();
                    [controller onToggleRecording:nil];
                    double action_ms = (CFAbsoluteTimeGetCurrent() - action_start) * 1000.0;
                    if (action_ms > 100.0) controller.captureSmokeFailed = YES;
                    printf("[capture-smoke] Stop action returned in %.2f ms\n", action_ms);
                    if (pending_permission) {
                        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
                        ascolto_live_app_read_snapshot(app, &snapshot);
                        BOOL cancelled = snapshot.state == ASCOLTO_STATE_IDLE &&
                                         !controller.sessionTransitionPending && snapshot.mic_samples == 0;
                        if (!cancelled) controller.captureSmokeFailed = YES;
                        printf("[capture-smoke] Stop during pending permission: %s\n", cancelled ? "PASS" : "FAIL");
                    }
                });
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((offset + 6.75) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                    ascolto_live_snapshot_t snapshot;
                    ascolto_live_app_read_snapshot(app, &snapshot);
                    BOOL stopped = snapshot.state == ASCOLTO_STATE_IDLE;
                    BOOL passed = stopped && !controller.sessionTransitionPending;
                    if (!passed) controller.captureSmokeFailed = YES;
                    printf("[capture-smoke] Stop: %s | main_ticks=%d\n", passed ? "PASS" : "FAIL", controller.smokeMainThreadTicks);
                    if (getenv("ASCOLTO_DEVICE_TEST_DELAY_MS")) {
                        BOOL cache_ready = controller.deviceInventory.length > 0;
                        if (!cache_ready) controller.captureSmokeFailed = YES;
                        printf("[capture-smoke] Device cache populated: %s\n", cache_ready ? "PASS" : "FAIL");
                    }
                    fflush(NULL);
                });
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, rounds * 7 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                [nsApp terminate:nil];
            });
        }

        /* Support automated smoke-test exit via environment variable ASCOLTO_MENUBAR_SMOKE_MS */
        const char* smoke_ms = getenv("ASCOLTO_MENUBAR_SMOKE_MS");
        if (smoke_ms && atoi(smoke_ms) > 0) {
            double delay_s = (double)atoi(smoke_ms) / 1000.0;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay_s * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{
                [controller rebuildMenu];
                [controller updateStatusBarTitle];
                [nsApp terminate:nil];
            });
        }

        [nsApp run];
        dispatch_source_cancel(term_source);
        dispatch_source_cancel(int_source);
    }
    return 0;
}
