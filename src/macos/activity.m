#import "activity.h"
#include "ascolto/ai_settings.h"
#include "ascolto/retranscribe.h"

@interface AscoltoActivityPreparation : NSObject
@property (atomic) BOOL cancelled;
@end
@implementation AscoltoActivityPreparation
@end
static bool preparationCancelled(void* context) { return [(__bridge AscoltoActivityPreparation*)context cancelled]; }

@interface AscoltoActivityButton : NSButton
@property id representedObject;
@end
@implementation AscoltoActivityButton
@end

@interface AscoltoActivityBackground : NSView
@end
@implementation AscoltoActivityBackground
- (BOOL)isOpaque { return YES; }
- (void)drawRect:(NSRect)dirtyRect { [NSColor.windowBackgroundColor setFill]; NSRectFill(NSIntersectionRect(dirtyRect,self.bounds)); }
@end

@interface AscoltoActivityDocument : NSView
@end
@implementation AscoltoActivityDocument
- (BOOL)isFlipped { return YES; }
@end

@interface AscoltoActivityViewController ()
@property NSString* config;
@property NSDictionary* snapshot;
@property NSNumber* selectedID;
@property NSStackView* calls;
@property NSStackView* detail;
@property NSSegmentedControl* filter;
@property NSSegmentedControl* tabs;
@property NSTextField* notice;
@property NSButton* refreshButton;
@property NSStackView* split;
@property NSScrollView* detailsScroll;
@property NSTimer* timer;
@property NSTask* reader;
@property NSTask* mutation;
@property NSTask* control;
@property NSString* message;
@property NSString* previewReport;
@property NSDate* lastRead;
@property NSInteger attentionCount;
@property ascolto_local_models_t* localModels;
@property NSData* localModelsFixture;
@property NSMenu* localLanguageMenu;
@property (copy) void (^manageModels)(void);
@property AscoltoActivityPreparation* localPreparation;
@property NSString* localMessage;
@property NSNumber* localMessageCallID;
@property NSAlert* transcriptionSheet;
@property NSPopUpButton* retranscriptionModels;
@property NSSegmentedControl* retranscriptionPrecision;
@end

@implementation AscoltoActivityViewController
- (instancetype)initWithConfig:(NSString*)config {
    self = [super initWithNibName:nil bundle:nil];
    if (!self) return nil;
    _config = [config copy];
    self.view = [[AscoltoActivityBackground alloc] initWithFrame:NSMakeRect(0,0,830,780)];
    NSView* content = self.view;
    NSStackView* layout=[[NSStackView alloc] initWithFrame:NSZeroRect]; layout.orientation=NSUserInterfaceLayoutOrientationVertical; layout.alignment=NSLayoutAttributeLeading; layout.spacing=12; layout.translatesAutoresizingMaskIntoConstraints=NO; [content addSubview:layout];
    [NSLayoutConstraint activateConstraints:@[[layout.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16], [layout.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16], [layout.topAnchor constraintEqualToAnchor:content.topAnchor constant:16], [layout.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16]]];
    NSStackView* header=[[NSStackView alloc] initWithFrame:NSZeroRect]; header.wantsLayer=YES; header.orientation=NSUserInterfaceLayoutOrientationHorizontal; header.spacing=20;
    _filter = [NSSegmentedControl segmentedControlWithLabels:@[@"All calls",@"Needs attention"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(changeFilter:)]; _filter.selectedSegment=0; [header addArrangedSubview:_filter]; NSView* spacer=[[NSView alloc] initWithFrame:NSZeroRect]; [header addArrangedSubview:spacer]; [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSButton* refresh=[NSButton buttonWithTitle:@"Refresh" target:self action:@selector(refresh:)]; _refreshButton=refresh; [header addArrangedSubview:refresh]; [layout addArrangedSubview:header]; [header.widthAnchor constraintEqualToAnchor:layout.widthAnchor].active=YES;
    _notice=[NSTextField labelWithString:@"Loading recorded activity…"]; [layout addArrangedSubview:_notice];
    NSStackView* split=[[NSStackView alloc] initWithFrame:NSMakeRect(0,0,1000,600)]; _split=split; split.orientation=NSUserInterfaceLayoutOrientationHorizontal; split.alignment=NSLayoutAttributeTop; split.spacing=12; [layout addArrangedSubview:split];
    [split.widthAnchor constraintEqualToAnchor:layout.widthAnchor].active=YES;
    [split setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [split setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    NSScrollView* callScroll=[self scrollWithStack:&_calls]; callScroll.wantsLayer=YES; callScroll.frame=NSMakeRect(0,0,280,600); [split addArrangedSubview:callScroll]; [callScroll.widthAnchor constraintEqualToConstant:260].active=YES; [callScroll.heightAnchor constraintEqualToAnchor:split.heightAnchor].active=YES;
    NSView* right=[[AscoltoActivityBackground alloc] initWithFrame:NSMakeRect(0,0,720,600)]; right.wantsLayer=YES; [split addArrangedSubview:right]; [right.heightAnchor constraintEqualToAnchor:split.heightAnchor].active=YES;
    _tabs=[NSSegmentedControl segmentedControlWithLabels:@[@"Overview",@"Technical details",@"Attempts"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(changeTab:)]; _tabs.selectedSegment=0; _tabs.translatesAutoresizingMaskIntoConstraints=NO; [right addSubview:_tabs];
    NSScrollView* details=[self scrollWithStack:&_detail]; _detailsScroll=details; details.translatesAutoresizingMaskIntoConstraints=NO; [right addSubview:details];
    [NSLayoutConstraint activateConstraints:@[[_tabs.leadingAnchor constraintEqualToAnchor:right.leadingAnchor constant:12], [_tabs.topAnchor constraintEqualToAnchor:right.topAnchor constant:4], [_tabs.trailingAnchor constraintLessThanOrEqualToAnchor:right.trailingAnchor constant:-8], [details.leadingAnchor constraintEqualToAnchor:right.leadingAnchor constant:8], [details.trailingAnchor constraintEqualToAnchor:right.trailingAnchor], [details.topAnchor constraintEqualToAnchor:_tabs.bottomAnchor constant:12], [details.bottomAnchor constraintEqualToAnchor:right.bottomAnchor]]];
    _timer = [NSTimer timerWithTimeInterval:3 target:self selector:@selector(refresh:) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    [self refresh:nil]; return self;
}
- (NSScrollView*)scrollWithStack:(NSStackView* __strong*)destination {
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect]; scroll.hasVerticalScroller=YES;
    AscoltoActivityDocument* document = [[AscoltoActivityDocument alloc] initWithFrame:NSZeroRect]; document.translatesAutoresizingMaskIntoConstraints=NO; scroll.documentView=document;
    NSStackView* stack = [[NSStackView alloc] initWithFrame:NSZeroRect]; stack.orientation=NSUserInterfaceLayoutOrientationVertical;
    stack.alignment=NSLayoutAttributeLeading; stack.spacing=12; stack.edgeInsets=NSEdgeInsetsMake(16,12,20,12); stack.translatesAutoresizingMaskIntoConstraints=NO;
    [document addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[[document.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:document.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:document.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:document.topAnchor], [stack.bottomAnchor constraintEqualToAnchor:document.bottomAnchor]]];
    *destination=stack; return scroll;
}
- (void)clear:(NSStackView*)stack { for(NSView* view in stack.arrangedSubviews.copy) { [stack removeArrangedSubview:view]; [view removeFromSuperview]; } }
- (void)text:(NSString*)text stack:(NSStackView*)stack heading:(BOOL)heading {
    NSTextField* label=[NSTextField wrappingLabelWithString:text ?: @""];
    label.font=heading?[NSFont systemFontOfSize:17 weight:NSFontWeightSemibold]:[NSFont systemFontOfSize:13];
    label.selectable=YES; [stack addArrangedSubview:label];
    [label.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-24].active=YES;
}
- (NSButton*)button:(NSString*)title action:(SEL)action value:(id)value stack:(NSStackView*)stack {
    AscoltoActivityButton* button=[AscoltoActivityButton buttonWithTitle:title target:self action:action]; button.representedObject=value;
    [stack addArrangedSubview:button]; return button;
}
- (NSString*)statusLabel:(NSString*)status {
    NSDictionary* labels=@{@"needs_attention":@"Needs attention",@"not_requested":@"Not requested",@"ready":@"Ready",@"completed":@"Completed",@"skipped":@"Skipped",@"running":@"Running",@"queued":@"Queued",@"failed":@"Failed",@"interrupted":@"Interrupted",@"cancelled":@"Cancelled"};
    return labels[status] ?: status ?: @"Unknown";
}
- (NSDictionary*)selectedCall { for(NSDictionary* call in _snapshot[@"calls"]) if([call[@"id"] isEqual:_selectedID]) return call; return nil; }
- (NSString*)timestamp:(id)value {
    if(![value isKindOfClass:NSNumber.class]) return @"Unavailable";
    return [NSDateFormatter localizedStringFromDate:[NSDate dateWithTimeIntervalSince1970:[value doubleValue]/1000] dateStyle:NSDateFormatterMediumStyle timeStyle:NSDateFormatterMediumStyle];
}
- (void)render {
    [self clear:_calls]; [self clear:_detail];
    _attentionCount=[_snapshot[@"summary"][@"needs_attention"] integerValue];
    _notice.stringValue=_message ?: [NSString stringWithFormat:@"%ld calls · %ld need attention · %ld running",(long)[_snapshot[@"summary"][@"total_calls"] integerValue],(long)_attentionCount,(long)[_snapshot[@"summary"][@"running"] integerValue]];
    NSArray* all=_snapshot[@"calls"] ?: @[]; NSMutableArray* visible=[NSMutableArray array];
    for(NSDictionary* call in all) if(_filter.selectedSegment==0 || [call[@"needs_attention"] boolValue]) [visible addObject:call];
    BOOL found=NO; for(NSDictionary* call in visible) if([call[@"id"] isEqual:_selectedID]) found=YES;
    if(!found) _selectedID=[visible firstObject][@"id"];
    for(NSDictionary* call in visible) {
        NSString* title=[NSString stringWithFormat:@"%@%@\n%@",[call[@"id"] isEqual:_selectedID]?@"▸ ":@"",call[@"label"],[self statusLabel:call[@"status"]]];
        NSButton* button=[self button:title action:@selector(selectCall:) value:call[@"id"] stack:_calls];
        button.alignment=NSTextAlignmentLeft; button.cell.wraps=YES; button.bezelStyle=NSBezelStyleRegularSquare; [button.heightAnchor constraintEqualToConstant:48].active=YES; [button.widthAnchor constraintEqualToAnchor:_calls.widthAnchor constant:-24].active=YES;
    }
    if(!visible.count) [self text:_snapshot?@"No calls match this filter.":@"Activity is unavailable. Select Refresh to try again." stack:_calls heading:NO];
    NSDictionary* call=[self selectedCall]; if(!call) return;
    [self text:call[@"label"] stack:_detail heading:YES];
    if (_previewReport) {
        [self text:@"Review diagnostics before copying" stack:_detail heading:YES];
        [self text:@"Only status metadata is included. No transcript, audio, credentials or tokens. Nothing is uploaded." stack:_detail heading:NO];
        [self button:@"Copy metadata" action:@selector(confirmCopy:) value:nil stack:_detail];
        [self button:@"Back to activity" action:@selector(closePreview:) value:nil stack:_detail];
        [self text:_previewReport stack:_detail heading:NO];
        return;
    }

    if(_tabs.selectedSegment==0) {
        [self text:call[@"outcome"] stack:_detail heading:YES]; [self text:call[@"reason"] stack:_detail heading:NO];
        for(NSDictionary* operation in call[@"operations"]) {
            [self text:[NSString stringWithFormat:@"%@ · %@",operation[@"name"],[self statusLabel:operation[@"status"]]] stack:_detail heading:NO];
            [self text:operation[@"reason"] stack:_detail heading:NO];
        }
        for(NSDictionary* attempt in call[@"attempts"]) if([@[@"queued",@"running",@"validating"] containsObject:attempt[@"status"]]) {
            id elapsed=attempt[@"elapsed_ms"];
            [self text:[NSString stringWithFormat:@"%@ · %@\n%@ · %@\nElapsed: %@",attempt[@"task"],[self statusLabel:attempt[@"status"]],attempt[@"provider"],attempt[@"model"],[elapsed isKindOfClass:NSNumber.class]?[NSString stringWithFormat:@"%.1f s",[elapsed doubleValue]/1000]:@"Unavailable"] stack:_detail heading:NO];
        }
        [self renderLocalTranscription:call];
        [self text:@"Saved artifacts" stack:_detail heading:YES];
        for(NSDictionary* artifact in call[@"artifacts"]) if(![artifact[@"label"] isEqual:@"Open transcript"]) [self button:artifact[@"label"] action:@selector(openArtifact:) value:artifact stack:_detail];
        if([call[@"retry_request"] isKindOfClass:NSDictionary.class]) {
            NSButton* retry=[self button:@"Generate / retry recap" action:@selector(retry:) value:call[@"retry_request"] stack:_detail]; retry.enabled=!_mutation;
            [self text:call[@"retry_reason"] stack:_detail heading:NO];
            NSDictionary* request=call[@"retry_request"];
            [self text:[NSString stringWithFormat:@"Uses configured provider %@ and model %@. Only transcript text is sent.",request[@"expected_provider"],request[@"expected_model"]] stack:_detail heading:NO];
        } else [self text:call[@"retry_reason"] stack:_detail heading:NO];
        for(NSDictionary* attempt in call[@"attempts"]) if([attempt[@"cancel_request"] isKindOfClass:NSDictionary.class]) {
            NSButton* cancel=[self button:[attempt[@"cancel_requested"] boolValue]?@"Cancellation requested":@"Cancel request" action:@selector(cancelJob:) value:attempt[@"cancel_request"] stack:_detail];
            cancel.enabled=!_control && ![attempt[@"cancel_requested"] boolValue];
        }
    } else if(_tabs.selectedSegment==1) {
        [self text:[NSString stringWithFormat:@"Session %@ · %@ committed turns\nStarted: %@\nState: %@",call[@"id"],call[@"turn_count"],[self timestamp:call[@"started_at_ms"]],call[@"state"]] stack:_detail heading:NO];
        [self text:@"Stage times are recorded events. Missing timing is unavailable; no completion estimate is inferred. Failed raw provider responses are not retained." stack:_detail heading:NO];
        for(NSDictionary* attempt in call[@"attempts"]) [self renderAttempt:attempt events:NO];
    } else {
        if(![call[@"attempts"] count]) [self text:@"No AI attempts recorded for this call." stack:_detail heading:NO];
        for(NSDictionary* attempt in call[@"attempts"]) [self renderAttempt:attempt events:YES];
    }
    [self button:@"Copy diagnostics…" action:@selector(copyDiagnostics:) value:call[@"safe_report"] stack:_detail];
}
- (void)renderAttempt:(NSDictionary*)attempt events:(BOOL)events {
    [self text:[NSString stringWithFormat:@"%@ · %@ · %@ · attempt %@ · job %@",attempt[@"task"],attempt[@"phase"],attempt[@"status"],attempt[@"attempt"],attempt[@"id"]] stack:_detail heading:YES];
    id elapsed=attempt[@"elapsed_ms"];
    [self text:[NSString stringWithFormat:@"Provider: %@\nModel: %@\nQueued: %@\nStarted: %@\nFinished: %@\nElapsed: %@\nError: %@\n%@",attempt[@"provider"],attempt[@"model"],[self timestamp:attempt[@"queued_at_ms"]],[self timestamp:attempt[@"started_at_ms"]],[self timestamp:attempt[@"finished_at_ms"]],[elapsed isKindOfClass:NSNumber.class]?[NSString stringWithFormat:@"%.1f s",[elapsed doubleValue]/1000]:@"Unavailable",[attempt[@"error_code"] isKindOfClass:NSString.class]?attempt[@"error_code"]:@"None",attempt[@"reason"] ?: @""] stack:_detail heading:NO];
    if(events) for(NSDictionary* event in attempt[@"events"]) [self text:[NSString stringWithFormat:@"%@ · %@",[self timestamp:event[@"at_ms"]],event[@"status"]] stack:_detail heading:NO];
    if(events) for(NSDictionary* event in attempt[@"progress_events"]) [self text:[NSString stringWithFormat:@"%@ · %@",[self timestamp:event[@"at_ms"]],event[@"description"] ?: @""] stack:_detail heading:NO];
}
- (void)configureLocalModels:(ascolto_local_models_t*)models fixture:(NSData*)fixture languageMenu:(NSMenu*)languageMenu manageModels:(void (^)(void))manageModels {
    _localModels=models; _localModelsFixture=[fixture copy]; _localLanguageMenu=[languageMenu copy]; _manageModels=[manageModels copy];
}
- (void)renderLocalTranscription:(NSDictionary*)call {
    NSDictionary* local=[call[@"local_transcription"] isKindOfClass:NSDictionary.class]?call[@"local_transcription"]:@{};
    [self text:@"Transcription" stack:_detail heading:YES];
    NSStackView* actions=[[NSStackView alloc] initWithFrame:NSZeroRect]; actions.orientation=NSUserInterfaceLayoutOrientationHorizontal; actions.spacing=8;
    [_detail addArrangedSubview:actions];
    for(NSDictionary* artifact in call[@"artifacts"]) if([artifact[@"label"] isEqual:@"Open transcript"]) [self button:artifact[@"label"] action:@selector(openArtifact:) value:artifact stack:actions];
    NSButton* again=[self button:@"Transcribe again…" action:@selector(transcribeAgain:) value:call stack:actions];
    again.enabled=[local[@"can_retranscribe"] boolValue] && !_localPreparation;
    if([local[@"reason"] isKindOfClass:NSString.class]) [self text:local[@"reason"] stack:_detail heading:NO];
    if([_localMessageCallID isEqual:call[@"id"]] && _localMessage) [self text:_localMessage stack:_detail heading:NO];
    for(NSDictionary* version in local[@"versions"]) {
        NSString* model=[version[@"model_id"] isKindOfClass:NSString.class] && [version[@"model_id"] length]?version[@"model_id"]:@"Model unavailable";
        NSString* state=[self statusLabel:version[@"state"]];
        [self text:[NSString stringWithFormat:@"%@ · %@\n%@ · %@",version[@"label"] ?: @"Transcript version",model,[self timestamp:version[@"created_at_ms"]],state] stack:_detail heading:NO];
        id progress=version[@"progress"];
        if([progress isKindOfClass:NSNumber.class] && ![version[@"original"] boolValue]) {
            NSProgressIndicator* bar=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0,0,240,8)]; bar.indeterminate=NO; bar.maxValue=1; bar.doubleValue=MAX(0,MIN(1,[progress doubleValue])); [_detail addArrangedSubview:bar];
            [self text:[NSString stringWithFormat:@"%.0f%% processed",bar.doubleValue*100] stack:_detail heading:NO];
        }
        if([version[@"error_code"] isKindOfClass:NSString.class]) [self text:[NSString stringWithFormat:@"Error: %@",version[@"error_code"]] stack:_detail heading:NO];
        if(![version[@"original"] boolValue] && [version[@"path"] isKindOfClass:NSString.class]) [self button:@"Open version transcript" action:@selector(openArtifact:) value:@{@"path":version[@"path"]} stack:_detail];
    }
}
- (NSData*)localCatalog {
    ascolto_local_models_snapshot_t snapshot;
    ascolto_local_models_snapshot(_localModels,&snapshot);
    if(_localModelsFixture.length==sizeof(snapshot)) memcpy(&snapshot,_localModelsFixture.bytes,sizeof(snapshot));
    return [NSData dataWithBytes:&snapshot length:sizeof(snapshot)];
}
- (void)selectRetranscriptionModel:(id)sender {
    (void)sender;
    _retranscriptionPrecision.hidden=!ascolto_nemotron_bits([_retranscriptionModels.selectedItem.representedObject UTF8String]);
}
- (void)selectRetranscriptionPrecision:(NSSegmentedControl*)sender {
    NSMenuItem* item=_retranscriptionModels.selectedItem;
    if(ascolto_nemotron_bits([item.representedObject UTF8String]))
        item.representedObject=@(ascolto_nemotron_id((int)[sender tagForSegment:sender.selectedSegment]));
}
- (void)transcribeAgain:(AscoltoActivityButton*)sender {
    NSDictionary* call=sender.representedObject;
    NSDictionary* local=call[@"local_transcription"];
    if(_localPreparation || _transcriptionSheet || ![local[@"can_retranscribe"] boolValue] || !self.view.window) return;
    NSNumber* callID=[call[@"id"] copy];
    NSData* catalog=[self localCatalog];
    const ascolto_local_models_snapshot_t* snapshot=catalog.bytes;
    NSAlert* alert=[[NSAlert alloc] init]; alert.messageText=@"Transcribe again";
    alert.informativeText=@"Add a local transcription to the post-call queue. The previous transcript and original audio are preserved. This choice does not change the model for future calls.";
    [alert addButtonWithTitle:@"Add to queue"]; [alert addButtonWithTitle:@"Cancel"]; [alert addButtonWithTitle:@"Manage models…"];
    NSStackView* form=[[NSStackView alloc] initWithFrame:NSMakeRect(0,0,420,140)]; form.orientation=NSUserInterfaceLayoutOrientationVertical; form.alignment=NSLayoutAttributeLeading; form.spacing=8;
    NSPopUpButton* models=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(0,0,420,28) pullsDown:NO]; models.identifier=@"retranscribeModel";
    [form addArrangedSubview:[NSTextField labelWithString:@"Installed model"]]; [form addArrangedSubview:models];
    NSSegmentedControl* precision=[NSSegmentedControl segmentedControlWithLabels:@[@"8 bit",@"16 bit"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(selectRetranscriptionPrecision:)];
    precision.identifier=@"retranscribePrecision"; precision.accessibilityLabel=@"Nemotron weight precision";
    const int choices[]={8,16};
    for(int segment=0;segment<2;segment++) { [precision setTag:choices[segment] forSegment:segment]; [precision setEnabled:NO forSegment:segment]; [precision setToolTip:@"Install this variant in Manage models" forSegment:segment]; }
    NSMenuItem* nemotron=nil;
    for(int index=0;index<ASCOLTO_LOCAL_MODEL_COUNT;index++) {
        const ascolto_model_snapshot_t* model=&snapshot->models[index];
        if(!model->id[0] || (model->state!=ASCOLTO_MODEL_INCLUDED && model->state!=ASCOLTO_MODEL_READY)) continue;
        int bits=ascolto_nemotron_bits(model->id);
        NSMenuItem* item;
        if(bits) {
            if(!nemotron) { [models addItemWithTitle:@"Nemotron 3.5 Transcribe"]; nemotron=models.lastItem; nemotron.representedObject=@(model->id); }
            item=nemotron;
            for(int segment=0;segment<2;segment++) if(choices[segment]==bits) { [precision setEnabled:YES forSegment:segment]; [precision setToolTip:@(model->name) forSegment:segment]; }
        } else { [models addItemWithTitle:@(model->name)]; item=models.lastItem; item.representedObject=@(model->id); }
        if(!strcmp(model->id,snapshot->selected)) { item.representedObject=@(model->id); [models selectItem:item]; }
    }
    int initialBits=ascolto_nemotron_bits([nemotron.representedObject UTF8String]);
    precision.selectedSegment=initialBits==8?0:initialBits==16?1:-1;
    _retranscriptionModels=models; _retranscriptionPrecision=precision;
    models.target=self; models.action=@selector(selectRetranscriptionModel:);
    [form addArrangedSubview:precision]; [self selectRetranscriptionModel:nil];
    NSPopUpButton* languages=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(0,0,420,28) pullsDown:NO]; languages.identifier=@"retranscribeLanguage";
    [form addArrangedSubview:[NSTextField labelWithString:@"Language"]]; [form addArrangedSubview:languages];
    for(NSMenuItem* source in _localLanguageMenu.itemArray) if([source.representedObject isKindOfClass:NSString.class]) { [languages addItemWithTitle:source.title]; languages.lastItem.representedObject=[source.representedObject copy]; }
    if(!languages.numberOfItems) { [languages addItemWithTitle:@"Detect automatically"]; languages.lastItem.representedObject=@"auto"; }
    NSString* language=[local[@"language"] isKindOfClass:NSString.class]?local[@"language"]:@"auto";
    BOOL matched=NO; for(NSMenuItem* item in languages.itemArray) if([item.representedObject isEqual:language]) { [languages selectItem:item]; matched=YES; break; }
    if(!matched && language.length && ![language isEqual:@"Unknown"]) { [languages addItemWithTitle:language]; languages.lastItem.representedObject=language; [languages selectItem:languages.lastItem]; }
    if(!models.numberOfItems) { [form addArrangedSubview:[NSTextField wrappingLabelWithString:@"No installed model is ready. Open Manage models to install or verify a model."]]; alert.buttons.firstObject.enabled=NO; }
    alert.accessoryView=form; _transcriptionSheet=alert;
    __weak AscoltoActivityViewController* weakSelf=self;
    [alert beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        AscoltoActivityViewController* owner=weakSelf; if(!owner || owner.transcriptionSheet!=alert) return;
        owner.transcriptionSheet=nil; owner.retranscriptionModels=nil; owner.retranscriptionPrecision=nil;
        if(response==NSAlertThirdButtonReturn) { if(owner.manageModels) owner.manageModels(); return; }
        if(response!=NSAlertFirstButtonReturn || !models.selectedItem.representedObject) return;
        [owner enqueueLocalCall:callID model:[models.selectedItem.representedObject copy] language:[languages.selectedItem.representedObject copy] catalog:catalog];
    }];
}
- (NSString*)localError:(NSString*)code {
    return @{@"source_audio_missing":@"Saved audio is missing or unreadable. Restore the original audio to transcribe again.",@"source_busy":@"This call is still being processed. Try again when it finishes.",@"model_unavailable":@"The selected model is no longer ready. Check Manage models and try again.",@"invalid_language":@"This language is unavailable. Choose another language.",@"cancelled":@"Transcription preparation cancelled. Previous artifacts are preserved.",@"already_queued":@"This transcription is already in the local queue.",@"decode_failed":@"Saved audio could not be prepared. Previous artifacts are preserved.",@"storage_error":@"The transcription could not be queued. Check archive access and available disk space."}[code] ?: @"The transcription could not be queued. Previous artifacts are preserved.";
}
- (void)enqueueLocalCall:(NSNumber*)callID model:(NSString*)model language:(NSString*)language catalog:(NSData*)catalog {
    if(_localPreparation) return;
    AscoltoActivityPreparation* preparation=[[AscoltoActivityPreparation alloc] init]; _localPreparation=preparation;
    _localMessageCallID=callID; _localMessage=@"Preparing saved audio for the local queue…"; [self render];
    NSString* config=[_config copy];
    __weak AscoltoActivityViewController* weakSelf=self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        char error[128]={0}; int64_t job=0;
        int result=ascolto_retranscribe_enqueue(config.UTF8String,catalog.bytes,callID.longLongValue,model.UTF8String,language.UTF8String,preparationCancelled,(__bridge void*)preparation,&job,error,sizeof(error));
        NSString* code=@(error);
        dispatch_async(dispatch_get_main_queue(), ^{
            AscoltoActivityViewController* owner=weakSelf; if(!owner || owner.localPreparation!=preparation) return;
            owner.localPreparation=nil;
            owner.localMessage=result?[owner localError:code]:@"Added to the local transcription queue. Progress and the new version will appear below.";
            [owner render]; if(owner.timer) [owner refresh:nil];
        });
    });
}
- (void)cancelLocalPreparation {
    _localPreparation.cancelled=YES;
    if(_transcriptionSheet) { NSAlert* alert=_transcriptionSheet; _transcriptionSheet=nil; [alert.window.sheetParent endSheet:alert.window returnCode:NSAlertSecondButtonReturn]; }
}
- (void)changeFilter:(id)sender { (void)sender; [self render]; }
- (void)changeTab:(id)sender { (void)sender; [self render]; }
- (void)selectCall:(AscoltoActivityButton*)sender { _selectedID=sender.representedObject; [self render]; }
- (void)openArtifact:(AscoltoActivityButton*)sender { NSString* path=sender.representedObject[@"path"]; if([path isKindOfClass:NSString.class]) [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]]; }
- (void)retry:(AscoltoActivityButton*)sender { [self runRequest:sender.representedObject mutation:YES]; }
- (void)cancelJob:(AscoltoActivityButton*)sender { [self runRequest:sender.representedObject mutation:YES]; }
- (void)copyDiagnostics:(AscoltoActivityButton*)sender {
    _previewReport=[sender.representedObject isKindOfClass:NSString.class] ? [sender.representedObject copy] : @"No diagnostic metadata available.";
    [self render];
}
- (void)confirmCopy:(id)sender {
    (void)sender;
    if (!_previewReport) return;
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:_previewReport forType:NSPasteboardTypeString];
    _message=@"Diagnostic metadata copied. Nothing was uploaded.";
    _previewReport=nil;
    [self render];
}
- (void)closePreview:(id)sender { (void)sender; _previewReport=nil; [self render]; }
- (void)refresh:(id)sender {
    if(sender && !self.view.window.visible && _lastRead && -[_lastRead timeIntervalSinceNow]<15) return;
    if(!_reader) [self runRequest:@{@"action":@"diagnostics"} mutation:NO];
}
- (void)runRequest:(NSDictionary*)request mutation:(BOOL)mutation {
    BOOL control=[request[@"action"] isEqual:@"cancel_job"];
    if((control && _control) || (mutation && !control && _mutation) || (!mutation && _reader)) return;
    char script[2300], python[2300];
    if(ascolto_ai_worker_paths("ai_providers.py",script,sizeof(script),python,sizeof(python))) { _message=@"Diagnostics worker is unavailable. Rebuild Ascolto."; [self render]; return; }
    NSTask* task=[[NSTask alloc] init]; task.executableURL=[NSURL fileURLWithPath:@"/usr/bin/env"];
    task.arguments=@[@"PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",@"PYTHONDONTWRITEBYTECODE=1",@(python),@"-B",@(script),@"--config",_config];
    NSPipe* input=[NSPipe pipe]; NSPipe* output=[NSPipe pipe]; task.standardInput=input; task.standardOutput=output; task.standardError=NSFileHandle.fileHandleWithNullDevice;
    NSData* bytes=[NSJSONSerialization dataWithJSONObject:request options:0 error:NULL];
    if(mutation) { if(control) _control=task; else _mutation=task; _message=control?@"Cancellation requested. A provider may continue work already received; waiting for the worker to stop safely.":@"Request submitted. Recorded stages will appear as the worker progresses."; } else { _reader=task; _lastRead=NSDate.date; }
    __weak AscoltoActivityViewController* weakSelf=self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        NSData* response=nil;
        @try { if([task launchAndReturnError:NULL]) { [input.fileHandleForWriting writeData:bytes]; [input.fileHandleForWriting closeFile]; response=[output.fileHandleForReading readDataToEndOfFile]; [task waitUntilExit]; } } @catch(NSException* exception) { (void)exception; }
        NSDictionary* result=response?[NSJSONSerialization JSONObjectWithData:response options:0 error:NULL]:nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            AscoltoActivityViewController* owner=weakSelf; if(!owner || (control?owner.control:mutation?owner.mutation:owner.reader)!=task) return;
            if(control) owner.control=nil; else if(mutation) owner.mutation=nil; else owner.reader=nil;
            if([result[@"ok"] boolValue]) { if(!mutation) owner.snapshot=result[@"result"]; if(mutation) owner.message=control?@"Cancellation requested. The provider may continue work it already received.":@"Request completed. Review the recorded result below."; }
            else owner.message=result[@"message"] ?: @"Could not read activity. Select Refresh to try again.";
            [owner render]; if(mutation) [owner refresh:nil];
        });
    });
    if(mutation) [self render];
}
- (void)showActivity { [self.view layoutSubtreeIfNeeded]; [self refresh:nil]; }
- (void)stopRefreshing { [_timer invalidate]; _timer=nil; if(_reader.running) [_reader terminate]; _reader=nil; [self cancelLocalPreparation]; }
@end
