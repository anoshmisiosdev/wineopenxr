/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_BRIDGE_H
#define __WINE_OPENXR_BRIDGE_H

#include <stdint.h>
#include "openxr/openxr.h"

#ifndef __d3d11_h__
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Texture2D ID3D11Texture2D;
#endif
typedef struct IDXGIKeyedMutex IDXGIKeyedMutex;
#ifndef __d3dcommon_h__
typedef int D3D_FEATURE_LEVEL;
#endif
#ifndef _OBJBASE_H_
typedef struct IUnknown IUnknown;
#endif

#include "unixcall.h"

struct openxr_function
{
    const char *name;
    void *pfn;
    int extension;
    uint32_t core_minor;
    int global;
};

const struct openxr_function *wine_xr_find_function(const char *name);
int wine_xr_extension_index(const char *name);

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

/* Length of a DXMT shared-resource mach service name, including the NUL.
 * Fixed by DXMT's on-the-wire private runtime data (char[54]) */
#define OXR_MACH_NAME_LEN 54

struct create_d3d11_session_params
{
    XrInstance instance;
    XrSystemId system_id;
    XrSession *session;
    /* in: launchd service name of the MTLSharedEvent behind the session's
     * keyed-mutex sync carrier, or an empty string for no GPU fence. The unix
     * side looks it up and opens its own id<MTLSharedEvent> on the same event */
    char fence_mach_port_name[OXR_MACH_NAME_LEN];
    void *mtl_device;             /* id<MTLDevice>, owned +1 reference returned to caller */
    void *mtl_command_queue;      /* id<MTLCommandQueue>, owned +1 reference returned to caller */
    uint64_t mtl_shared_event;    /* id<MTLSharedEvent>, owned +1 reference returned to caller */
    XrResult result;
};

struct release_metal_session_params
{
    void *mtl_device;             /* id<MTLDevice>, callee consumes caller's +1 reference */
    void *mtl_command_queue;      /* id<MTLCommandQueue>, callee consumes caller's +1 reference */
    uint64_t mtl_shared_event;    /* id<MTLSharedEvent>, callee consumes caller's +1 reference */
};

struct export_metal_textures_params
{
    XrSwapchain swapchain;
    uint32_t image_count;         /* capacity in, actual count out */
    uint64_t *mtl_textures;       /* borrowed id<MTLTexture> pointers, valid while swapchain lives */
    /* out: per-image launchd service name the unix side registered the
     * image's IOSurface mach port under, for DXMT's OpenSharedResource to
     * look up. image_count entries, NUL-terminated */
    char (*mach_port_names)[OXR_MACH_NAME_LEN];
    uint32_t width;
    uint32_t height;
    uint32_t array_size;
    int64_t dxgi_format;
    XrResult result;
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

    XrVersion api_version;
    uint64_t enabled_extensions[(XR_BRIDGE_EXTENSION_COUNT + 63) / 64];
};

struct wine_XrSession
{
    XrSession host_session;
    struct wine_XrInstance *instance;

    void *mtl_device;          /* id<MTLDevice>, owned +1 reference */
    void *mtl_command_queue;   /* id<MTLCommandQueue>, owned +1 reference */

    ID3D11Device         *d3d11_device;
    ID3D11DeviceContext  *d3d11_context;

    /* 1x1 keyed-mutex texture used only as a sync carrier: releasing its
     * mutex makes DXMT signal mtl_shared_event on its own command queue,
     * ordered after everything the app has encoded. See d3dkmt_interop.h */
    ID3D11Texture2D *sync_carrier;
    IDXGIKeyedMutex *sync_mutex;
    int64_t gpu_fence_value;                /* signalled value, guarded by swapchain_lock */
    uint64_t mtl_shared_event;              /* id<MTLSharedEvent>, +1 owned by the unix side */
    int gpu_fence_checked;                  /* unix side: first-release sanity check done */

    /* Our own D3DKMT adapter/device, used to mint the shared-resource records
     * that stock DXMT's OpenSharedResource imports. See d3dkmt_interop.h.
     * kmt_ready is set only once the layout probe has passed */
    uint32_t kmt_adapter;
    uint32_t kmt_device;
    int kmt_ready;

    /* PROTOTYPE (dmsubst.h): nonzero when the app's D3D11 device is D3DMetal's
     * and swapchain images are D3DMetal textures substituted with the runtime's
     * own MTLTextures. dms_query is an ID3D11Query (D3D11_QUERY_EVENT) the PE
     * side CPU-waits on at release time: the correct-but-slow sync.
     * dms_fence/dms_ctx4 (ID3D11Fence / ID3D11DeviceContext4) are the GPU sync:
     * D3DMetal's fence is backed by an MTLSharedEvent we substituted into
     * CreateFence, stored in mtl_shared_event below */
    int dmsubst;
    void *dms_query;
    void *dms_fence;
    void *dms_ctx4;

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
    /* PROTOTYPE: the runtime's id<MTLTexture> per image (borrowed) */
    uint64_t *mtl_textures;
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

#ifdef _WIN32
#include <windows.h>

typedef uint64_t unixlib_handle_t;
extern unixlib_handle_t __wineopenxr_unixlib_handle;
extern __declspec(dllimport) NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t, unsigned int, void *);

NTSTATUS WINAPI init_unix_call(void);

#define UNIX_CALL(code, params) \
    __wine_unix_call_dispatcher(__wineopenxr_unixlib_handle, unix_##code, (params))
#endif

#endif /* __WINE_OPENXR_BRIDGE_H */
