#include <signal.h>
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#include "ascolto.h"
#include "ascolto/macos_control.h"

@interface AscoltoMenuController : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (nonatomic, assign) ascolto_live_app_t* app;
@property (nonatomic, assign) ascolto_macos_capture_t** capture;
@property (nonatomic, strong) NSStatusItem* statusItem;
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
@property (nonatomic, assign) BOOL terminationPending;
@property (nonatomic, assign) int smokeMainThreadTicks;
@property (nonatomic, assign) BOOL captureSmokeFailed;

@end

@implementation AscoltoMenuController

- (instancetype)initWithApp:(ascolto_live_app_t*)app {
    self = [super init];
    if (self) {
        _app = app;
        _sessionQueue = dispatch_queue_create("com.cristianbianco.ascolto.session-actions", DISPATCH_QUEUE_SERIAL);
        _deviceQueue = dispatch_queue_create("com.cristianbianco.ascolto.device-inventory", DISPATCH_QUEUE_SERIAL);
        _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        _statusMenu = [[NSMenu alloc] initWithTitle:@"Ascolto"];
        _statusMenu.delegate = self;
        _statusItem.menu = _statusMenu;

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
    [self updateStatusBarTitle];
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
    if (_sessionTransitionPending) {
        _statusItem.button.title = @"Ascolto…";
        return;
    }
    char title_buf[64];
    ascolto_live_app_format_statusbar(_app, title_buf, sizeof(title_buf));
    _statusItem.button.title = [NSString stringWithUTF8String:title_buf];
}

- (void)menuWillOpen:(NSMenu*)menu {
    (void)menu;
    [self rebuildMenu];
    if (CFAbsoluteTimeGetCurrent() - _lastDeviceRefresh >= 2.0) [self refreshDeviceInventory];
}

- (NSMenuItem*)itemWithTitle:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.target = self;
    return item;
}

- (void)rebuildMenu {
    [_statusMenu removeAllItems];

    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(_app, &snapshot);
    ascolto_app_state_t st = snapshot.state;
    /* 1. Status Header */
    NSString* stateStr = @"Stato: 🎙️ In attesa di chiamata";
    if (st == ASCOLTO_STATE_RECORDING) {
        stateStr = [NSString stringWithFormat:@"Stato: 🔴 In registrazione (%d frasi, %d interlocutori)", snapshot.utterances, snapshot.remote_speakers];
    } else if (st == ASCOLTO_STATE_STOPPING) {
        stateStr = @"Stopping transcription…";
    } else if (st == ASCOLTO_STATE_PAUSED) {
        stateStr = @"Stato: ⏸️ Monitoraggio in pausa";
    }
    NSMenuItem* sItem = [self itemWithTitle:stateStr action:nil key:@""];
    sItem.enabled = NO;
    [_statusMenu addItem:sItem];

    NSString* inStr = [NSString stringWithFormat:@"App IN (MIC): %s", snapshot.active_in_apps[0] ? snapshot.active_in_apps : "Nessuna"];
    NSMenuItem* inItem = [self itemWithTitle:inStr action:nil key:@""];
    inItem.enabled = NO;
    [_statusMenu addItem:inItem];

    NSString* outStr = [NSString stringWithFormat:@"App OUT (SYS): %s", snapshot.active_out_apps[0] ? snapshot.active_out_apps : "Nessuna"];
    NSMenuItem* outItem = [self itemWithTitle:outStr action:nil key:@""];
    outItem.enabled = NO;
    [_statusMenu addItem:outItem];

    if (snapshot.call_meta.window_title[0] != '\0') {
        NSString* wtStr = [NSString stringWithFormat:@"Titolo Finestra: %s", snapshot.call_meta.window_title];
        NSMenuItem* wtItem = [self itemWithTitle:wtStr action:nil key:@""];
        wtItem.enabled = NO;
        [_statusMenu addItem:wtItem];
    } else if (!snapshot.call_meta.ax_trusted) {
        NSMenuItem* axItem = [self itemWithTitle:@"🔓 Abilita Accessibilità (per leggere titolo finestra call)..." action:@selector(onRequestAccessibility:) key:@""];
        [_statusMenu addItem:axItem];
    }

    if (snapshot.call_meta.calendar_title[0] != '\0') {
        NSString* calStr = [NSString stringWithFormat:@"📅 Riunione: %s", snapshot.call_meta.calendar_title];
        NSMenuItem* calItem = [self itemWithTitle:calStr action:nil key:@""];
        calItem.enabled = NO;
        [_statusMenu addItem:calItem];
    } else if (!snapshot.call_meta.calendar_authorized) {
        NSMenuItem* calPermItem = [self itemWithTitle:@"📅 Abilita Calendario (per titolo e partecipanti call)..." action:@selector(onRequestCalendar:) key:@""];
        [_statusMenu addItem:calPermItem];
    }

    NSString* mStr = [NSString stringWithFormat:@"Microfono: %s (MIC %.0f dB | SYS %.0f dB)", snapshot.active_mic_name, snapshot.last_mic_rms_db, snapshot.last_sys_rms_db];
    NSMenuItem* mItem = [self itemWithTitle:mStr action:nil key:@""];
    mItem.enabled = NO;
    [_statusMenu addItem:mItem];

    NSString* sysPermStr = snapshot.sys_audio_permission_ok
        ? @"Audio Sistema (SYS): ✅ CoreAudio Tap attivo (no condivisione schermo)"
        : @"Audio Sistema (SYS): CoreAudio Tap pronto (attiva in Impostazioni se muto)...";
    NSMenuItem* permItem = [self itemWithTitle:sysPermStr action:@selector(onRequestSysAudioPermission:) key:@""];
    [_statusMenu addItem:permItem];

    NSString* prevStr = nil;
    if (snapshot.last_speaker[0] != '\0') {
        prevStr = [NSString stringWithFormat:@"Ultimo [%s]: %s", snapshot.last_speaker, snapshot.last_utterance_text];
    } else {
        prevStr = [NSString stringWithFormat:@"Ultimo: %s", snapshot.last_utterance_text];
    }
    if (prevStr.length > 68) {
        prevStr = [[prevStr substringToIndex:65] stringByAppendingString:@"..."];
    }
    NSMenuItem* pItem = [self itemWithTitle:prevStr action:nil key:@""];
    pItem.enabled = NO;
    [_statusMenu addItem:pItem];

    [_statusMenu addItem:[NSMenuItem separatorItem]];

    /* 2. Manual Start / Stop & Auto-Trigger Toggles */
    if (st == ASCOLTO_STATE_RECORDING || st == ASCOLTO_STATE_STOPPING) {
        NSMenuItem* stopItem = [self itemWithTitle:@"⏹ Ferma trascrizione ora" action:@selector(onToggleRecording:) key:@"s"];
        stopItem.enabled = !_sessionTransitionPending && st != ASCOLTO_STATE_STOPPING;
        [_statusMenu addItem:stopItem];
    } else {
        NSMenuItem* startItem = [self itemWithTitle:@"▶ Avvia trascrizione ora" action:@selector(onToggleRecording:) key:@"r"];
        startItem.enabled = !_sessionTransitionPending;
        [_statusMenu addItem:startItem];
    }

    NSMenuItem* autoItem = [self itemWithTitle:@"Attiva automaticamente col microfono" action:@selector(onToggleAutoTrigger:) key:@""];
    autoItem.state = snapshot.config.auto_trigger_mic ? NSControlStateValueOn : NSControlStateValueOff;
    [_statusMenu addItem:autoItem];

    NSMenuItem* pauseMenu = [self itemWithTitle:@"Sospendi monitoraggio automatico" action:@selector(onTogglePause:) key:@""];
    pauseMenu.state = snapshot.config.paused ? NSControlStateValueOn : NSControlStateValueOff;
    [_statusMenu addItem:pauseMenu];

    NSMenuItem* saveAudioItem = [self itemWithTitle:@"Salva audio compresso (.m4a ~21 MB/ora)" action:@selector(onToggleSaveAudio:) key:@""];
    saveAudioItem.state = snapshot.config.save_audio ? NSControlStateValueOn : NSControlStateValueOff;
    [_statusMenu addItem:saveAudioItem];
    NSMenuItem* recapItem = [self itemWithTitle:@"Cloud recap (OpenAI + Claude)" action:@selector(onToggleCloudRecap:) key:@""];
    recapItem.state = snapshot.config.cloud_recap ? NSControlStateValueOn : NSControlStateValueOff;
    [_statusMenu addItem:recapItem];
    NSMenuItem* ttsItem = [self itemWithTitle:@"Generate recap voice (OpenAI TTS)" action:@selector(onToggleRecapTTS:) key:@""];
    ttsItem.state = snapshot.config.recap_tts ? NSControlStateValueOn : NSControlStateValueOff;
    [_statusMenu addItem:ttsItem];
    NSMenuItem* runRecap = [self itemWithTitle:@"Generate recap for the last session" action:@selector(onRunRecap:) key:@""];
    runRecap.enabled = snapshot.current_md_path[0] && st != ASCOLTO_STATE_RECORDING && st != ASCOLTO_STATE_STOPPING;
    [_statusMenu addItem:runRecap];

    [_statusMenu addItem:[NSMenuItem separatorItem]];

    /* 3. Language Submenu */
    NSMenuItem* langRoot = [self itemWithTitle:@"Lingua trascrizione (Whistle)" action:nil key:@""];
    NSMenu* langMenu = [[NSMenu alloc] initWithTitle:@"Lingua"];
    struct { const char* code; const char* label; } langs[] = {
        {"auto", "Rilevamento automatico (Auto)"},
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
    langRoot.submenu = langMenu;
    [_statusMenu addItem:langRoot];

    /* 4. CoreAudio Devices Submenus (MIC & SYS) */
    const ascolto_macos_device_info_t* devs = _deviceInventory.bytes;
    int n_devs = (int)(_deviceInventory.length / sizeof(*devs));

    NSMenuItem* micRoot = [self itemWithTitle:@"Dispositivo Microfono (Io / ME)" action:nil key:@""];
    NSMenu* micSub = [[NSMenu alloc] initWithTitle:@"Microfono"];
    NSMenuItem* defMicItem = [self itemWithTitle:@"Predefinito di sistema" action:@selector(onSelectMicDevice:) key:@""];
    defMicItem.tag = 0;
    if (snapshot.config.mic_device_id == 0) defMicItem.state = NSControlStateValueOn;
    [micSub addItem:defMicItem];
    [micSub addItem:[NSMenuItem separatorItem]];

    for (int i = 0; i < n_devs; i++) {
        if (!devs[i].is_input) continue;
        NSString* dTitle = [NSString stringWithFormat:@"%s (ID %u)", devs[i].device_name, devs[i].device_id];
        NSMenuItem* di = [self itemWithTitle:dTitle action:@selector(onSelectMicDevice:) key:@""];
        di.tag = (NSInteger)devs[i].device_id;
        if (snapshot.config.mic_device_id == devs[i].device_id) di.state = NSControlStateValueOn;
        [micSub addItem:di];
    }
    micRoot.submenu = micSub;
    [_statusMenu addItem:micRoot];

    NSMenuItem* sysRoot = [self itemWithTitle:@"Sorgente Audio Call (Altri / SYS)" action:nil key:@""];
    NSMenu* sysSub = [[NSMenu alloc] initWithTitle:@"Audio Sistema"];
    NSMenuItem* autoSysItem = [self itemWithTitle:@"Automatico (Virtual / Loopback)" action:@selector(onSelectSysDevice:) key:@""];
    autoSysItem.tag = 0;
    if (snapshot.config.sys_device_id == 0) autoSysItem.state = NSControlStateValueOn;
    [sysSub addItem:autoSysItem];
    [sysSub addItem:[NSMenuItem separatorItem]];

    for (int i = 0; i < n_devs; i++) {
        if (!devs[i].is_input) continue;
        NSString* dTitle = [NSString stringWithFormat:@"%s (ID %u)", devs[i].device_name, devs[i].device_id];
        NSMenuItem* di = [self itemWithTitle:dTitle action:@selector(onSelectSysDevice:) key:@""];
        di.tag = (NSInteger)devs[i].device_id;
        if (snapshot.config.sys_device_id == devs[i].device_id) di.state = NSControlStateValueOn;
        [sysSub addItem:di];
    }
    sysRoot.submenu = sysSub;
    [_statusMenu addItem:sysRoot];

    /* 5. Speaker Diarization Sensitivity Submenu */
    NSMenuItem* diarRoot = [self itemWithTitle:@"Sensibilità Interlocutori (Diarization)" action:nil key:@""];
    NSMenu* diarSub = [[NSMenu alloc] initWithTitle:@"Diarization"];
    struct { float val; const char* label; } presets[] = {
        {0.72f, "Alta — Distingue più interlocutori (0.72)"},
        {0.60f, "Bilanciata — Consigliata (0.60)"},
        {0.48f, "Conservativa — Meno interlocutori (0.48)"}
    };
    for (size_t i = 0; i < 3; i++) {
        NSMenuItem* pi = [self itemWithTitle:[NSString stringWithUTF8String:presets[i].label] action:@selector(onSelectDiarizerThreshold:) key:@""];
        pi.representedObject = @(presets[i].val);
        if (fabsf(snapshot.config.diarizer_threshold - presets[i].val) < 0.04f) {
            pi.state = NSControlStateValueOn;
        }
        [diarSub addItem:pi];
    }
    diarRoot.submenu = diarSub;
    [_statusMenu addItem:diarRoot];

    [_statusMenu addItem:[NSMenuItem separatorItem]];

    /* 6. Quick Actions: Open Latest Transcript, Open Folder, Edit Keywords */
    NSMenuItem* openLastItem = [self itemWithTitle:@"Apri ultima trascrizione (.md)" action:@selector(onOpenLastTranscript:) key:@"o"];
    [_statusMenu addItem:openLastItem];

    NSMenuItem* openDirItem = [self itemWithTitle:@"Apri cartella trascrizioni..." action:@selector(onOpenOutputFolder:) key:@""];
    [_statusMenu addItem:openDirItem];

    NSMenuItem* editKwItem = [self itemWithTitle:@"Modifica Keywords personalizzate..." action:@selector(onEditKeywords:) key:@""];
    [_statusMenu addItem:editKwItem];

    [_statusMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quitItem = [self itemWithTitle:@"Esci da Ascolto" action:@selector(onQuit:) key:@"q"];
    [_statusMenu addItem:quitItem];
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
            [self updateStatusBarTitle];
            ascolto_diagnostic("UI session action presented");
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
}

- (void)onToggleRecording:(id)sender {
    (void)sender;
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

- (void)onRunRecap:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        if (ascolto_recap_start(&snapshot.config, snapshot.current_md_path, false, false)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                NSAlert* alert = [NSAlert new];
                alert.messageText = @"Recap could not start";
                alert.informativeText = @"Wait for audio finalization and check the Python setup in README.macos.md.";
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

- (void)onEditKeywords:(id)sender {
    (void)sender;
    [self performSessionAction:^(ascolto_live_app_t* app) {
        ascolto_live_snapshot_t snapshot;
        ascolto_live_app_read_snapshot(app, &snapshot);
        ascolto_config_save(&snapshot.config, NULL);
        NSString* path = [NSString stringWithUTF8String:snapshot.config.keywords_path];
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
        });
    }];
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
    (void)sender;
    [NSApp terminate:nil];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
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
                    BOOL passed = controller.smokeMainThreadTicks - starting_ticks >= 4 &&
                                  !controller.sessionTransitionPending &&
                                  (delayed_start || mic_samples - mic_padding >= ASCOLTO_SAMPLE_RATE);
                    if (!passed) controller.captureSmokeFailed = YES;
                    printf("[capture-smoke] Start: %s | main_ticks=%d | MIC samples=%lld | MIC padding=%lld\n",
                           passed ? "PASS" : "FAIL", controller.smokeMainThreadTicks, mic_samples, mic_padding);
                    double action_start = CFAbsoluteTimeGetCurrent();
                    [controller onToggleRecording:nil];
                    double action_ms = (CFAbsoluteTimeGetCurrent() - action_start) * 1000.0;
                    if (action_ms > 100.0) controller.captureSmokeFailed = YES;
                    printf("[capture-smoke] Stop action returned in %.2f ms\n", action_ms);
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
