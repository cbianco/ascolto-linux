#import <Cocoa/Cocoa.h>

@interface AscoltoActivityViewController : NSViewController
@property (readonly) NSInteger attentionCount;
- (instancetype)initWithConfig:(NSString*)config;
- (void)showActivity;
- (void)stopRefreshing;
@end
