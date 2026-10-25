#import <Cocoa/Cocoa.h>
#import "../src/macos/app_icon.h"
#include <assert.h>

int main(int argc,const char** argv) {
    @autoreleasepool {
        NSString* path=argc>1 ? @(argv[1]) : @"bin/Ascolto.app";
        NSBundle* bundle=[NSBundle bundleWithPath:path.stringByStandardizingPath.stringByResolvingSymlinksInPath];
        if (!bundle) bundle=[NSBundle bundleWithPath:[NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path]];
        NSImage* icon=ascolto_application_icon_for_bundle(bundle);
        assert(icon.valid);
        NSBitmapImageRep* pixels=[[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL pixelsWide:256 pixelsHigh:256
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:pixels]];
        [icon drawInRect:NSMakeRect(0,0,256,256) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];
        [NSGraphicsContext restoreGraphicsState];
        NSInteger minX=256,minY=256,maxX=-1,maxY=-1,green=0;
        for(NSInteger y=0;y<256;y++) for(NSInteger x=0;x<256;x++) {
            NSColor* color=[[pixels colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
            if(color.alphaComponent>.9) {
                minX=MIN(minX,x);minY=MIN(minY,y);maxX=MAX(maxX,x);maxY=MAX(maxY,y);
                if(color.redComponent<.25 && color.greenComponent<.35 && color.blueComponent<.3) green++;
            }
        }
        // Reject the generic gray/blue placeholder and full-bleed oversized art.
        assert(green>1000);
        assert(minX>=24 && minX<=26 && minY>=24 && minY<=26);
        assert(maxX-minX+1>=204 && maxX-minX+1<=208);
        assert(maxY-minY+1>=204 && maxY-minY+1<=208);
        assert([pixels colorAtX:0 y:0].alphaComponent<.01);
        if(argc>2) assert([[pixels representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES]);
        printf("Native application icon: PASS (approved green gesture, transparent margin, 206px tile at 256px)\n");
    }
    return 0;
}
