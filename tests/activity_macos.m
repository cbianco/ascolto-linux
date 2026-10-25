#import <Cocoa/Cocoa.h>
#import "../src/macos/activity.h"
#import "../src/macos/settings.h"

@interface AscoltoActivityViewController (Testing)
- (void)render;
- (void)runRequest:(NSDictionary*)request mutation:(BOOL)mutation;
@end
@interface ActivityFixtureWindow : AscoltoActivityViewController
@end
@implementation ActivityFixtureWindow
- (void)runRequest:(NSDictionary*)request mutation:(BOOL)mutation { (void)request; (void)mutation; }
@end

static BOOL containsText(NSView* view, NSString* text) {
    if([view isKindOfClass:NSTextField.class] && [[(NSTextField*)view stringValue] containsString:text]) return YES;
    if([view isKindOfClass:NSButton.class] && [[(NSButton*)view title] containsString:text]) return YES;
    for(NSView* child in view.subviews) if(containsText(child,text)) return YES;
    return NO;
}
static NSButton* findButton(NSView* view, NSString* title) {
    if([view isKindOfClass:NSButton.class] && [[(NSButton*)view title] isEqual:title]) return (NSButton*)view;
    for(NSView* child in view.subviews) { NSButton* found=findButton(child,title); if(found) return found; }
    return nil;
}

int main(int argc, const char** argv) {
    @autoreleasepool {
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSString* output=argc>1?@(argv[1]):@"build/activity-previews";
        [[NSFileManager defaultManager] createDirectoryAtPath:output withIntermediateDirectories:YES attributes:nil error:NULL];
        ActivityFixtureWindow* controller=[[ActivityFixtureWindow alloc] initWithConfig:@"/unused/config.json"]; [controller stopRefreshing];
        AscoltoSettingsWindow* host=[[AscoltoSettingsWindow alloc] initWithOptions:^NSDictionary* {
            return @{@"config":@"/tmp/ascolto-unified-preview/config.json"};
        } target:nil];
        [host stopActivityRefresh];
        [host setValue:controller forKey:@"activity"];
        [host showActivity];
        NSWindow* originalWindow=host.window;
        NSCAssert(NSApp.activationPolicy==NSApplicationActivationPolicyRegular,@"An open main window must be available to the Dock and Command-Tab");
        NSUInteger originalWindowCount=NSApp.windows.count;
        NSCAssert(controller.view.window==originalWindow,@"Diagnostics must be embedded in the single app window");
        NSDictionary* attempt=@{@"id":@6,@"task":@"recap",@"phase":@"final",@"attempt":@1,@"status":@"failed",@"provider":@"openrouter",@"model":@"fixture/model",@"model_label":@"Fixture model",@"error_code":@"invalid_evidence",@"reason":@"A generated citation did not match the transcript. Publication was blocked.",@"queued_at_ms":@1792146408690,@"started_at_ms":@1792146408696,@"finished_at_ms":@1792146442002,@"elapsed_ms":@33312,@"events":@[@{@"status":@"queued",@"at_ms":@1792146408690},@{@"status":@"running",@"at_ms":@1792146408696},@{@"status":@"failed",@"at_ms":@1792146442002}],@"cancel_request":NSNull.null};
        attempt = [attempt mutableCopy];
        [(NSMutableDictionary*)attempt setObject:@[@{@"at_ms":@1792146442002,@"description":@"Chunk 3 of 18; Stage: validation; Section: actions; Item: 2; Citation: 1; Reason: quote_not_in_source."}] forKey:@"progress_events"];
        NSDictionary* failed=@{@"id":@45,@"label":@"16 Oct 2026 · 12:21",@"started_at_ms":@1792146065444,@"state":@"completed",@"status":@"needs_attention",@"outcome":@"Recap needs attention",@"reason":@"Transcript saved. A generated citation did not match the source transcript.",@"turn_count":@91,@"needs_attention":@YES,@"operations":@[@{@"task":@"local_transcript",@"name":@"Local transcript",@"status":@"ready",@"reason":@"91 committed turns"},@{@"task":@"recap",@"name":@"Final recap",@"status":@"failed",@"reason":@"Publication blocked by evidence validation."}],@"artifacts":@[@{@"label":@"Open transcript",@"path":@"/fixture/transcript.md",@"is_directory":@NO}],@"attempts":@[attempt],@"retry_request":@{@"action":@"recap",@"session_id":@45,@"expected_provider":@"openrouter",@"expected_model":@"fixture/model"},@"safe_report":@"Session 45\nError: invalid_evidence\nNo captured content."};
        NSMutableDictionary* empty=[failed mutableCopy]; empty[@"id"]=@47; empty[@"label"]=@"16 Oct 2026 · 13:06"; empty[@"status"]=@"Skipped"; empty[@"outcome"]=@"No speech detected"; empty[@"reason"]=@"No recap input."; empty[@"turn_count"]=@0; empty[@"needs_attention"]=@NO; empty[@"attempts"]=@[]; empty[@"retry_request"]=NSNull.null; empty[@"retry_reason"]=@"No committed speech to summarize.";
        [controller setValue:@{@"summary":@{@"total_calls":@2,@"needs_attention":@1,@"running":@0},@"calls":@[empty,failed]} forKey:@"snapshot"];
        [controller render];
        NSCAssert(containsText(host.window.contentView,@"No speech detected"),@"Empty calls remain visible");
        NSCAssert(containsText([controller valueForKey:@"calls"],@"12:21"),@"Earlier failed calls remain listed");
        NSCAssert(controller.attentionCount==1,@"Attention count belongs to all calls");
        NSSegmentedControl* filter=[controller valueForKey:@"filter"]; filter.selectedSegment=1; [controller render];
        NSCAssert(containsText([controller valueForKey:@"detail"],@"Transcript saved"),@"Evidence failure must not imply transcript loss");
        NSCAssert(containsText([controller valueForKey:@"detail"],@"Generate / retry recap"),@"Explicit retry is reachable");
        NSCAssert(!containsText([controller valueForKey:@"calls"],@"13:06"),@"Attention filter removes successful/empty calls");
        NSSegmentedControl* tabs=[controller valueForKey:@"tabs"];
        for(NSString* appearance in @[NSAppearanceNameAqua,NSAppearanceNameDarkAqua]) {
            host.window.appearance=[NSAppearance appearanceNamed:appearance]; [host showActivity];
            for(NSInteger tab=0;tab<3;tab++) {
                tabs.selectedSegment=tab; [controller render];
                if(tab==2) NSCAssert(containsText([controller valueForKey:@"detail"],@"job 6"),@"Attempts without output artifacts remain visible");
                if(tab==2) NSCAssert(containsText([controller valueForKey:@"detail"],@"Chunk 3 of 18"),@"Attempts expose recorded chunk validation failures");
                if(tab==1) NSCAssert(containsText([controller valueForKey:@"detail"],@"invalid_evidence"),@"Technical safe error is displayed");
                [host.window.appearance performAsCurrentDrawingAppearance:^{
                    [host.window.contentView layoutSubtreeIfNeeded]; [host.window displayIfNeeded];
                    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
                    NSView* view=host.window.contentView;  NSBitmapImageRep* bitmap=[view bitmapImageRepForCachingDisplayInRect:view.bounds];
                    [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
                    NSButton* selectedButton=[[[controller valueForKey:@"calls"] arrangedSubviews] firstObject];
                    NSBitmapImageRep* callBitmap=[selectedButton bitmapImageRepForCachingDisplayInRect:selectedButton.bounds];
                    [selectedButton cacheDisplayInRect:selectedButton.bounds toBitmapImageRep:callBitmap];
                    NSUInteger textPixels=0; BOOL dark=[appearance isEqual:NSAppearanceNameDarkAqua];
                    for(NSInteger y=5;y<callBitmap.pixelsHigh-5;y++) for(NSInteger x=8;x<callBitmap.pixelsWide-8;x++) {
                        NSColor* pixel=[[callBitmap colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
                        CGFloat brightness=(pixel.redComponent+pixel.greenComponent+pixel.blueComponent)/3;
                        if(dark?brightness>0.65:brightness<0.3) textPixels++;
                    }
                    NSCAssert(textPixels>100,@"Selected call timestamp and status must render visibly in both appearances");
                    NSString* name=[NSString stringWithFormat:@"activity-%@-%ld.png",[appearance isEqual:NSAppearanceNameAqua]?@"light":@"dark",(long)tab];
                    NSCAssert([[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[output stringByAppendingPathComponent:name] atomically:YES],@"Preview renders");
                }];
            }
        }
        NSMutableDictionary* activeAttempt=[attempt mutableCopy]; activeAttempt[@"status"]=@"running"; activeAttempt[@"elapsed_ms"]=@12000;
        activeAttempt[@"cancel_request"]=@{@"action":@"cancel_job",@"job_id":@6}; activeAttempt[@"cancel_requested"]=@NO;
        NSMutableDictionary* activeCall=[failed mutableCopy]; activeCall[@"attempts"]=@[activeAttempt]; activeCall[@"retry_request"]=NSNull.null; activeCall[@"retry_reason"]=@"A recap request is already running.";
        [controller setValue:@{@"summary":@{@"total_calls":@2,@"needs_attention":@1,@"running":@1},@"calls":@[empty,activeCall]} forKey:@"snapshot"];
        NSTask* pending=[[NSTask alloc] init];
        [controller setValue:pending forKey:@"mutation"]; tabs.selectedSegment=0; [controller render];
        NSCAssert(findButton([controller valueForKey:@"detail"],@"Cancel request").enabled,@"Cancel stays available while the long-running retry worker is occupied");
        NSCAssert(containsText([controller valueForKey:@"detail"],@"12.0 s"),@"Overview exposes observable elapsed time");
        NSCAssert(containsText([controller valueForKey:@"detail"],@"fixture/model"),@"Overview exposes running provider/model");
        [controller setValue:[[NSTask alloc] init] forKey:@"control"]; [controller render];
        NSCAssert(!findButton([controller valueForKey:@"detail"],@"Cancel request").enabled,@"Duplicate cancel control requests stay disabled");
        [controller setValue:nil forKey:@"control"];
        [controller setValue:@45 forKey:@"selectedID"];
        [host setValue:@{} forKey:@"settings"];
        [host setValue:@"Storage" forKey:@"category"]; [host showSettings];
        [host showActivity];
        NSCAssert(host.window==originalWindow && NSApp.windows.count==originalWindowCount,@"Navigation must reuse the same top-level window");
        NSCAssert([[controller valueForKey:@"selectedID"] isEqual:@45],@"Navigation must preserve the selected call");
        NSCAssert([controller valueForKey:@"mutation"]==pending,@"Navigation must preserve the running request");
        [controller setValue:nil forKey:@"mutation"];
        filter.selectedSegment=1; [controller render];
        NSButton* preview=findButton([controller valueForKey:@"detail"],@"Copy diagnostics…"); [preview performClick:nil];
        NSCAssert(containsText([controller valueForKey:@"detail"],@"Review diagnostics before copying"),@"Diagnostic preview must be inline");
        NSCAssert(NSApp.windows.count==originalWindowCount,@"Copy preview must not create a second window");
        NSCAssert((host.window.styleMask & NSWindowStyleMaskResizable)!=0,@"The shared window must allow native resizing and zoom");
        NSCAssert((host.window.collectionBehavior & NSWindowCollectionBehaviorFullScreenPrimary)!=0,@"The shared window must support native full screen");
        NSCAssert([host.window standardWindowButton:NSWindowZoomButton].enabled,@"The native green window button must be enabled");
        [controller performSelector:@selector(closePreview:) withObject:nil];
        [host.window setContentSize:NSMakeSize(1280,920)];
        [host.window.contentView layoutSubtreeIfNeeded];
        NSScrollView* viewport=[host valueForKey:@"pageScroll"];
        NSCAssert(fabs(NSWidth(viewport.frame)-(NSWidth(host.window.contentView.bounds)-190))<2,@"The content viewport must grow with the window");
        NSCAssert(NSWidth(controller.view.bounds)>1000 && fabs(NSHeight(controller.view.bounds)-NSHeight(viewport.contentView.bounds))<2,@"Activity must fill the available enlarged viewport");
        NSCAssert([controller valueForKey:@"selectedID"]!=nil,@"Resizing must retain the selected call");
        [host setValue:@"Transcription" forKey:@"category"]; [host performSelector:@selector(buildPage)];
        [host.window setContentSize:NSMakeSize(900,540)];
        [host.window.contentView layoutSubtreeIfNeeded];
        NSCAssert(NSHeight(viewport.documentView.bounds)>=780 && viewport.hasVerticalScroller,@"Compact windows must keep the full settings form scrollable");
        [host showActivity];
        NSCAssert(host.window==originalWindow,@"Resize and navigation must retain the same window");
        [host.window resignKeyWindow];
        NSCAssert(NSApp.activationPolicy==NSApplicationActivationPolicyRegular,@"Switching to another window must not remove Ascolto from Command-Tab");
        [host.window close];
        NSCAssert(NSApp.activationPolicy==NSApplicationActivationPolicyAccessory,@"Closing the main window must restore menu-only operation");
        puts("Activity diagnostics AppKit: PASS (empty/failure/filter/retry/history/running/cancel, native window activation, light/dark; no provider requests)");
    }
    return 0;
}
