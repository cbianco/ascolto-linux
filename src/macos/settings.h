#import <Cocoa/Cocoa.h>
@interface AscoltoSettingsWindow : NSWindowController
- (instancetype)initWithOptions:(NSDictionary* (^)(void))options target:(id)target;
- (void)showSettings;
- (void)refreshNativeValues;
- (void)cancelWorker;
- (void)performAITask:(NSString*)action;
@end
