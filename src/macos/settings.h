#import <Cocoa/Cocoa.h>
@interface AscoltoSettingsWindow : NSWindowController
@property (readonly) NSInteger attentionCount;
- (instancetype)initWithOptions:(NSDictionary* (^)(void))options target:(id)target;
- (void)showSettings;
- (void)showActivity;
- (void)stopActivityRefresh;
- (void)refreshNativeValues;
- (void)cancelWorker;
- (void)performAITask:(NSString*)action;
@end
