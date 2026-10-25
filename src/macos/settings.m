#import "settings.h"
#import "activity.h"
#import "app_icon.h"
#include "ascolto/ai_settings.h"
#include "ascolto/local_stt.h"

/* Explicit foreground drawing keeps the branded button readable in key and
 * inactive windows, independently of AppKit's bezel/tint rendering. */
@interface AscoltoChatGPTButton : NSButton
@end
@implementation AscoltoChatGPTButton
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSBezierPath* background = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 1, 1) xRadius:6 yRadius:6];
    [(self.highlighted ? [NSColor colorWithWhite:0.16 alpha:1] : NSColor.blackColor) setFill];
    [background fill];
    CGFloat opacity = self.enabled ? 1 : 0.5;
    NSDictionary* attributes = @{NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor.whiteColor colorWithAlphaComponent:opacity]};
    NSSize titleSize = [self.title sizeWithAttributes:attributes];
    CGFloat logoSize = self.image ? 20 : 0, gap = self.image ? 8 : 0;
    CGFloat x = (NSWidth(self.bounds) - titleSize.width - logoSize - gap) / 2;
    if (self.image) {
        NSImage* source = self.image;
        NSImage* foreground = [NSImage imageWithSize:NSMakeSize(20, 20) flipped:NO drawingHandler:^BOOL(NSRect bounds) {
            [source drawInRect:bounds];
            [NSColor.whiteColor setFill];
            NSRectFillUsingOperation(bounds, NSCompositingOperationSourceIn);
            return YES;
        }];
        [foreground drawInRect:NSMakeRect(x, (NSHeight(self.bounds) - logoSize) / 2, logoSize, logoSize)
            fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:opacity respectFlipped:YES hints:nil];
    }
    [self.title drawAtPoint:NSMakePoint(x + logoSize + gap, (NSHeight(self.bounds) - titleSize.height) / 2) withAttributes:attributes];
    if (self.window.firstResponder == self) {
        [NSGraphicsContext saveGraphicsState];
        NSSetFocusRingStyle(NSFocusRingOnly);
        [background fill];
        [NSGraphicsContext restoreGraphicsState];
    }
}
@end

@interface AscoltoSettingsCanvas : NSView
@end
@implementation AscoltoSettingsCanvas
- (BOOL)isFlipped { return YES; }
@end

@interface AscoltoSettingsWindow () <NSWindowDelegate, NSMenuDelegate>
@property (copy) NSDictionary* (^options)(void);
@property (weak) id actionTarget;
@property NSView* page;
@property NSView* sidebar;
@property NSScrollView* pageScroll;
@property NSView* canvas;
@property BOOL nativeMenuTracking;
@property NSMutableArray<NSPopUpButton*>* nativePopups;
@property NSString* category;
@property NSString* provider;
@property NSDictionary* settings;
@property NSTextField* notice;
@property NSTextField* archiveLocation;
@property NSSecureTextField* secureKey;
@property NSTextField* plainKey;
@property NSPopUpButton* model;
@property NSPopUpButton* language;
@property NSPopUpButton* transcriptionModel;
@property NSButton* audioConsent;
@property NSButton* autoTranscription;
@property NSTextField* keywords;
@property NSPopUpButton* account;
@property NSButton* automatic;
@property NSButton* monitoring;
@property NSButton* saveAudio;
@property NSButton* legacy;
@property NSButton* legacyTTS;
@property NSButton* connect;
@property NSButton* useProvider;
@property NSTask* worker;
@property BOOL busy;
@property BOOL closing;
@property NSString* message;
@property NSString* pendingTask;
@property NSString* latestAIOutput;
@property NSInteger transcriptionTab;
@property NSView* localRows;
@property uint64_t localRevision;
@property BOOL renderingLocal;
@property BOOL lastLocalBusy;
@property AscoltoActivityViewController* activity;
@property BOOL userNavigated;

@end

@implementation AscoltoSettingsWindow

- (instancetype)initWithOptions:(NSDictionary* (^)(void))options target:(id)target {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1020, 780)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self = [super initWithWindow:window];
    if (self) {
        _options = [options copy]; _actionTarget = target;
        _provider = @"opencode-zen";
        _category = @"AI providers";
        window.title = @"Ascolto";
        window.delegate = self;
        window.releasedWhenClosed = NO;
        window.contentMinSize = NSMakeSize(900, 540);
        window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        [window center];
        NSView* sidebar = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 190, 780)];
        sidebar.wantsLayer = YES;
        sidebar.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
        _sidebar = sidebar;
        sidebar.autoresizingMask = NSViewHeightSizable;
        [window.contentView addSubview:sidebar];
        NSTextField* brand = [self label:@"Ascolto" size:21 bold:YES];
        brand.frame = NSMakeRect(24, 700, 150, 30); [sidebar addSubview:brand];
        brand.autoresizingMask = NSViewMinYMargin;
        NSArray* categories = @[@"Activity & diagnostics", @"General", @"AI providers", @"Transcription", @"Recording", @"Storage"];
        for (NSUInteger index = 0; index < categories.count; index++) {
            NSButton* button = [self button:categories[index] action:@selector(selectCategory:)];
            button.frame = NSMakeRect(16, 624 - index * 46, 158, 36);
            button.identifier = categories[index];
            button.buttonType = NSButtonTypePushOnPushOff;
            button.autoresizingMask = NSViewMinYMargin;
            [sidebar addSubview:button];
        }
        NSTextField* local = [self label:@"Local transcription\nOn this device" size:11 bold:NO];
        local.textColor = NSColor.secondaryLabelColor;
        local.frame = NSMakeRect(24, 28, 156, 42); [sidebar addSubview:local];
        _page = [[NSView alloc] initWithFrame:NSMakeRect(190, 0, 830, 780)];
        _page.wantsLayer = YES;
        _pageScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(190,0,830,780)];
        _pageScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _pageScroll.hasVerticalScroller = YES; _pageScroll.hasHorizontalScroller = YES;
        _pageScroll.autohidesScrollers = YES;
        _canvas = [[AscoltoSettingsCanvas alloc] initWithFrame:NSMakeRect(0,0,830,780)];
        _pageScroll.documentView = _canvas;
        [_canvas addSubview:_page];
        [window.contentView addSubview:_pageScroll];
        _activity = [[AscoltoActivityViewController alloc] initWithConfig:_options()[@"config"]];
        [self buildPage];
    }
    return self;
}

- (void)layoutSharedWindow {
    NSSize available = _pageScroll.contentView.bounds.size;
    BOOL activity = [_category isEqual:@"Activity & diagnostics"];
    NSSize pageSize = NSMakeSize(MAX(activity ? 680 : 830, available.width), activity ? MAX(400,available.height) : 780);
    _canvas.frame = NSMakeRect(0,0,pageSize.width,MAX(pageSize.height,available.height));
    _page.frame = NSMakeRect(0,0,pageSize.width,pageSize.height);
    if (activity) _activity.view.frame = _page.bounds;
}

- (void)windowDidResize:(NSNotification*)notification { (void)notification; [self layoutSharedWindow]; }
- (void)windowDidEnterFullScreen:(NSNotification*)notification { (void)notification; [self layoutSharedWindow]; }
- (void)windowDidExitFullScreen:(NSNotification*)notification { (void)notification; [self layoutSharedWindow]; }

- (NSTextField*)label:(NSString*)text size:(CGFloat)size bold:(BOOL)bold {
    NSTextField* label = [NSTextField wrappingLabelWithString:text ?: @""];
    label.font = bold ? [NSFont systemFontOfSize:size weight:NSFontWeightSemibold] : [NSFont systemFontOfSize:size];
    return label;
}

- (NSButton*)button:(NSString*)title action:(SEL)action {
    return [NSButton buttonWithTitle:title target:self action:action];
}

- (void)addLabel:(NSString*)text y:(CGFloat)y size:(CGFloat)size bold:(BOOL)bold {
    NSTextField* label = [self label:text size:size bold:bold];
    label.frame = NSMakeRect(30, y, 770, size > 20 ? 38 : 42);
    if (!bold) label.textColor = NSColor.secondaryLabelColor;
    [_page addSubview:label];
}

- (NSButton*)nativeButton:(NSString*)title selector:(SEL)selector y:(CGFloat)y check:(BOOL)check value:(BOOL)value {
    NSButton* button = check ? [NSButton checkboxWithTitle:title target:_actionTarget action:selector] : [NSButton buttonWithTitle:title target:_actionTarget action:selector];
    button.frame = NSMakeRect(30, y, 600, 32); button.state = value;
    [_page addSubview:button]; return button;
}

- (void)addPopup:(NSMenu*)menu label:(NSString*)label y:(CGFloat)y {
    [self addLabel:label y:y+34 size:12 bold:NO];
    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(30, y, 560, 30) pullsDown:NO];
    popup.menu = [menu copy];
    popup.menu.delegate = self;
    popup.identifier = [@{@"Transcription language": @"languageMenu", @"Speaker recognition": @"speakersMenu", @"Microphone": @"micMenu", @"Call audio": @"sysMenu"} objectForKey:label];
    [_nativePopups addObject:popup];
    for (NSMenuItem* item in popup.itemArray) if (item.state == NSControlStateValueOn) [popup selectItem:item];
    [_page addSubview:popup];
}

- (void)selectCategory:(NSButton*)sender {
    _category = sender.identifier;
    _userNavigated = YES;

    [self buildPage];
    if (!_busy) [self request:@{@"action": @"section", @"section":_category}];
}

- (void)buildPage {
    for (NSView* view in _page.subviews.copy) [view removeFromSuperview];
    _nativePopups = [NSMutableArray array];
    _sidebar.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
    for (NSView* view in _sidebar.subviews) if ([view isKindOfClass:NSButton.class]) {
        NSButton* button = (NSButton*)view; button.state = [button.identifier isEqual:_category];
    }
    _localRows = nil;
    _notice = nil; _archiveLocation = nil; _monitoring = nil; _saveAudio = nil; _legacy = nil; _legacyTTS = nil;
    _page.layer.backgroundColor = NSColor.controlBackgroundColor.CGColor;
    [self layoutSharedWindow];
    if ([_category isEqual:@"Activity & diagnostics"]) {
        _activity.view.frame = _page.bounds;
        _activity.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [_page addSubview:_activity.view];
        return;
    }
    [self addLabel:_category y:704 size:25 bold:YES];
    NSDictionary* values = _options();
    if ([_category isEqual:@"AI providers"]) { [self buildProviders]; return; }
    if ([_category isEqual:@"General"]) {
        [self addLabel:@"Choose when Ascolto listens and manage system permissions." y:660 size:13 bold:NO];
        _monitoring = [self nativeButton:@"Automatic monitoring" selector:@selector(onToggleAutoTrigger:) y:596 check:YES value:[values[@"automatic"] boolValue]];
        [self addLabel:@"Mirrors the shortcut in the menu bar. Microphone activity from call apps starts local transcription." y:538 size:12 bold:NO];
        [self addLabel:@"Permissions" y:464 size:17 bold:YES];
        [self nativeButton:@"Microphone privacy settings…" selector:@selector(onMicrophonePrivacy:) y:418 check:NO value:NO];
        [self nativeButton:@"Allow call audio capture…" selector:@selector(onRequestSysAudioPermission:) y:372 check:NO value:NO];
        [self nativeButton:@"Allow Accessibility access…" selector:@selector(onRequestAccessibility:) y:326 check:NO value:NO];
        [self nativeButton:@"Allow Calendar access…" selector:@selector(onRequestCalendar:) y:280 check:NO value:NO];
    } else if ([_category isEqual:@"Transcription"]) {
        NSSegmentedControl* tabs = [NSSegmentedControl segmentedControlWithLabels:@[@"On device", @"Cloud after calls"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(selectTranscriptionTab:)];
        tabs.frame = NSMakeRect(30, 658, 470, 32); tabs.selectedSegment = _transcriptionTab; [_page addSubview:tabs];
        if (_transcriptionTab == 0) {
            [self buildLocalTranscription:values];
            [self refreshNativeValues];
            return;
        }
        [self addLabel:@"Optional cloud work is independent of the model selected on this device." y:600 size:13 bold:NO];
        [self addLabel:@"Optional post-call cloud transcription" y:544 size:17 bold:YES];
        [self addLabel:@"OpenRouter transcription models only. ChatGPT sign-in supports text recaps. Save audio in Recording to upload the complete MIC/SYS timeline." y:494 size:12 bold:NO];
        NSDictionary* preference=_settings[@"transcription"];
        _transcriptionModel=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(30,456,490,30) pullsDown:NO];
        [_transcriptionModel addItemWithTitle:@"Local transcription only"];_transcriptionModel.lastItem.representedObject=@"";
        for(NSDictionary* model in _settings[@"profiles"][@"openrouter"][@"transcription_models"]){[_transcriptionModel addItemWithTitle:model[@"name"] ?: model[@"id"]];_transcriptionModel.lastItem.representedObject=model[@"id"];if([model[@"id"] isEqual:preference[@"model"]])[_transcriptionModel selectItem:_transcriptionModel.lastItem];}
        _transcriptionModel.enabled=!_busy;[_page addSubview:_transcriptionModel];
        NSButton* refresh=[self button:@"Refresh transcription models" action:@selector(refreshTranscription:)];refresh.frame=NSMakeRect(530,456,260,30);refresh.enabled=!_busy;[_page addSubview:refresh];
        _audioConsent=[NSButton checkboxWithTitle:@"Allow uploading saved MIC and SYS audio to the selected OpenRouter model" target:nil action:nil];_audioConsent.frame=NSMakeRect(30,414,740,30);_audioConsent.state=[preference[@"cloud_audio_opt_in"] boolValue];[_page addSubview:_audioConsent];
        _autoTranscription=[NSButton checkboxWithTitle:@"Transcribe complete saved audio automatically after each session" target:nil action:nil];_autoTranscription.frame=NSMakeRect(30,378,740,30);_autoTranscription.state=[preference[@"enabled"] boolValue];[_page addSubview:_autoTranscription];
        NSButton* save=[self button:@"Save cloud preferences" action:@selector(saveTranscription:)];save.frame=NSMakeRect(30,338,240,30);save.enabled=!_busy;[_page addSubview:save];
        NSButton* plan=[self button:@"Preview latest audio upload" action:@selector(planTranscription:)];plan.frame=NSMakeRect(30,294,300,30);plan.enabled=!_busy;[_page addSubview:plan];
        NSButton* transcribe=[self button:@"Transcribe latest saved session" action:@selector(runTranscription:)];transcribe.frame=NSMakeRect(350,294,320,30);transcribe.enabled=!_busy;[_page addSubview:transcribe];
        _notice=[self label:_message ?: @"Cloud transcription is off by default; remote speaker identity stays UNKNOWN without local evidence." size:12 bold:NO];_notice.frame=NSMakeRect(30,228,640,56);[_page addSubview:_notice];
    } else if ([_category isEqual:@"Recording"]) {
        [self addLabel:@"Choose audio sources and optional archives. Changes apply to the next session." y:660 size:13 bold:NO];
        [self addPopup:values[@"micMenu"] label:@"Microphone" y:574];
        [self addPopup:values[@"sysMenu"] label:@"Call audio" y:476];
        _saveAudio = [self nativeButton:@"Save compressed audio (M4A · about 21 MB/hour)" selector:@selector(onToggleSaveAudio:) y:394 check:YES value:[values[@"saveAudio"] boolValue]];
        [self addLabel:@"Legacy cloud tools" y:316 size:17 bold:YES];
        [self addLabel:@"Separate opt-in workflow: uploads audio to OpenAI for transcription, then uses Claude CLI. Requires its existing API key and CLI setup." y:250 size:12 bold:NO];
        _legacy = [self nativeButton:@"Legacy automatic cloud transcription and recap" selector:@selector(onToggleCloudRecap:) y:206 check:YES value:[values[@"cloudRecap"] boolValue]];
        _legacyTTS = [self nativeButton:@"Legacy spoken recap (OpenAI TTS)" selector:@selector(onToggleRecapTTS:) y:164 check:YES value:[values[@"tts"] boolValue]];
        [self nativeButton:@"Generate latest legacy cloud recap" selector:@selector(onRunLegacyRecap:) y:112 check:NO value:NO];
    } else if ([_category isEqual:@"Storage"]) {
        [self addLabel:@"Archive folder" y:624 size:17 bold:YES];
        NSTextField* location = [self label:values[@"output"] size:14 bold:NO];
        _archiveLocation = location;
        location.selectable = YES; location.frame = NSMakeRect(30, 558, 740, 50); [_page addSubview:location];
        NSButton* folder=[self nativeButton:@"Open archive folder…" selector:@selector(onOpenOutputFolder:) y:498 check:NO value:NO];folder.frame=NSMakeRect(30,498,300,32);
        NSButton* change=[self nativeButton:@"Change folder…" selector:@selector(onChangeOutputFolder:) y:498 check:NO value:NO];change.frame=NSMakeRect(350,498,240,32);
        [self addLabel:@"Each call keeps transcript.md and recap.md together, with Audio, Data and Recap-history folders. Folder changes apply to new calls; existing files keep their paths." y:430 size:12 bold:NO];
        NSArray* kinds=@[@"call_directory",@"transcript",@"recap"];
        NSArray* titles=@[@"Open latest call",@"Open transcript",@"Open recap"];
        for(NSUInteger i=0;i<kinds.count;i++){NSButton* open=[self button:titles[i] action:@selector(openArchiveItem:)];open.identifier=kinds[i];open.frame=NSMakeRect(30+i*250,370,240,32);open.enabled=!_busy;[_page addSubview:open];}
        NSButton* provisional=[self button:@"Provisional recap so far" action:@selector(runProvisional:)];provisional.frame=NSMakeRect(30,320,300,32);provisional.enabled=!_busy;[_page addSubview:provisional];
        NSButton* final=[self button:@"Generate final recap of latest session" action:@selector(runFinal:)];final.frame=NSMakeRect(30,274,380,32);final.enabled=!_busy;[_page addSubview:final];
        [self nativeButton:@"View activity…" selector:@selector(onActivity:) y:180 check:NO value:NO];
        NSButton* status=[self button:@"Refresh AI job status" action:@selector(refreshJobStatus:)];status.frame=NSMakeRect(30,224,260,32);status.enabled=!_busy;[_page addSubview:status];
        NSDictionary* job=[_settings[@"jobs"] firstObject];
        NSString* jobText=job ? [NSString stringWithFormat:@"Latest %@: %@ · %@%@",job[@"task"],job[@"status"],job[@"model_label_snapshot"],job[@"error_code"]!=NSNull.null ? [@" · " stringByAppendingString:job[@"error_code"] ?: @""] : @""] : @"No AI jobs yet.";
        [self addLabel:_message ?: jobText y:106 size:12 bold:NO];
    }
    if(_latestAIOutput && [_category isEqual:@"Storage"]){NSButton* open=[self button:@"Open latest AI output…" action:@selector(openAIOutput:)];open.frame=NSMakeRect(320,224,280,32);[_page addSubview:open];}
    if(_busy){NSButton* cancel=[self button:@"Cancel" action:@selector(cancelRequest:)];cancel.frame=NSMakeRect(680,46,90,30);[_page addSubview:cancel];}
    [self refreshNativeValues];
}

- (ascolto_local_models_t*)localModels {
    return [_options()[@"localModels"] pointerValue];
}
- (void)selectTranscriptionTab:(NSSegmentedControl*)sender {
    _transcriptionTab = sender.selectedSegment; [self buildPage];
}
- (void)buildLocalTranscription:(NSDictionary*)values {
    _localRows = [[NSView alloc] initWithFrame:NSMakeRect(30, 254, 770, 394)];
    [_page addSubview:_localRows];
    [self renderLocalModels];
    NSTextField* note = [self label:@"Small Italian sample, automatic reference; not certified accuracy. Whisper shares the reference model family." size:11 bold:NO];
    note.textColor = NSColor.secondaryLabelColor; note.frame = NSMakeRect(30, 210, 600, 36); [_page addSubview:note];
    NSButton* comparison = [self button:@"View comparison…" action:@selector(showLocalComparison:)]; comparison.frame = NSMakeRect(640, 216, 160, 30); [_page addSubview:comparison];
    [self addPopup:values[@"languageMenu"] label:@"Transcription language" y:152];
    _page.subviews[_page.subviews.count-2].frame = NSMakeRect(30,188,350,20);
    NSPopUpButton* language = _nativePopups.lastObject; language.frame = NSMakeRect(30, 152, 350, 30);
    [self addPopup:values[@"speakersMenu"] label:@"Speaker recognition" y:88];
    _page.subviews[_page.subviews.count-2].frame = NSMakeRect(30,124,350,20);
    NSPopUpButton* speakers = _nativePopups.lastObject; speakers.frame = NSMakeRect(30, 88, 350, 30);
    NSTextField* title = [self label:@"Custom keywords (comma separated)" size:12 bold:NO]; title.frame=NSMakeRect(410,186,380,28); [_page addSubview:title];
    _keywords = [NSTextField textFieldWithString:_settings[@"keywords"] ?: @""];
    _keywords.frame=NSMakeRect(410,152,380,28); [_page addSubview:_keywords];
    NSButton* save=[self button:@"Save keywords" action:@selector(saveKeywords:)];save.frame=NSMakeRect(410,88,160,30);[_page addSubview:save];
    _notice=[self label:_message ?: @"Downloads and recognition stay on this device. Choosing a model does not enable cloud audio upload." size:12 bold:NO];_notice.frame=NSMakeRect(30,22,740,48);[_page addSubview:_notice];
}
- (void)renderLocalModels {
    if (!_localRows || _renderingLocal) return;
    _renderingLocal = YES;
    for (NSView* view in _localRows.subviews.copy) [view removeFromSuperview];
    ascolto_local_models_snapshot_t snapshot;
    ascolto_local_models_snapshot([self localModels], &snapshot);
    NSData* fixture = _options()[@"localModelsFixture"];
    if (fixture.length == sizeof(snapshot)) memcpy(&snapshot, fixture.bytes, sizeof(snapshot));
    _localRevision = snapshot.revision;
    _lastLocalBusy = [_options()[@"busy"] boolValue];
    NSString* selected=@(snapshot.selected);NSString* active=@(snapshot.active);
    for(int i=0;i<4;i++){if(!strcmp(snapshot.models[i].id,snapshot.selected))selected=@(snapshot.models[i].name);if(!strcmp(snapshot.models[i].id,snapshot.active))active=@(snapshot.models[i].name);}
    NSString* status = snapshot.active[0] ? [NSString stringWithFormat:@"Recording with %@ · Next session: %@", active, selected] : [NSString stringWithFormat:@"Next session: %@ · %.2f GB available", selected, snapshot.available_bytes / 1e9];
    if (snapshot.error[0]) status = @(snapshot.error);
    NSTextField* header=[self label:status size:12 bold:NO]; header.frame=NSMakeRect(0,356,765,38);header.textColor=snapshot.error[0]?NSColor.systemRedColor:NSColor.secondaryLabelColor;[_localRows addSubview:header];
    for (int i=0;i<ASCOLTO_LOCAL_MODEL_COUNT;i++) {
        ascolto_model_snapshot_t* model=&snapshot.models[i];
        if (!model->id[0]) continue;
        CGFloat y=266-i*88;
        NSTextField* name=[self label:@(model->name) size:14 bold:YES];name.frame=NSMakeRect(0,y+58,580,24);[_localRows addSubview:name];
        NSTextField* description=[self label:@(model->description) size:12 bold:NO];description.frame=NSMakeRect(0,y+30,590,30);description.textColor=NSColor.secondaryLabelColor;[_localRows addSubview:description];
        NSString* detail;
        if (model->state==ASCOLTO_MODEL_DOWNLOADING) detail=[NSString stringWithFormat:@"Downloading %.1f / %.1f MB",model->transferred/1e6,model->bytes/1e6];
        else if (model->state==ASCOLTO_MODEL_VERIFYING) detail=@"Verifying download…";
        else if (model->state==ASCOLTO_MODEL_CHECKING) detail=@"Checking model files…";
        else if (model->message[0]) detail=@(model->message);
        else detail=model->state==ASCOLTO_MODEL_READY?@"Downloaded · verified":model->state==ASCOLTO_MODEL_MISSING?@"Not downloaded":@(model->hint);
        if(model->bytes && (model->state==ASCOLTO_MODEL_MISSING || model->state==ASCOLTO_MODEL_READY)) detail=[NSString stringWithFormat:@"%@ · %@ · %.1f MB download",detail,@(model->hint),model->bytes/1e6];
        if(i==2 && model->selected)detail=[detail stringByAppendingString:@" · Automatic language; keyword prompts unavailable"];
        if(model->selected)detail=[@"Selected · " stringByAppendingString:detail];
        NSTextField* state=[self label:detail size:11 bold:NO];state.frame=NSMakeRect(0,y+5,590,27);[_localRows addSubview:state];
        if(model->state==ASCOLTO_MODEL_DOWNLOADING){NSProgressIndicator* progress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0,y+1,580,5)];progress.indeterminate=NO;progress.maxValue=model->bytes;progress.doubleValue=model->transferred;[_localRows addSubview:progress];}
        NSString* action;NSString* title;
        if(model->state==ASCOLTO_MODEL_INCLUDED || model->state==ASCOLTO_MODEL_READY){action=@"use";title=model->selected?@"Selected":snapshot.active[0]?@"Use next session":@"Use model";}
        else if(model->state==ASCOLTO_MODEL_DOWNLOADING || model->state==ASCOLTO_MODEL_VERIFYING){action=@"cancel";title=@"Cancel";}
        else {action=@"download";title=model->state==ASCOLTO_MODEL_ERROR?@"Retry download":@"Download";}
        NSButton* button=[self button:title action:@selector(localModelAction:)];button.identifier=[NSString stringWithFormat:@"%@:%s",action,model->id];button.frame=NSMakeRect(610,y+46,155,30);button.enabled=![_options()[@"busy"] boolValue] && (!model->selected || ![action isEqual:@"use"]) && model->state!=ASCOLTO_MODEL_CHECKING && model->state!=ASCOLTO_MODEL_UNSUPPORTED;
        if([action isEqual:@"cancel"])button.enabled=YES;
        button.accessibilityLabel=[NSString stringWithFormat:@"%@: %@",title,@(model->name)];
        [_localRows addSubview:button];
        if(i && !model->selected && !model->active && (model->state==ASCOLTO_MODEL_READY || model->state==ASCOLTO_MODEL_ERROR)){NSButton* remove=[self button:@"Remove…" action:@selector(localModelAction:)];remove.identifier=[@"remove:" stringByAppendingString:@(model->id)];remove.accessibilityLabel=[NSString stringWithFormat:@"Remove %@",@(model->name)];remove.frame=NSMakeRect(610,y+10,155,28);remove.enabled=![_options()[@"busy"] boolValue];[_localRows addSubview:remove];}
    }
    _renderingLocal = NO;
}
- (void)localModelAction:(NSButton*)sender {
    NSArray* parts=[sender.identifier componentsSeparatedByString:@":"];if(parts.count!=2)return;
    NSString* action=parts[0];NSString* model=parts[1];
    ascolto_local_models_t* manager=[self localModels];if(!manager)return;
    if([action isEqual:@"cancel"]){ascolto_local_models_cancel(manager,model.UTF8String);return;}
    void (^perform)(NSString*, NSString*) = _options()[@"localModelAction"];
    if (perform) perform(action, model);

}
- (void)showLocalComparison:(id)sender {
    (void)sender;
    NSAlert* alert=[[NSAlert alloc] init];alert.messageText=@"Local model comparison";
    alert.informativeText=@"These descriptions summarize a small Italian call sample against an automatic cloud reference, not human ground truth or certified accuracy. Whisper is in the same model family as the reference. Processing delay, recognition agreement and memory use are different measurements. Whistle is the included lightweight baseline; Whisper had the closest reference agreement, Parakeet had very low processing delay, and Qwen used more memory. Language and custom keywords depend on the selected engine; Parakeet detects language automatically.";
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

- (void)buildProviders {
    [self addLabel:@"Choose how Ascolto generates your recaps. Only transcript text is sent." y:662 size:13 bold:NO];
    NSArray* names = @[@"OpenCode\nZen / Go · API key", @"OpenRouter\nAPI key", @"OpenAI\nYour ChatGPT plan"];
    NSArray* ids = @[@"opencode", @"openrouter", @"openai"];
    for (NSUInteger index = 0; index < 3; index++) {
        NSString* key = ids[index];
        BOOL selected = [_settings[@"selected_provider"] hasPrefix:key];
        BOOL connectedCard = [key isEqual:@"opencode"] ? ([_settings[@"profiles"][@"opencode-zen"][@"connected"] boolValue] || [_settings[@"profiles"][@"opencode-go"][@"connected"] boolValue]) : [_settings[@"profiles"][key][@"connected"] boolValue];
        NSString* title = [names[index] stringByAppendingFormat:@"\n%@", selected ? @"✓ Selected for recaps" : connectedCard ? @"Connected" : @"Not connected"];
        NSButton* card = [self button:title action:@selector(selectProvider:)];
        card.buttonType = NSButtonTypePushOnPushOff;
        card.frame = NSMakeRect(30 + index * 260, 570, 246, 76);
        card.identifier = ids[index];
        card.state = [_provider hasPrefix:ids[index]] ? NSControlStateValueOn : NSControlStateValueOff;
        card.bezelStyle = NSBezelStyleRegularSquare;
        [_page addSubview:card];
    }
    NSDictionary* profile = _settings[@"profiles"][_provider] ?: @{};
    BOOL openai = [_provider isEqual:@"openai"], opencode = [_provider hasPrefix:@"opencode"];
    BOOL connected = [profile[@"connected"] boolValue];
    [self addLabel:openai ? @"Connect OpenAI" : opencode ? @"Connect OpenCode" : @"Connect OpenRouter" y:516 size:19 bold:YES];
    if (opencode) {
        NSSegmentedControl* plan = [NSSegmentedControl segmentedControlWithLabels:@[@"Zen · pay as you go", @"Go · subscription"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(selectPlan:)];
        plan.frame = NSMakeRect(30, 476, 450, 30); plan.selectedSegment = [_provider isEqual:@"opencode-go"] ? 1 : 0; plan.enabled = !_busy; [_page addSubview:plan];
    }
    if (openai) {
        _account = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(30, 470, 500, 30) pullsDown:NO];
        [_account addItemWithTitle:@"Add another ChatGPT account"];
        _account.lastItem.representedObject = @"";
        for (NSDictionary* account in _settings[@"accounts"]) {
            [_account addItemWithTitle:account[@"label"]]; _account.lastItem.representedObject = account[@"id"];
            if ([account[@"id"] isEqual:profile[@"account_id"]]) [_account selectItem:_account.lastItem];
        }
        _account.enabled = !_busy; [_page addSubview:_account];
        _connect = [AscoltoChatGPTButton buttonWithTitle:@"Continue with ChatGPT" target:self action:@selector(signIn:)];
        _connect.bordered = NO;
        _connect.frame = NSMakeRect(30, 425, 290, 32);
        NSString* logoPath = [[NSBundle mainBundle] pathForResource:@"chatgpt-logo" ofType:@"svg"];
        if (!logoPath) {
            NSString* executable = [NSProcessInfo processInfo].arguments.firstObject;
            logoPath = [[executable stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"../assets/chatgpt-logo.svg"];
        }
        NSImage* logo = [[NSImage alloc] initWithContentsOfFile:logoPath];
        logo.size = NSMakeSize(20, 20); logo.template = NO;
        _connect.image = logo; _connect.imagePosition = NSImageLeft;
        [_page addSubview:_connect];
        NSButton* usage = [self button:@"Manage usage ↗" action:@selector(manageUsage:)];
        usage.frame = NSMakeRect(552, 425, 218, 32); [_page addSubview:usage];
        [self addLabel:connected ? [@"Using ChatGPT plan · " stringByAppendingString:profile[@"account_label"] ?: @""] : @"Use an eligible personal ChatGPT plan (Plus / Pro). Your browser opens for secure sign-in." y:374 size:12 bold:NO];
    } else {
        NSButton* getKey = [self button:@"Get an API key ↗" action:@selector(getAPIKey:)];
        getKey.frame = NSMakeRect(opencode ? 490 : 30, 478, opencode ? 130 : 210, 30); [_page addSubview:getKey];
        _secureKey = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(30, 424, 470, 30)];
        _plainKey = [[NSTextField alloc] initWithFrame:_secureKey.frame];
        _secureKey.placeholderString = connected ? @"Enter a replacement API key" : @"Paste your API key";
        _plainKey.placeholderString = _secureKey.placeholderString; _plainKey.hidden = YES;
        [_page addSubview:_secureKey]; [_page addSubview:_plainKey];
        NSButton* reveal = [NSButton checkboxWithTitle:@"Show" target:self action:@selector(revealKey:)];
        reveal.frame = NSMakeRect(506, 424, 70, 30); [_page addSubview:reveal];
        _connect = [self button:connected ? @"Verify replacement" : @"Verify & connect" action:@selector(connectKey:)];
        _connect.frame = NSMakeRect(580, 424, 190, 32); [_page addSubview:_connect];
        [self addLabel:connected ? @"Connected · Key stored in Keychain. An invalid replacement keeps the existing connection." : @"Your key is masked and stored in the system credential manager." y:378 size:12 bold:NO];
        if ([_provider isEqual:@"opencode-go"]) [self addLabel:@"Go is intended for coding agents. Confirm provider support before using it for call recaps." y:332 size:12 bold:NO];
    }
    NSButton* disconnect = [self button:@"Disconnect…" action:@selector(disconnectProvider:)];
    disconnect.frame = NSMakeRect(622, 478, 148, 30); disconnect.enabled = connected && !_busy; [_page addSubview:disconnect];
    [self addLabel:@"Recap preferences" y:284 size:17 bold:YES];
    _model = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(30, 242, 465, 30) pullsDown:NO];
    [_model addItemWithTitle:connected ? @"Choose a model" : @"Connect a provider to load models"];
    _model.lastItem.representedObject = @"";
    for (NSDictionary* model in profile[@"models"]) {
        [_model addItemWithTitle:model[@"name"]]; _model.lastItem.representedObject = model[@"id"];
        if ([model[@"id"] isEqual:profile[@"model"]]) [_model selectItem:_model.lastItem];
    }
    _model.enabled = connected && !_busy; _model.target = self; _model.action = @selector(modelChanged:); [_page addSubview:_model];
    NSButton* refresh = [self button:@"Refresh" action:@selector(refreshModels:)];
    refresh.frame = NSMakeRect(500, 242, 88, 30); refresh.enabled = connected && !_busy; [_page addSubview:refresh];
    _language = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(600, 242, 170, 30) pullsDown:NO];
    [_language addItemsWithTitles:@[@"Italian", @"English", @"German", @"French", @"Spanish", @"Dutch", @"Polish"]];
    [_language selectItemWithTitle:_settings[@"language"] ?: @"Italian"];
    _language.target = self; _language.action = @selector(updatePreferences:); _language.enabled = !_busy; [_page addSubview:_language];
    _useProvider = [self button:@"Use for recaps" action:@selector(useForRecaps:)];
    _useProvider.frame = NSMakeRect(30, 194, 210, 32); _useProvider.enabled = connected && !_busy && [_model.selectedItem.representedObject length]; [_page addSubview:_useProvider];
    NSString* selected = _settings[@"selected_provider"] ?: @"";
    [self addLabel:[selected isEqual:_provider] ? @"Selected for recaps" : selected.length ? [@"Current recap provider: " stringByAppendingString:selected] : @"No provider selected. Recaps are off by default." y:148 size:12 bold:NO];
    _automatic = [NSButton checkboxWithTitle:@"Generate a recap after each session" target:self action:@selector(updatePreferences:)];
    _automatic.frame = NSMakeRect(30, 110, 600, 30); _automatic.state = [_settings[@"automatic_recaps"] boolValue];
    _automatic.enabled = selected.length && !_busy; [_page addSubview:_automatic];
    NSString* help = [_settings[@"runtime_ready"] boolValue] ? @"Only transcript text is sent. On-device transcription and audio archives are unchanged." : @"AI dependencies are missing. Run make recap-deps and rebuild the app bundle.";
    _notice = [self label:_message ?: _settings[@"secure_store_message"] ?: help size:12 bold:NO];
    _notice.textColor = NSColor.secondaryLabelColor; _notice.frame = NSMakeRect(30, 40, 640, 62); [_page addSubview:_notice];
    NSButton* cancel = [self button:@"Cancel" action:@selector(cancelRequest:)]; cancel.frame = NSMakeRect(680, 48, 90, 30); cancel.hidden = !_busy; [_page addSubview:cancel];
    _connect.enabled = !_busy;
}

- (void)selectProvider:(NSButton*)sender {
    if (_busy) return;
    _provider = [sender.identifier isEqual:@"opencode"] ? @"opencode-zen" : sender.identifier;
    _message = nil; [self buildPage];
}
- (void)selectPlan:(NSSegmentedControl*)sender { _provider = sender.selectedSegment ? @"opencode-go" : @"opencode-zen"; _message = nil; [self buildPage]; }
- (void)revealKey:(NSButton*)sender {
    BOOL show = sender.state == NSControlStateValueOn;
    if (show) _plainKey.stringValue = _secureKey.stringValue; else _secureKey.stringValue = _plainKey.stringValue;
    _secureKey.hidden = show; _plainKey.hidden = !show;
}
- (void)connectKey:(id)sender {
    (void)sender;
    NSString* key = _secureKey.hidden ? _plainKey.stringValue : _secureKey.stringValue;
    [self request:@{@"action": @"connect", @"provider": _provider, @"key": key}];
    _secureKey.stringValue = @""; _plainKey.stringValue = @"";
}
- (void)signIn:(id)sender {
    (void)sender;
    NSMutableDictionary* request = [@{@"action": @"sign_in", @"provider": @"openai"} mutableCopy];
    NSString* account = _account.selectedItem.representedObject;
    if (account.length) request[@"account_id"] = account;
    [self request:request];
}
- (void)refreshModels:(id)sender { (void)sender; [self request:@{@"action": @"refresh_models", @"provider": _provider}]; }
- (void)useForRecaps:(id)sender {
    (void)sender;
    if ([_provider isEqual:@"opencode-go"]) {
        NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = @"Use OpenCode Go for call recaps?";
        alert.informativeText = @"Go is intended for coding agents. Confirm provider support before using it for call recaps. Ascolto identifies itself as a call recap app.";
        [alert addButtonWithTitle:@"Use for recaps"]; [alert addButtonWithTitle:@"Cancel"];
        if ([alert runModal] != NSAlertFirstButtonReturn) return;
    }
    [self request:@{@"action": @"use", @"provider": _provider, @"model": _model.selectedItem.representedObject ?: @"", @"acknowledge_go_policy": @([_provider isEqual:@"opencode-go"])}];
}
- (void)updatePreferences:(id)sender {
    (void)sender;
    [self request:@{@"action": @"preferences", @"language": _language.titleOfSelectedItem ?: @"Italian", @"automatic_recaps": @(_automatic.state == NSControlStateValueOn)}];
}
- (void)modelChanged:(id)sender { (void)sender; _useProvider.enabled = !_busy && [_model.selectedItem.representedObject length]; }
- (void)getAPIKey:(id)sender {
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:[_provider hasPrefix:@"opencode"] ? @"https://opencode.ai/console" : @"https://openrouter.ai/settings/keys"]];
}
- (void)manageUsage:(id)sender { (void)sender; [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://chatgpt.com/settings/usage"]]; }
- (void)disconnectProvider:(id)sender {
    (void)sender;
    NSAlert* alert = [[NSAlert alloc] init]; alert.messageText = @"Disconnect this provider?";
    alert.informativeText = @"This removes the local credential and turns off its automatic recaps. Transcripts and audio archives are kept.";
    [alert addButtonWithTitle:@"Disconnect"]; [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] == NSAlertFirstButtonReturn) [self request:@{@"action": @"disconnect", @"provider": _provider}];
}

- (void)request:(NSDictionary*)request {
    if (_busy) return;
    char script[2300], python[2300];
    if (ascolto_ai_worker_paths("ai_providers.py", script, sizeof(script), python, sizeof(python))) {
        _message = @"AI settings worker is missing. Rebuild the app bundle."; [self buildPage]; return;
    }
    _busy = YES;
    _message = [@[@"recap",@"provisional_recap",@"transcription",@"plan_transcription"] containsObject:request[@"action"]] ? @"Running the selected AI task…" : [request[@"action"] isEqual:@"sign_in"] ? @"Waiting for your browser… Complete sign-in, or cancel here to keep your existing connection." : @"Updating provider settings…";
    NSData* input = [NSJSONSerialization dataWithJSONObject:request options:0 error:NULL];
    NSString* config = _options()[@"config"];
    NSTask* task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/env"];
    task.arguments = @[@"PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin", @"PYTHONDONTWRITEBYTECODE=1", @(python), @"-B", @(script), @"--config", config];
    NSPipe* inPipe = [NSPipe pipe]; NSPipe* outPipe = [NSPipe pipe];
    task.standardInput = inPipe; task.standardOutput = outPipe; task.standardError = [NSFileHandle fileHandleWithNullDevice];
    _worker = task;
    [self buildPage];
    __weak AscoltoSettingsWindow* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError* error;
        BOOL launched = weakSelf.worker == task && [task launchAndReturnError:&error];
        if (launched && weakSelf.worker != task) [task terminate];
        NSData* output = nil;
        if (launched) {
            @try { [inPipe.fileHandleForWriting writeData:input]; [inPipe.fileHandleForWriting closeFile]; output = [outPipe.fileHandleForReading readDataToEndOfFile]; [task waitUntilExit]; }
            @catch (NSException* exception) { (void)exception; }
        }
        NSDictionary* result = output ? [NSJSONSerialization JSONObjectWithData:output options:0 error:NULL] : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            AscoltoSettingsWindow* strongSelf = weakSelf;
            if (!strongSelf || strongSelf.worker != task) return;
            strongSelf.worker = nil; strongSelf.busy = NO;
            if ([result[@"ok"] boolValue]) {
                NSString* openPath=result[@"result"][@"open_path"];if([openPath isKindOfClass:NSString.class])[[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:openPath]];
                if(!strongSelf.settings && !strongSelf.userNavigated){NSString* saved=result[@"settings"][@"settings_section"];if([@[@"Activity & diagnostics",@"General",@"AI providers",@"Transcription",@"Recording",@"Storage"] containsObject:saved])strongSelf.category=saved;}
                strongSelf.settings = result[@"settings"];
                if([result[@"result"][@"session_dir"] isKindOfClass:NSString.class])strongSelf.latestAIOutput=result[@"result"][@"session_dir"];
                if ([request[@"action"] isEqual:@"use"] || ([request[@"action"] isEqual:@"preferences"] && [request[@"automatic_recaps"] boolValue]))
                    [NSApp sendAction:@selector(onSelectAIRecap:) to:strongSelf.actionTarget from:strongSelf];
                strongSelf.message = [result[@"message"] length] ? result[@"message"] : nil;
            } else strongSelf.message = result[@"message"] ?: @"The AI worker could not start. Check your Python installation and run make recap-deps.";
            [strongSelf buildPage];
            if(strongSelf.pendingTask && !strongSelf.closing){NSString* pending=strongSelf.pendingTask;strongSelf.pendingTask=nil;[strongSelf request:@{@"action":pending}];return;}
            if ([strongSelf.settings[@"profiles"][@"openai"][@"welcome_pending"] boolValue] && !strongSelf.closing) {
                NSAlert* welcome = [[NSAlert alloc] init]; welcome.messageText = @"You're using your ChatGPT plan";
                welcome.informativeText = @"Eligible AI requests use your ChatGPT plan. Manage usage in ChatGPT settings. Ascolto sends only transcript text for recaps after your explicit selection.";
                [welcome addButtonWithTitle:@"Got it"]; [welcome runModal];
                [strongSelf request:@{@"action": @"acknowledge_plan"}];
            }
        });
    });
}

- (void)openAIOutput:(id)sender { (void)sender; if(_latestAIOutput)[[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:_latestAIOutput]]; }
- (void)saveKeywords:(id)sender { (void)sender; [self request:@{@"action":@"keywords",@"text":_keywords.stringValue ?: @""}]; }
- (void)refreshTranscription:(id)sender { (void)sender; [self request:@{@"action":@"refresh_transcription_models"}]; }
- (void)saveTranscription:(id)sender { (void)sender; [self request:@{@"action":@"transcription_preferences",@"provider":@"openrouter",@"model":_transcriptionModel.selectedItem.representedObject ?: @"",@"cloud_audio_opt_in":@(_audioConsent.state==NSControlStateValueOn),@"enabled":@(_autoTranscription.state==NSControlStateValueOn)}]; }
- (void)planTranscription:(id)sender { (void)sender; [self request:@{@"action":@"plan_transcription"}]; }
- (void)runTranscription:(id)sender { (void)sender; [self request:@{@"action":@"transcription"}]; }
- (void)runProvisional:(id)sender { (void)sender; [self request:@{@"action":@"provisional_recap"}]; }
- (void)runFinal:(id)sender { (void)sender; [self request:@{@"action":@"recap"}]; }
- (void)refreshJobStatus:(id)sender { (void)sender; [self request:@{@"action":@"status"}]; }
- (void)openArchiveItem:(NSButton*)sender { [self request:@{@"action":@"archive_path",@"kind":sender.identifier}]; }

- (void)performAITask:(NSString*)action {
    _closing=NO; _category=@"Storage";
    [self presentMainWindow];
    if(_busy){_pendingTask=action;[self buildPage];}else [self request:@{@"action":action}];
}
- (void)cancelRequest:(id)sender { (void)sender; [self cancelWorker]; _message = @"Cancelled. Your existing connection has been kept."; [self buildPage]; }
- (void)cancelWorker { _pendingTask=nil; NSTask* task = _worker; _worker = nil; _busy = NO; if (task.running) [task terminate]; }
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification; _closing = YES; [self cancelWorker];
    _secureKey.stringValue = @""; _plainKey.stringValue = @"";
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}
- (void)presentMainWindow {
    // Remain a regular app while the window is open, including when inactive,
    // hidden or minimized, so the Dock and Command-Tab can restore it.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    ascolto_apply_application_icon();
    if (self.window.miniaturized) [self.window deminiaturize:nil];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    ascolto_apply_application_icon();
}
- (void)showSettings {
    _closing = NO; [self buildPage]; [self presentMainWindow];
    if (!_settings && !_busy) [self request:@{@"action": @"status"}];
}
- (void)showActivity {
    _closing=NO; _userNavigated=YES; _category=@"Activity & diagnostics";
    [self buildPage];
    [self presentMainWindow];
    [_activity showActivity];
}
- (NSInteger)attentionCount { return _activity.attentionCount; }
- (void)stopActivityRefresh { [_activity stopRefreshing]; }
- (void)menuWillOpen:(NSMenu*)menu { (void)menu; _nativeMenuTracking = YES; }
- (void)menuDidClose:(NSMenu*)menu { (void)menu; _nativeMenuTracking = NO; }
- (void)refreshNativeValues {
    NSDictionary* values = _options();
    if (_localRows) {
        ascolto_local_models_snapshot_t snapshot; ascolto_local_models_snapshot([self localModels], &snapshot);
        if (snapshot.revision != _localRevision || _lastLocalBusy != [values[@"busy"] boolValue]) [self renderLocalModels];
    }
    _archiveLocation.stringValue = values[@"output"] ?: @"";
    [self.window.effectiveAppearance performAsCurrentDrawingAppearance:^{
        self.sidebar.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
        self.page.layer.backgroundColor = NSColor.controlBackgroundColor.CGColor;
    }];
    _monitoring.state = [values[@"automatic"] boolValue]; _saveAudio.state = [values[@"saveAudio"] boolValue];
    _legacy.state = [values[@"cloudRecap"] boolValue]; _legacyTTS.state = [values[@"tts"] boolValue];
    if (!_nativeMenuTracking) {
        for (NSPopUpButton* popup in _nativePopups) {
            NSMenu* menu = values[popup.identifier];
            if (!menu) continue;
            popup.menu = [menu copy]; popup.menu.delegate = self;
            for (NSMenuItem* item in popup.itemArray) if (item.state == NSControlStateValueOn) [popup selectItem:item];
        }
    }
    BOOL enabled = ![values[@"busy"] boolValue];
    _monitoring.enabled = enabled; _saveAudio.enabled = enabled; _legacy.enabled = enabled; _legacyTTS.enabled = enabled;
}
@end
