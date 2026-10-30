#import <Cocoa/Cocoa.h>
#include "ascolto/local_stt.h"

@interface AscoltoActivityViewController : NSViewController
@property (readonly) NSInteger attentionCount;
- (instancetype)initWithConfig:(NSString*)config;
- (void)showActivity;
- (void)stopRefreshing;
- (void)configureLocalModels:(ascolto_local_models_t*)models fixture:(NSData*)fixture languageMenu:(NSMenu*)languageMenu manageModels:(void (^)(void))manageModels;
- (void)cancelLocalPreparation;
@end
