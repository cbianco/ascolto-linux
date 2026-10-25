#import <Cocoa/Cocoa.h>
#import "../src/macos/settings.h"
#include <math.h>
#include "ascolto/local_stt.h"

@interface AscoltoSettingsWindow (PreviewTesting)
- (void)buildPage;
@end

@interface SettingsPreviewTarget : NSObject
@end
@implementation SettingsPreviewTarget
- (void)onToggleAutoTrigger:(id)sender { (void)sender; }
@end

static void verifySignInButton(AscoltoSettingsWindow *controller, NSString *directory, NSString *theme) {
    NSButton *button = [controller valueForKey:@"connect"];
    NSCAssert([button.title isEqual:@"Continue with ChatGPT"] && button.image, @"Sign-in must retain its accessible title and official logo");
    NSCAssert(button.action == NSSelectorFromString(@"signIn:"), @"Sign-in action must remain attached");
    for (NSNumber *key in @[@YES, @NO]) {
        if (key.boolValue) [controller.window makeKeyAndOrderFront:nil];
        else [controller.window resignKeyWindow];
        for (NSNumber *enabled in @[@YES, @NO]) {
            button.enabled = enabled.boolValue;
            [button setNeedsDisplay:YES]; [controller.window displayIfNeeded];
            NSBitmapImageRep *rep = [button bitmapImageRepForCachingDisplayInRect:button.bounds];
            [button cacheDisplayInRect:button.bounds toBitmapImageRep:rep];
            NSUInteger foreground = 0, background = 0;
            for (NSInteger y = 0; y < rep.pixelsHigh; y++) for (NSInteger x = 0; x < rep.pixelsWide; x++) {
                NSColor *pixel = [[rep colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
                CGFloat r = pixel.redComponent, g = pixel.greenComponent, b = pixel.blueComponent;
                if (r > 0.35 && g > 0.35 && b > 0.35 && fabs(r-g) < 0.03 && fabs(g-b) < 0.03) foreground++;
                if (r < 0.1 && g < 0.1 && b < 0.1) background++;
            }
            NSCAssert(foreground > 100 && background > 500, @"Text and logo must remain visible against black in every window/control state");
            NSString *name = [NSString stringWithFormat:@"chatgpt-%@-%@-%@.png", theme, key.boolValue ? @"key" : @"inactive", enabled.boolValue ? @"enabled" : @"disabled"];
            [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[directory stringByAppendingPathComponent:name] atomically:YES];
        }
    }
    button.enabled = YES;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSString *directory = @(argv[1]);
        [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL];
        NSMenu *(^menu)(NSString*) = ^NSMenu*(NSString *title) {
            NSMenu *result = [[NSMenu alloc] initWithTitle:title];
            NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"System default" action:nil keyEquivalent:@""];
            item.state = NSControlStateValueOn; [result addItem:item]; return result;
        };
        SettingsPreviewTarget *target = [[SettingsPreviewTarget alloc] init];
        __block ascolto_local_models_snapshot_t localModels = {0};
        snprintf(localModels.selected,sizeof(localModels.selected),"whistle");localModels.available_bytes=32000000000ULL;
        const char *ids[]={"whistle","whisper-turbo-q5","parakeet-v3-q8","qwen3-asr-1.7b"};
        const char *names[]={"Whistle","Whisper large-v3-turbo Q5","Parakeet TDT v3","Qwen3-ASR 1.7B"};
        const char *descriptions[]={"Current baseline. Lower agreement in our sample; very lightweight.","Closest to the cloud reference in our Italian sample.","Good agreement in our sample, with very low processing delay.","Good agreement in our sample. Uses more memory than the other options."};
        const char *hints[]={"Default · included · 17 MB","Best test agreement","Speed / quality balance","CPU option · about 7 GB memory"};
        for(int i=0;i<4;i++){snprintf(localModels.models[i].id,40,"%s",ids[i]);snprintf(localModels.models[i].name,80,"%s",names[i]);snprintf(localModels.models[i].description,160,"%s",descriptions[i]);snprintf(localModels.models[i].hint,64,"%s",hints[i]);localModels.models[i].state=i?ASCOLTO_MODEL_MISSING:ASCOLTO_MODEL_INCLUDED;localModels.models[i].selected=i==0;localModels.models[i].bytes=i==1?574041195ULL:i==2?940663680ULL:i==3?4703112789ULL:0;}
        AscoltoSettingsWindow *controller = [[AscoltoSettingsWindow alloc] initWithOptions:^NSDictionary* {
            return @{@"localModelsFixture":[NSData dataWithBytes:&localModels length:sizeof(localModels)], @"config": @"/tmp/ascolto-settings-preview/config.json", @"output": @"~/Documents/Ascolto", @"automatic": @YES, @"saveAudio": @YES, @"cloudRecap": @NO, @"tts": @NO, @"busy": @NO,
                     @"languageMenu": menu(@"Language"), @"speakersMenu": menu(@"Speakers"), @"micMenu": menu(@"Microphone"), @"sysMenu": menu(@"Call audio")};
        } target:target];
        NSDictionary *profile = @{@"connected": @YES, @"model": @"fixture-model", @"models": @[@{@"id": @"fixture-model", @"name": @"Available model from provider"}], @"transcription_models":@[@{@"id":@"fixture-stt",@"name":@"Dedicated STT model from OpenRouter"}]};
        NSMutableDictionary *openai = [profile mutableCopy]; openai[@"account_label"] = @"person@example.com · abc123"; openai[@"account_id"] = @"fixture-account";
        NSDictionary *fixture = @{@"automatic_recaps": @NO, @"selected_provider": @"openrouter", @"language": @"Italian", @"runtime_ready": @YES,
            @"transcription": @{@"model":@"fixture-stt",@"enabled":@NO,@"cloud_audio_opt_in":@YES},
            @"profiles": @{@"opencode-zen": profile, @"opencode-go": profile, @"openrouter": profile, @"openai": openai},
            @"accounts": @[@{@"id": @"fixture-account", @"label": @"person@example.com · abc123"}]};
        [controller setValue:fixture forKey:@"settings"];
        [controller.window orderFront:nil];
        for (NSString *appearanceName in @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]) {
            controller.window.appearance = [NSAppearance appearanceNamed:appearanceName];
            NSString *theme = [appearanceName isEqual:NSAppearanceNameAqua] ? @"light" : @"dark";
            for (NSString *category in @[@"General", @"AI providers", @"Transcription", @"Recording", @"Storage"]) {
                NSArray *providers = [category isEqual:@"AI providers"] ? @[@"opencode-zen", @"opencode-go", @"openrouter", @"openai"] : @[@"opencode-zen"];
                for (NSString *provider in providers) {
                    [controller setValue:category forKey:@"category"]; [controller setValue:provider forKey:@"provider"];
                    [controller.window.appearance performAsCurrentDrawingAppearance:^{
                        [controller buildPage];
                        [controller.window.contentView layoutSubtreeIfNeeded];
                        [controller.window displayIfNeeded];
                        NSView *view = controller.window.contentView;
                        NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
                        [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
                        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                        NSString *name = [NSString stringWithFormat:@"settings-%@-%@-%@.png", theme, [category stringByReplacingOccurrencesOfString:@" " withString:@"-"], provider];
                        NSCAssert([png writeToFile:[directory stringByAppendingPathComponent:name] atomically:YES], @"Native preview must render");
                        if ([provider isEqual:@"openai"]) {
                            verifySignInButton(controller, directory, theme);
                            NSMutableDictionary *disconnected = [fixture mutableCopy];
                            disconnected[@"accounts"] = @[];
                            disconnected[@"profiles"] = @{@"openai": @{@"connected": @NO}};
                            [controller setValue:disconnected forKey:@"settings"];
                            [controller buildPage];
                            verifySignInButton(controller, directory, [theme stringByAppendingString:@"-disconnected"]);
                            [controller setValue:fixture forKey:@"settings"];
                        }
                    }];
                }
            }
        }
        [controller setValue:@"Transcription" forKey:@"category"];
        for(NSString *theme in @[@"light",@"dark"]){
            controller.window.appearance=[NSAppearance appearanceNamed:[theme isEqual:@"light"]?NSAppearanceNameAqua:NSAppearanceNameDarkAqua];
            for(NSString *state in @[@"initial",@"downloading",@"ready-retry",@"recording",@"unsupported",@"cloud"]){
                [controller setValue:@([state isEqual:@"cloud"]?1:0) forKey:@"transcriptionTab"];
                snprintf(localModels.selected,40,"%s",[state isEqual:@"ready-retry"]?"whisper-turbo-q5":"whistle");
                localModels.models[0].selected=![state isEqual:@"ready-retry"];localModels.models[1].selected=[state isEqual:@"ready-retry"];
                localModels.models[1].state=[state isEqual:@"downloading"]?ASCOLTO_MODEL_DOWNLOADING:ASCOLTO_MODEL_READY;
                localModels.models[1].transferred=210000000;
                localModels.models[2].state=ASCOLTO_MODEL_READY;
                localModels.models[3].state=ASCOLTO_MODEL_ERROR;
                snprintf(localModels.models[3].message,256,"Download interrupted. Check connection and retry.");
                if([state isEqual:@"unsupported"]){localModels.models[3].state=ASCOLTO_MODEL_UNSUPPORTED;snprintf(localModels.models[3].message,256,"Native runtime is unavailable in this build. Install a build with local model support.");}
                if([state isEqual:@"initial"]){for(int i=1;i<4;i++){localModels.models[i].state=ASCOLTO_MODEL_MISSING;localModels.models[i].message[0]=0;}}
                snprintf(localModels.active,40,"%s",[state isEqual:@"recording"]?"whisper-turbo-q5":"");localModels.models[1].active=[state isEqual:@"recording"];localModels.revision++;
                [controller.window.appearance performAsCurrentDrawingAppearance:^{[controller buildPage];[controller.window displayIfNeeded];NSView *view=controller.window.contentView;NSBitmapImageRep *rep=[view bitmapImageRepForCachingDisplayInRect:view.bounds];[view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];NSString *name=[NSString stringWithFormat:@"local-models-%@-%@.png",theme,state];NSCAssert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[directory stringByAppendingPathComponent:name] atomically:YES],@"Local model preview must render");}];
            }
        }
        [controller.window orderOut:nil];
        puts("Native AppKit Settings preview: PASS (all categories, provider profiles, light/dark; no credentials or network)");
    }
    return 0;
}
