/* MTLDevice accounts for allocations in this process, including shared buffers. */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stddef.h>

size_t bench_metal_allocated_bytes(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        return device ? device.currentAllocatedSize : 0;
    }
}
