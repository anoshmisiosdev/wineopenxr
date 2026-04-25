/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_LOADER_H
#define __WINE_OPENXR_LOADER_H

#include <stdint.h>
#include "openxr/openxr.h"

typedef struct IMTLD3D11InteropDevice IMTLD3D11InteropDevice;
#ifndef __d3d11_h__
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Texture2D ID3D11Texture2D;
#endif
typedef struct ID3D11DeviceContext4 ID3D11DeviceContext4;
typedef struct ID3D11Fence ID3D11Fence;
#ifndef __d3dcommon_h__
typedef int D3D_FEATURE_LEVEL;
#endif
#ifndef _OBJBASE_H_
typedef struct IUnknown IUnknown;
#endif

#include "loader_thunks.h"

#ifndef __WINEOPENXR_LIST_INLINE
#define __WINEOPENXR_LIST_INLINE

struct list { struct list *next, *prev; };

static inline void list_add_tail(struct list *head, struct list *entry)
{
    entry->prev = head->prev;
    entry->next = head;
    head->prev->next = entry;
    head->prev = entry;
}

static inline void list_remove(struct list *entry)
{
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = entry->prev = entry;
}

#endif /* __WINEOPENXR_LIST_INLINE */

struct init_params
{
    NTSTATUS result;
};

struct is_available_instance_function_params
{
    XrInstance instance;
    const char *name;
    int available;
};

struct create_d3d11_session_params
{
    XrInstance instance;
    XrSystemId system_id;
    XrSession *session;
    void *mtl_device;             /* id<MTLDevice>, owned +1 reference returned to caller */
    void *mtl_command_queue;      /* id<MTLCommandQueue>, owned +1 reference returned to caller */
    void *mtl_listener;           /* MTLSharedEventListener *, owned +1 reference returned to caller */
    XrResult result;
};

struct release_metal_session_params
{
    void *mtl_device;             /* id<MTLDevice>, callee consumes caller's +1 reference */
    void *mtl_command_queue;      /* id<MTLCommandQueue>, callee consumes caller's +1 reference */
    void *mtl_listener;           /* MTLSharedEventListener *, callee consumes caller's +1 reference */
};

struct export_metal_textures_params
{
    XrSwapchain swapchain;
    uint32_t image_count;         /* capacity in, actual count out */
    uint64_t *mtl_textures;       /* borrowed id<MTLTexture> pointers, valid while swapchain lives */
    uint32_t width;
    uint32_t height;
    uint32_t array_size;
    int64_t dxgi_format;
    XrResult result;
};

struct xrEndFrame_params
{
    XrSession session;
    const XrFrameEndInfo *frameEndInfo;
    XrResult result;
    uint64_t gpu_fence_value;
    uint64_t mtl_shared_event;
};

struct wine_XrInstance
{
    /* Keep host_instance first. wine_xrCreateInstance passes &host_instance to
     * the host xrCreateInstance and recovers the wrapper via an offset-0 cast */
    XrInstance host_instance;

    /* Set under primary_session_lock during teardown. A concurrent session
     * create observes the flag and returns XR_ERROR_INSTANCE_LOST rather
     * than producing an orphan */
    int destroying;

    /* Last systemId for which xrGetD3D11GraphicsRequirementsKHR succeeded, or
     * XR_NULL_SYSTEM_ID if never called. xrCreateSession requires the app to
     * call xrGet*GraphicsRequirements* for the graphics API and systemId it
     * binds. Monado would enforce this natively, but src/unix/session.m calls
     * xrGetMetalGraphicsRequirementsKHR inside wine_create_d3d11_session and
     * flips Monado's gotten_requirements, so the bridge must track it before
     * that call. Read and written under primary_session_lock */
    XrSystemId d3d11_requirements_queried_for;
};

struct wine_XrSession
{
    XrSession host_session;
    struct wine_XrInstance *instance;

    void *mtl_device;          /* id<MTLDevice>, owned +1 reference */
    void *mtl_command_queue;   /* id<MTLCommandQueue>, owned +1 reference */
    void *mtl_listener;        /* MTLSharedEventListener *, owned +1 reference */

    IMTLD3D11InteropDevice *dxmt_device;
    ID3D11Device         *d3d11_device;
    ID3D11DeviceContext  *d3d11_context;
    ID3D11DeviceContext4 *d3d11_context4;   /* cached to avoid per-frame QueryInterface */

    ID3D11Fence *gpu_fence;                 /* keeps the MTLSharedEvent behind mtl_shared_event alive */
    volatile int64_t gpu_fence_value;       /* InterlockedIncrement64, monotonic */
    uint64_t mtl_shared_event;              /* borrowed id<MTLSharedEvent>, valid while gpu_fence lives */

    struct list swapchain_list;

#ifdef _WIN32
    /* Guards swapchain_list mutations and the first-fill of
     * wine_XrSwapchain::images against concurrent callers */
    CRITICAL_SECTION swapchain_lock;
#endif
};

struct wine_XrSwapchain
{
    XrSwapchain host_swapchain;
    struct wine_XrSession *session;
    struct list entry;

    /* Subset of XrSwapchainCreateInfo. Copying the whole struct would dangle
     * on the caller's next-chain pointer, which we do not own */
    struct
    {
        uint32_t width;
        uint32_t height;
        uint32_t arraySize;
        uint32_t mipCount;
        int64_t format;
        XrSwapchainUsageFlags usageFlags;
    } create_info;

    XrSwapchainImageBaseHeader *images;
    uint32_t image_count;

    volatile int64_t pending_fence_value;
};

static inline struct wine_XrInstance *wine_instance_from_handle(XrInstance handle)
{
    return (struct wine_XrInstance *)(uintptr_t)handle;
}

static inline struct wine_XrSession *wine_session_from_handle(XrSession handle)
{
    return (struct wine_XrSession *)(uintptr_t)handle;
}

static inline struct wine_XrSwapchain *wine_swapchain_from_handle(XrSwapchain handle)
{
    return (struct wine_XrSwapchain *)(uintptr_t)handle;
}

void *wine_xr_get_instance_proc_addr(const char *name);

#ifdef _WIN32
#include <windows.h>

typedef uint64_t unixlib_handle_t;
extern unixlib_handle_t __wineopenxr_unixlib_handle;
extern __declspec(dllimport) NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t, unsigned int, void *);

NTSTATUS WINAPI init_unix_call(void);

#define UNIX_CALL(code, params) \
    __wine_unix_call_dispatcher(__wineopenxr_unixlib_handle, unix_##code, (params))
#endif

#endif /* __WINE_OPENXR_LOADER_H */
