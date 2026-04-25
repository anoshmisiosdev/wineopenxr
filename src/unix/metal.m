/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* GPU fence wait primitive via MTLSharedEvent. The listener is passed in
 * per call so the primitive is stateless, and listener ownership sits in
 * wine_XrSession.
 *
 * Uses wine/debug_slim.h because Metal.h's signed-char BOOL collides with
 * Wine's int BOOL if <windef.h> is included */

#import <Metal/Metal.h>

#include <stdlib.h>
#include <stdint.h>
#include <mach/mach_time.h>

#include "wine/debug_slim.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

static int g_gpu_sync_stats_enabled;
static uint64_t g_gpu_sync_wait_count;
static uint64_t g_gpu_sync_timeout_count;
static uint64_t g_gpu_sync_max_wait_ns;
static uint64_t g_gpu_sync_last_report;
static mach_timebase_info_data_t g_timebase;

static void ensure_gpu_sync_stats_init(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *env = getenv("WINEOPENXR_GPU_SYNC_STATS");
        mach_timebase_info(&g_timebase);
        g_gpu_sync_stats_enabled = (env && env[0] == '1') ? 1 : 0;
        if (g_gpu_sync_stats_enabled)
            g_gpu_sync_last_report = mach_absolute_time();
    });
}

static uint64_t ticks_to_ns(uint64_t ticks)
{
    return ticks * g_timebase.numer / g_timebase.denom;
}

static void gpu_sync_report_if_due(void)
{
    uint64_t now, elapsed_ns;

    if (!g_gpu_sync_stats_enabled)
        return;

    now = mach_absolute_time();
    elapsed_ns = ticks_to_ns(now - g_gpu_sync_last_report);

    if (elapsed_ns >= 1000000000ULL)
    {
        WINE_TRACE("gpu_sync waits=%llu timeouts=%llu max_wait_ms=%.1f\n",
                   (unsigned long long)g_gpu_sync_wait_count,
                   (unsigned long long)g_gpu_sync_timeout_count,
                   (double)g_gpu_sync_max_wait_ns / 1e6);
        g_gpu_sync_wait_count = 0;
        g_gpu_sync_timeout_count = 0;
        g_gpu_sync_max_wait_ns = 0;
        g_gpu_sync_last_report = now;
    }
}

int wait_gpu_fence(void *mtl_listener_ptr,
                   uint64_t mtl_shared_event_ptr,
                   uint64_t fence_value)
{
    long wait_result;
    uint64_t wait_start_ticks = 0, wait_ns;
    id<MTLSharedEvent> shared_event;
    MTLSharedEventListener *shared_event_listener;

    ensure_gpu_sync_stats_init();

    if (!mtl_shared_event_ptr || !fence_value)
        return 0;

    shared_event = (id<MTLSharedEvent>)(void *)(uintptr_t)mtl_shared_event_ptr;

    if (shared_event.signaledValue >= fence_value)
    {
        gpu_sync_report_if_due();
        return 0;
    }

    if (!mtl_listener_ptr)
    {
        WINE_ERR("wait_gpu_fence called without a listener; slow path disabled\n");
        return -1;
    }
    shared_event_listener = (MTLSharedEventListener *)mtl_listener_ptr;

    /* One timestamp feeds the per-wait trace and optional envelope counters */
    wait_start_ticks = mach_absolute_time();

    @autoreleasepool {
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

        /* Transfer ownership of semaphore and listener into the block. On timeout
         * the caller releases its refs and returns, but the block stays
         * queued on the listener's internal dispatch queue until the GPU
         * reaches fence_value. The callback-owned refs intentionally remain
         * live after a timeout. If the value never signals, they remain live
         * until process exit */
        dispatch_retain(semaphore);
        MTLSharedEventListener *retained_listener = [shared_event_listener retain];

        [shared_event notifyListener:shared_event_listener
                              atValue:fence_value
                                block:^(id<MTLSharedEvent> _Nonnull event, uint64_t value) {
            (void)event; (void)value;
            dispatch_semaphore_signal(semaphore);
            dispatch_release(semaphore);
            [retained_listener release];
        }];

        wait_result = dispatch_semaphore_wait(semaphore,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(50 * NSEC_PER_MSEC)));

        dispatch_release(semaphore);
    }

    wait_ns = ticks_to_ns(mach_absolute_time() - wait_start_ticks);
    if (g_gpu_sync_stats_enabled)
    {
        g_gpu_sync_wait_count++;
        if (wait_ns > g_gpu_sync_max_wait_ns)
            g_gpu_sync_max_wait_ns = wait_ns;
    }
    WINE_TRACE("gpu_sync wait_ms=%.2f fence_value=%llu\n",
               (double)wait_ns / 1e6,
               (unsigned long long)fence_value);

    if (wait_result != 0)
    {
        if (g_gpu_sync_stats_enabled)
            g_gpu_sync_timeout_count++;
        WINE_WARN("GPU fence wait timed out (expected=%llu, signaled=%llu)\n",
                  (unsigned long long)fence_value,
                  (unsigned long long)shared_event.signaledValue);
        return -1;
    }

    gpu_sync_report_if_due();
    return 0;
}
