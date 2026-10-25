#ifndef ASCOLTO_MACOS_APP_ICON_H
#define ASCOLTO_MACOS_APP_ICON_H

#import <Cocoa/Cocoa.h>

static inline NSImage* ascolto_application_icon_for_bundle(NSBundle* bundle) {
    NSString* path=[bundle pathForResource:@"Ascolto" ofType:@"icns"];
    NSImage* source=path ? [[NSImage alloc] initWithContentsOfFile:path] : nil;
    if (!source.valid) return nil;
    // Render the known resource explicitly. Workspace can return a cached
    // generic application icon while an accessory app is being registered.
    // Match the 824px visible tile on a 1024px legacy macOS icon canvas.
    NSBitmapImageRep* pixels=[[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:1024 pixelsHigh:1024
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
        colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    if (!pixels) return nil;
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:pixels]];
    [NSColor.clearColor setFill];
    NSRectFillUsingOperation(NSMakeRect(0,0,1024,1024),NSCompositingOperationCopy);
    [source drawInRect:NSMakeRect(100,100,824,824) fromRect:NSZeroRect
        operation:NSCompositingOperationSourceOver fraction:1];
    [NSGraphicsContext restoreGraphicsState];
    NSImage* result=[[NSImage alloc] initWithSize:NSMakeSize(1024,1024)];
    [result addRepresentation:pixels];
    return result;
}

static inline BOOL ascolto_apply_application_icon(void) {
    static NSImage* icon;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        icon=ascolto_application_icon_for_bundle(NSBundle.mainBundle);
    });
    if (!icon) return NO;
    NSApp.applicationIconImage=icon;
    return YES;
}

#endif
