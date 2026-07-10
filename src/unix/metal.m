/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* Uses wine/debug_slim.h because Metal.h's signed-char BOOL collides with
 * Wine's int BOOL if <windef.h> is included */

#import <Metal/Metal.h>

#include <stdint.h>

#include "wine/debug_slim.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

void encode_gpu_wait(void *mtl_command_queue,
                     uint64_t mtl_shared_event,
                     uint64_t fence_value)
{
    @autoreleasepool {
        id<MTLCommandQueue> queue = (id<MTLCommandQueue>)mtl_command_queue;
        id<MTLSharedEvent> event = (id<MTLSharedEvent>)(void *)(uintptr_t)mtl_shared_event;
        id<MTLCommandBuffer> cmdbuf;

        if (event.signaledValue >= fence_value) {
            return;
        }

        cmdbuf = [queue commandBuffer];

        if (!cmdbuf) {
            WINE_ERR("commandBuffer allocation failed for fence value %llu\n",
                     (unsigned long long)fence_value);
            return;
        }

        [cmdbuf encodeWaitForEvent:event value:fence_value];
        [cmdbuf commit];

        WINE_TRACE("encoded wait for fence value %llu\n",
                   (unsigned long long)fence_value);
    }
}
