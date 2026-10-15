/* Vector artwork from the approved FigCraft ear and cupped-hand studies.
 * Clear compositing leaves real transparency on every menu-bar background.
 * AppKit tints the template; the recording badge is drawn separately in red. */
#import <Cocoa/Cocoa.h>

static NSImage *ascolto_status_icon(BOOL recording, BOOL paused, BOOL pending) {
    /* Called only on the UI thread; templates remain appearance-adaptive. */
    static NSImage *icons[4];
    NSUInteger index = recording ? 1 : pending ? 3 : paused ? 2 : 0;
    if (icons[index]) return icons[index];
    CGFloat side = recording ? 20 : 18;
    NSImage *image = [NSImage imageWithSize:NSMakeSize(side, side) flipped:NO
        drawingHandler:^BOOL(NSRect rect) {
        CGContextRef c = NSGraphicsContext.currentContext.CGContext;
        CGContextSaveGState(c);
        CGContextTranslateCTM(c, 0, rect.size.height);
        CGFloat grid = recording ? 64 : 24;
        CGContextScaleCTM(c, rect.size.width / grid, -rect.size.height / grid);
        CGContextSetLineCap(c, kCGLineCapRound);
        CGContextSetLineJoin(c, kCGLineJoinRound);
        CGContextSetRGBStrokeColor(c, 0, 0, 0, 1);
        CGContextSetRGBFillColor(c, 0, 0, 0, 1);
        CGContextBeginTransparencyLayer(c, NULL);
        if (recording) {
            CGContextSetLineWidth(c, 3.2);

    CGContextBeginPath(c);
    CGContextMoveToPoint(c, 35, 61);
    CGContextAddCurveToPoint(c, 33, 55, 29, 51, 25, 47);
    CGContextAddLineToPoint(c, 21, 44);
    CGContextAddCurveToPoint(c, 18, 41, 20, 38, 23, 39);
    CGContextAddLineToPoint(c, 29, 43);
    CGContextAddCurveToPoint(c, 29, 37, 28, 31, 26, 25);
    CGContextAddLineToPoint(c, 21, 10);
    CGContextAddCurveToPoint(c, 20, 7, 22, 5, 24, 6);
    CGContextAddCurveToPoint(c, 26, 7, 27, 12, 28, 15);
    CGContextAddLineToPoint(c, 32, 27);
    CGContextAddLineToPoint(c, 26, 5);
    CGContextAddCurveToPoint(c, 25, 2, 27, 0, 29, 1);
    CGContextAddCurveToPoint(c, 31, 1, 32, 4, 33, 8);
    CGContextAddLineToPoint(c, 38, 25);
    CGContextAddLineToPoint(c, 34, 9);
    CGContextAddCurveToPoint(c, 33, 6, 35, 4, 37, 5);
    CGContextAddCurveToPoint(c, 39, 6, 40, 9, 41, 12);
    CGContextAddLineToPoint(c, 45, 28);
    CGContextAddLineToPoint(c, 43, 18);
    CGContextAddCurveToPoint(c, 42, 15, 44, 13, 46, 14);
    CGContextAddCurveToPoint(c, 49, 15, 50, 22, 52, 28);
    CGContextAddCurveToPoint(c, 55, 37, 54, 43, 52, 49);
    CGContextAddLineToPoint(c, 54, 61);
    CGContextStrokePath(c);
    CGContextSetBlendMode(c, kCGBlendModeClear);
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, 9, 37);
    CGContextAddCurveToPoint(c, 4, 26, 10, 14, 22, 14);
    CGContextAddCurveToPoint(c, 32, 14, 39, 23, 38, 32);
    CGContextAddCurveToPoint(c, 37, 40, 31, 44, 28, 50);
    CGContextAddCurveToPoint(c, 25, 57, 15, 58, 11, 52);
    CGContextAddCurveToPoint(c, 7, 47, 10, 41, 9, 37);
    CGContextClosePath(c);
    CGContextFillPath(c);
    CGContextSetBlendMode(c, kCGBlendModeNormal);
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, 12, 36);
    CGContextAddCurveToPoint(c, 7, 27, 12, 17, 21, 17);
    CGContextAddCurveToPoint(c, 30, 17, 36, 23, 35, 31);
    CGContextAddCurveToPoint(c, 34, 39, 28, 42, 25, 48);
    CGContextAddCurveToPoint(c, 23, 53, 17, 54, 14, 50);
    CGContextAddCurveToPoint(c, 12, 48, 13, 44, 15, 43);
    CGContextStrokePath(c);
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, 17, 29);
    CGContextAddCurveToPoint(c, 16, 24, 24, 22, 27, 26);
    CGContextAddCurveToPoint(c, 29, 29, 25, 32, 22, 33);
    CGContextAddCurveToPoint(c, 20, 34, 20, 37, 20, 39);
    CGContextStrokePath(c);
    CGContextSetBlendMode(c, kCGBlendModeClear);
    CGContextFillEllipseInRect(c, CGRectMake(25.7, 13.7, 12.6, 12.6));
    CGContextSetBlendMode(c, kCGBlendModeNormal);
        } else {
            CGContextSetLineWidth(c, 1.8);
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, 6, 9);
    CGContextAddCurveToPoint(c, 6, 4, 9, 2, 12.5, 2);
    CGContextAddCurveToPoint(c, 17, 2, 20, 5.3, 20, 9.4);
    CGContextAddCurveToPoint(c, 20, 13.5, 17, 14.6, 15.8, 16.7);
    CGContextAddCurveToPoint(c, 14.5, 19, 14.7, 22, 11.3, 22);
    CGContextAddCurveToPoint(c, 8.8, 22, 7.3, 20.4, 7.3, 18.5);
    CGContextStrokePath(c);
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, 10, 8.8);
    CGContextAddCurveToPoint(c, 10, 6.2, 14.7, 5.8, 15.5, 8.7);
    CGContextAddCurveToPoint(c, 16.1, 11.1, 13, 11.8, 12.1, 13.5);
    CGContextAddCurveToPoint(c, 11.6, 14.4, 11.8, 15.4, 11.8, 16.1);
    CGContextStrokePath(c);
            if (paused || pending) {
                CGContextSetBlendMode(c, kCGBlendModeClear);
                CGContextFillEllipseInRect(c, CGRectMake(13.7, 13.7, 10.6, 10.6));
                CGContextSetBlendMode(c, kCGBlendModeNormal);
                if (pending) {
                    for (int i = 0; i < 3; i++)
                        CGContextFillEllipseInRect(c, CGRectMake(14.5 + i * 3.2, 17.8, 2.4, 2.4));
                } else {
                    CGContextSetLineWidth(c, 2.1);
                    CGContextBeginPath(c);
                    CGContextMoveToPoint(c, 17.3, 16.6);
                    CGContextAddLineToPoint(c, 17.3, 21.4);
                    CGContextMoveToPoint(c, 20.7, 16.6);
                    CGContextAddLineToPoint(c, 20.7, 21.4);
                    CGContextStrokePath(c);
                }
            }
        }
        CGContextEndTransparencyLayer(c);
        CGContextRestoreGState(c);
        return YES;
    }];
    image.template = YES;
    image.accessibilityDescription = @"Ascolto";
    icons[index] = image;
    return image;
}

@interface AscoltoRecordingBadge : NSView
@end
@implementation AscoltoRecordingBadge
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSButton *button = (NSButton *)self.superview;
    NSRect image = [button.cell imageRectForBounds:button.bounds];
    /* SVG's y axis points down; AppKit image coordinates point up. */
    CGFloat y = button.isFlipped ? 20.0 / 64 : 44.0 / 64;
    NSPoint center = NSMakePoint(NSMinX(image) + image.size.width * .5,
                                NSMinY(image) + image.size.height * y);
    center = [self convertPoint:center fromView:button];
    CGFloat radius = image.size.width * 4.2 / 64;
    [[NSColor colorWithSRGBRed:220.0/255 green:62.0/255 blue:76.0/255 alpha:1] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(center.x-radius, center.y-radius,
                                                     radius*2, radius*2)] fill];
}
@end
