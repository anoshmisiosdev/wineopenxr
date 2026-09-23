/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* PROTOTYPE: zero-copy D3D11 swapchain images under CrossOver's D3DMetal.
 *
 * D3DMetal (Apple GPTK) refuses every D3D11/DXGI sharing entry point, so the
 * DXMT path (OpenSharedResource on a D3DKMT record) cannot work there. Instead
 * the unix half swizzles the Metal texture-creation selectors D3DMetal calls
 * and, while a thread is "armed", hands D3DMetal the OpenXR runtime's own
 * swapchain MTLTexture in place of the one it asked for. The technique (not the
 * code) follows utmapp/d3dmetal-native's dmn_share_metal.mm (MIT).
 *
 * What CrossOver 26.2's D3DMetal 4.0b1 was observed to do (x86_64/Rosetta,
 * Apple M4 Pro, test/d3d11_d3dmetal_subst_test.cpp):
 *  - every D3D11 CreateTexture2D (RT, typeless, sRGB, 16F, arrays, mips, UAV,
 *    depth, with/without init data) allocates through exactly one
 *    -[MTLDevice newTextureWithDescriptor:], synchronously on the calling
 *    thread. Descriptors ask for Shared storage + PixelFormatView; typeless
 *    maps to the UNORM format; ArraySize 1 is Type2D, >1 Type2DArray.
 *  - D3D11 renders on CLASSIC MTLCommandQueues (unretained command buffers
 *    encoded on D3DMetal worker threads). It creates MTL4 queues and residency
 *    sets, and adds each new texture (ours included) to a residency set, but
 *    the MTL4 backend is gated on API==D3D12 (and D3DM_MTL4), so D3D11 never
 *    commits on them. No heap/placement textures, no -heap queries on ours.
 *  - ID3D11Fence is a plain MTLEvent made by -newEvent inside CreateFence on
 *    the calling thread; substituting an MTLSharedEvent there gives a working
 *    GPU-side release fence (Context4::Signal lands on D3DMetal's queue).
 *
 * Plain C, no Metal/D3D types: shared by the PE side (MinGW) and the unix side
 * (ObjC). Keep the layout identical on both (all fields fixed-width). */

#ifndef __WINE_OPENXR_DMSUBST_H
#define __WINE_OPENXR_DMSUBST_H

#include <stdint.h>

enum dmsubst_op
{
    DMSUBST_OP_DETECT = 1,   /* out: detect_flags */
    DMSUBST_OP_INSTALL,      /* install the Metal swizzles (idempotent) */
    DMSUBST_OP_ARM,          /* arm this thread: mtl_texture = substitute, 0 + flags PROBE = log only */
    DMSUBST_OP_DISARM,       /* disarm; out: captured, seen, desc_*, ... */
    DMSUBST_OP_READBACK,     /* blit one texel of mtl_texture (slice, x, y) to CPU; out: raw, rgba.
                              * in: if event != 0, the blit's command buffer first GPU-waits
                              * for event >= event_value (what the runtime's queue does) */
    DMSUBST_OP_THREAD,       /* out: thread_id of the calling thread as the unix side sees it */
    DMSUBST_OP_STATS,        /* out: global counters */
    DMSUBST_OP_EVENT_VALUE,  /* mtl_texture = id<MTLSharedEvent>; out: event_value */
    DMSUBST_OP_DUMP,         /* write slice of mtl_texture as a PPM to the path in where[] (GPU-waits on event like READBACK) */
};

#define DMSUBST_ARM_PROBE       0x1  /* count/log creations, do not substitute */
#define DMSUBST_ARM_EVENT       0x2  /* capture the next MTLSharedEvent/MTLEvent D3DMetal creates */
#define DMSUBST_ARM_EVENT_SHARED 0x4 /* ...and hand D3DMetal an MTLSharedEvent where it asked newEvent */
#define DMSUBST_ARM_CAPTURE     0x8  /* with PROBE: keep (+1) the texture D3DMetal itself created */

#define DMSUBST_DETECT_D3DMETAL 0x1  /* D3DMetal.framework is mapped in this process */
#define DMSUBST_DETECT_DXMT     0x2  /* DXMT's winemetal.so is mapped in this process */
#define DMSUBST_DETECT_MTL4     0x4  /* D3DMetal created an MTL4 command queue (after INSTALL) */

struct dmsubst_params
{
    uint32_t op;
    uint32_t flags;
    uint64_t mtl_texture;        /* id<MTLTexture> (ARM, READBACK) */
    uint32_t slice, x, y, pad0;  /* READBACK */

    /* out */
    int32_t  status;             /* 0 ok, <0 error */
    uint32_t detect_flags;
    uint64_t thread_id;          /* pthread_threadid_np of the calling thread */
    uint32_t captured;           /* DISARM: the arm's texture was handed to D3DMetal */
    uint32_t seen;               /* DISARM: texture creations observed on this thread while armed */
    uint32_t global_seen;        /* DISARM: texture creations on ANY thread while armed */
    uint32_t result_is_view;     /* DISARM: a view of mtl_texture was handed out, not the texture */
    uint32_t desc_pixel_format;  /* DISARM: first descriptor D3DMetal asked for while armed */
    uint32_t desc_texture_type;
    uint32_t desc_width, desc_height;
    uint32_t desc_array_length, desc_mips;
    uint64_t desc_usage;
    uint32_t desc_storage_mode;
    uint32_t same_device;        /* DISARM: creating device == mtl_texture.device */
    char     where[48];          /* DISARM: selector that created (or was substituted) */
    uint64_t event;              /* DISARM with ARM_EVENT: captured id<MTLSharedEvent> (+1, never released) */
    uint64_t event_value;        /* EVENT_VALUE */
    uint64_t created_texture;    /* DISARM with ARM_CAPTURE: D3DMetal's own id<MTLTexture> (+1) */

    uint32_t tex_pixel_format;   /* READBACK */
    uint32_t pad1;
    uint8_t  raw[16];            /* READBACK: the texel's bytes */
    float    rgba[4];            /* READBACK: decoded (8-bit unorm / half / float formats) */

    /* STATS */
    uint64_t n_textures, n_heaps, n_resset_add, n_resset_add_subst, n_heap_queries_subst;
    uint64_t n_mtl4_queues, n_classic_queues, n_mtl4_commits, n_classic_cbs;
};

#endif /* __WINE_OPENXR_DMSUBST_H */
