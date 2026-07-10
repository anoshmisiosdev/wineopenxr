/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* Must precede any header that includes openxr_platform.h */
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>

#include <wine/debug.h>

#include "bridge.h"
#include "openxr/openxr_loader_negotiation.h"
#include "dxmt.h"
#include "formats.h"
#include "events.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif

__declspec(dllimport) NTSTATUS WINAPI NtQueryVirtualMemory(
    HANDLE ProcessHandle,
    const void *BaseAddress,
    int MemoryInformationClass,
    void *MemoryInformation,
    SIZE_T MemoryInformationLength,
    SIZE_T *ReturnLength);

#define MEMORY_WINE_UNIX_FUNCS 1000

unixlib_handle_t __wineopenxr_unixlib_handle = 0;

NTSTATUS WINAPI init_unix_call(void)
{
    extern IMAGE_DOS_HEADER __ImageBase;
    return NtQueryVirtualMemory(
        GetCurrentProcess(),
        &__ImageBase,
        MEMORY_WINE_UNIX_FUNCS,
        &__wineopenxr_unixlib_handle,
        sizeof(__wineopenxr_unixlib_handle),
        NULL);
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;

    if (fdwReason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(hInstDLL);

    if (init_unix_call() != STATUS_SUCCESS)
    {
        WINE_ERR("init_unix_call failed; not loaded as a Wine builtin?\n");
        return FALSE;
    }

    WINE_TRACE("DllMain OK, handle=0x%llx\n",
               (unsigned long long)__wineopenxr_unixlib_handle);
    return TRUE;
}

static CRITICAL_SECTION primary_session_lock = {NULL, -1, 0, 0, 0, 0};
static struct wine_XrSession *primary_session;

XrResult WINAPI xrGetInstanceProcAddr(XrInstance instance,
                                      const char *name,
                                      PFN_xrVoidFunction *function)
{
    const struct openxr_function *entry;
    struct wine_XrInstance *wine_instance;

    WINE_TRACE("instance=%p name=%s\n",
               (void *)(uintptr_t)instance, wine_dbgstr_a(name));

    if (!function)
        return XR_ERROR_VALIDATION_FAILURE;
    *function = NULL;
    if (!name)
        return XR_ERROR_VALIDATION_FAILURE;

    if (!strcmp(name, "xrInitializeLoaderKHR"))
        return XR_ERROR_FUNCTION_UNSUPPORTED;

    entry = wine_xr_find_function(name);

    if (!instance)
    {
        if (entry && entry->global)
        {
            *function = (PFN_xrVoidFunction)entry->pfn;
            return XR_SUCCESS;
        }
        return XR_ERROR_HANDLE_INVALID;
    }

    if (!entry)
        return XR_ERROR_FUNCTION_UNSUPPORTED;

    wine_instance = wine_instance_from_handle(instance);
    if (entry->extension >= 0)
    {
        if (!(wine_instance->enabled_extensions[entry->extension / 64]
              & (1ull << (entry->extension % 64))))
            return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    else if (XR_VERSION_MINOR(wine_instance->api_version) < entry->core_minor)
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    *function = (PFN_xrVoidFunction)entry->pfn;
    return XR_SUCCESS;
}

XrResult WINAPI xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput,
                                              uint32_t *propertyCountOutput,
                                              XrApiLayerProperties *properties)
{
    (void)propertyCapacityInput;
    (void)properties;

    if (!propertyCountOutput)
        return XR_ERROR_VALIDATION_FAILURE;
    *propertyCountOutput = 0;
    return XR_SUCCESS;
}

__declspec(dllexport)
XrResult WINAPI xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo *loaderInfo,
    XrNegotiateRuntimeRequest *runtimeRequest)
{
    struct init_params init_params;
    NTSTATUS status;

    WINE_TRACE("loaderInfo=%p runtimeRequest=%p\n",
               (void *)loaderInfo, (void *)runtimeRequest);

    if (!loaderInfo || !runtimeRequest)
        return XR_ERROR_INITIALIZATION_FAILED;

    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo))
    {
        WINE_ERR("negotiate: loader info validation failed\n");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
    {
        WINE_ERR("negotiate: interface version check failed\n");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest))
    {
        WINE_ERR("negotiate: request validation failed: "
                 "type=%u ver=%u size=%u\n",
                 runtimeRequest->structType, runtimeRequest->structVersion,
                 (unsigned)runtimeRequest->structSize);
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    status = UNIX_CALL(init, &init_params);
    if (status)
    {
        WINE_ERR("init unix call failed: 0x%x\n", (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (init_params.result != STATUS_SUCCESS)
    {
        WINE_ERR("Unix init failed: 0x%x\n", (unsigned)init_params.result);
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->getInstanceProcAddr = (PFN_xrGetInstanceProcAddr)xrGetInstanceProcAddr;
    runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;

    WINE_TRACE("negotiate: success\n");
    return XR_SUCCESS;
}

XrResult WINAPI xrCreateInstance(const XrInstanceCreateInfo *createInfo,
                                 XrInstance *instance)
{
    struct wine_XrInstance *wine_instance;
    struct xrCreateInstance_params params;
    uint32_t extension_index;
    NTSTATUS status;

    WINE_TRACE("createInfo=%p, instance=%p\n", createInfo, instance);

    if (!createInfo || !instance)
        return XR_ERROR_VALIDATION_FAILURE;

    wine_instance = calloc(1, sizeof(*wine_instance));
    if (!wine_instance)
        return XR_ERROR_OUT_OF_MEMORY;

    params.createInfo = createInfo;
    params.instance = &wine_instance->host_instance;

    status = UNIX_CALL(xrCreateInstance, &params);
    if (status)
    {
        WINE_ERR("xrCreateInstance unix call failed: 0x%x\n", (unsigned)status);
        free(wine_instance);
        return XR_ERROR_RUNTIME_FAILURE;
    }

    if (params.result != XR_SUCCESS)
    {
        WINE_WARN("xrCreateInstance failed: %d\n", params.result);
        free(wine_instance);
        return params.result;
    }

    wine_instance->api_version = createInfo->applicationInfo.apiVersion;
    for (extension_index = 0;
         extension_index < createInfo->enabledExtensionCount;
         extension_index++)
    {
        int bit = wine_xr_extension_index(createInfo->enabledExtensionNames[extension_index]);
        if (bit >= 0)
            wine_instance->enabled_extensions[bit / 64] |= 1ull << (bit % 64);
    }

    *instance = (XrInstance)wine_instance;
    WINE_TRACE("Created instance %p (host=%p)\n",
               wine_instance, (void *)(uintptr_t)wine_instance->host_instance);
    return XR_SUCCESS;
}

static void release_dxmt_refs(struct wine_XrSession *sess);
static void release_session_com_refs(struct wine_XrSession *sess);
static void release_metal_session_handles(struct wine_XrSession *sess);
static void free_session(struct wine_XrSession *sess);
static void abandon_partial_session(struct wine_XrSession *sess);
static void release_imported_d3d11_textures(XrSwapchainImageD3D11KHR *images,
                                            uint32_t count);

XrResult WINAPI xrDestroyInstance(XrInstance instance)
{
    struct wine_XrInstance *wine_instance = wine_instance_from_handle(instance);
    struct xrDestroyInstance_params params = {.instance = instance};
    struct wine_XrSession *doomed = NULL;
    NTSTATUS status;

    WINE_TRACE("instance=%p\n", wine_instance);

    if (!instance) {
        return XR_ERROR_HANDLE_INVALID;
    }

    /* Set destroying under primary_session_lock so a concurrent
     * xrCreateSession that is about to publish into primary_session either
     * sees the flag and bails, or we see its published pointer here.
     * No in-between window */
    EnterCriticalSection(&primary_session_lock);
    wine_instance->destroying = 1;
    if (primary_session && primary_session->instance == wine_instance)
    {
        doomed = primary_session;
        primary_session = NULL;
    }
    LeaveCriticalSection(&primary_session_lock);

    /* Teardown order.
     *   1. D3D11, DXMT, and fence Release before the native destroy. App
     *      D3D11 texture wrappers point into native-owned MTLTextures. We
     *      must drop our retains while those MTLTextures are still live,
     *      otherwise the wrappers dangle when native xrDestroyInstance
     *      tears down the compositor-owned backing.
     *   2. Native xrDestroyInstance.
     *   3. Metal device, command queue, and shared-event listener Release
     *      on the Unix side. These are app-retained (see
     *      wine_create_d3d11_session), not runtime-owned, so their
     *      lifetime is independent of native destroy.
     *
     * Safety depends on the OpenXR spec contract that xrDestroyInstance is
     * called after xrDestroySession, which in turn follows xrEndSession. A
     * well-behaved app has drained GPU work by the time we land here.
     * Apps that violate this ordering are in UB per spec.
     *
     * Runs without primary_session_lock because D3D11 and DXMT Release
     * paths can reenter (debug-layer callbacks, app-installed COM hooks)
     * and xrPollEvent would want the lock too */
    if (doomed)
        release_session_com_refs(doomed);

    status = UNIX_CALL(xrDestroyInstance, &params);
    if (status)
        WINE_ERR("xrDestroyInstance unix call failed: 0x%x\n", (unsigned)status);
    if (params.result != XR_SUCCESS)
        WINE_WARN("xrDestroyInstance failed: %d\n", params.result);

    if (doomed)
    {
        release_metal_session_handles(doomed);
        free_session(doomed);
    }

    free(wine_instance);

    if (status)
        return XR_ERROR_RUNTIME_FAILURE;
    return params.result;
}

static UINT d3d11_bind_flags_from_usage(XrSwapchainUsageFlags flags)
{
    UINT ret = 0;

    if (flags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
        ret |= D3D11_BIND_RENDER_TARGET;
    if (flags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        ret |= D3D11_BIND_DEPTH_STENCIL;
    if (flags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
        ret |= D3D11_BIND_UNORDERED_ACCESS;
    if (flags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)
        ret |= D3D11_BIND_SHADER_RESOURCE;

    return ret;
}

static void release_imported_d3d11_textures(XrSwapchainImageD3D11KHR *images,
                                            uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        ID3D11Texture2D_Release(images[i].texture);
}

static void release_swapchain_d3d11_refs(struct wine_XrSession *sess)
{
    struct list *sc_ptr;
    for (sc_ptr = sess->swapchain_list.next;
         sc_ptr != &sess->swapchain_list;
         sc_ptr = sc_ptr->next)
    {
        struct wine_XrSwapchain *sc =
            CONTAINING_RECORD(sc_ptr, struct wine_XrSwapchain, entry);
        if (sc->images)
            release_imported_d3d11_textures((XrSwapchainImageD3D11KHR *)sc->images,
                                            sc->image_count);
    }
}

static void free_swapchain_wrappers(struct wine_XrSession *sess)
{
    struct list *sc_ptr, *sc_next;
    for (sc_ptr = sess->swapchain_list.next;
         sc_ptr != &sess->swapchain_list;
         sc_ptr = sc_next)
    {
        struct wine_XrSwapchain *sc =
            CONTAINING_RECORD(sc_ptr, struct wine_XrSwapchain, entry);
        sc_next = sc_ptr->next;
        free(sc->images);
        free(sc);
    }
}

static void release_dxmt_refs(struct wine_XrSession *sess)
{
    if (sess->d3d11_context4)
    {
        sess->d3d11_context4->lpVtbl->Release(sess->d3d11_context4);
        sess->d3d11_context4 = NULL;
    }
    if (sess->dxmt_device)
    {
        sess->dxmt_device->lpVtbl->Release(sess->dxmt_device);
        sess->dxmt_device = NULL;
    }
    if (sess->d3d11_context)
    {
        sess->d3d11_context->lpVtbl->Release(sess->d3d11_context);
        sess->d3d11_context = NULL;
    }
    if (sess->d3d11_device)
    {
        sess->d3d11_device->lpVtbl->Release(sess->d3d11_device);
        sess->d3d11_device = NULL;
    }
}

static void release_session_com_refs(struct wine_XrSession *sess)
{
    /* Serialize with any concurrent xrCreateSwapchain / xrDestroySwapchain
     * on this session. CRITICAL_SECTION is reentrant */
    EnterCriticalSection(&sess->swapchain_lock);
    release_swapchain_d3d11_refs(sess);
    LeaveCriticalSection(&sess->swapchain_lock);

    if (sess->gpu_fence)
    {
        ((IUnknown *)sess->gpu_fence)->lpVtbl->Release((IUnknown *)sess->gpu_fence);
        sess->gpu_fence = NULL;
    }

    release_dxmt_refs(sess);
}

static void release_metal_session_handles(struct wine_XrSession *sess)
{
    if (sess->mtl_command_queue || sess->mtl_device || sess->mtl_listener)
    {
        struct release_metal_session_params params = {
            .mtl_device = sess->mtl_device,
            .mtl_command_queue = sess->mtl_command_queue,
            .mtl_listener = sess->mtl_listener,
        };
        UNIX_CALL(release_metal_session, &params);
        sess->mtl_device = NULL;
        sess->mtl_command_queue = NULL;
        sess->mtl_listener = NULL;
    }
}

static void free_session(struct wine_XrSession *sess)
{
    free_swapchain_wrappers(sess);
    DeleteCriticalSection(&sess->swapchain_lock);
    free(sess);
}

/* Tear down a session that was half-built when xrCreateSession detected a
 * race with xrDestroyInstance. Mirrors xrDestroySession but skips the
 * primary_session clear (we never published) and tolerates missing state */
static void abandon_partial_session(struct wine_XrSession *sess)
{
    struct xrDestroySession_params dparams = {.session = (XrSession)sess};
    if (sess->host_session)
    {
        NTSTATUS s = UNIX_CALL(xrDestroySession, &dparams);
        if (s)
            WINE_ERR("xrDestroySession unix call failed during abandon: 0x%x\n",
                     (unsigned)s);
    }
    release_session_com_refs(sess);
    release_metal_session_handles(sess);
    free_session(sess);
}

static XrResult create_session_d3d11(struct wine_XrInstance *wine_instance,
                                     const XrGraphicsBindingD3D11KHR *binding,
                                     XrSystemId system_id,
                                     struct wine_XrSession *wine_session)
{
    struct create_d3d11_session_params params;
    NTSTATUS status;
    HRESULT hr;

    if (!binding->device)
    {
        WINE_WARN("D3D11 binding has NULL device\n");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    hr = ID3D11Device_QueryInterface(binding->device,
        &IID_IMTLD3D11InteropDevice, (void **)&wine_session->dxmt_device);
    if (FAILED(hr))
    {
        WINE_ERR("D3D11 device does not support IMTLD3D11InteropDevice "
                 "(not DXMT?)\n");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    wine_session->d3d11_device = binding->device;
    ID3D11Device_AddRef(wine_session->d3d11_device);
    ID3D11Device_GetImmediateContext(wine_session->d3d11_device,
                                     &wine_session->d3d11_context);

    /* Cache ID3D11DeviceContext4 so the per-frame Signal call does not
     * hit the DXMT QueryInterface vtable hop. DXMT always implements
     * ID3D11DeviceContext4 */
    hr = ID3D11DeviceContext_QueryInterface(wine_session->d3d11_context,
        &IID_ID3D11DeviceContext4, (void **)&wine_session->d3d11_context4);
    if (FAILED(hr))
        wine_session->d3d11_context4 = NULL;

    params.instance = (XrInstance)wine_instance;
    params.system_id = system_id;
    params.session = &wine_session->host_session;

    status = UNIX_CALL(create_d3d11_session, &params);
    if (status)
    {
        WINE_ERR("create_d3d11_session unix call failed: 0x%x\n", (unsigned)status);
        release_dxmt_refs(wine_session);
        return XR_ERROR_RUNTIME_FAILURE;
    }

    if (params.result != XR_SUCCESS)
    {
        release_dxmt_refs(wine_session);
        return params.result;
    }

    wine_session->mtl_device = params.mtl_device;
    wine_session->mtl_command_queue = params.mtl_command_queue;
    wine_session->mtl_listener = params.mtl_listener;

    /* Plumb the MTLSharedEvent through an ID3D11Fence so per-frame
     * Signal calls land in a native Metal event the compositor can wait
     * on without a GPU sync */
    {
        ID3D11Device5 *device5 = NULL;
        hr = ID3D11Device_QueryInterface(wine_session->d3d11_device,
            &IID_ID3D11Device5, (void **)&device5);
        if (SUCCEEDED(hr))
        {
            ID3D11Fence *fence = NULL;
            hr = ID3D11Device5_CreateFence(device5, 0, D3D11_FENCE_FLAG_NONE,
                                           &IID_ID3D11Fence, (void **)&fence);
            if (SUCCEEDED(hr))
            {
                UINT64 mtl_event = 0;
                hr = wine_session->dxmt_device->lpVtbl->GetFenceSharedEvent(
                    wine_session->dxmt_device, fence, &mtl_event);
                if (SUCCEEDED(hr) && mtl_event)
                {
                    wine_session->gpu_fence = fence;
                    wine_session->gpu_fence_value = 0;
                    wine_session->mtl_shared_event = mtl_event;
                }
                else
                {
                    WINE_TRACE("GetFenceSharedEvent failed: 0x%08x\n",
                               (unsigned)hr);
                    ID3D11Fence_Release(fence);
                }
            }
            else
            {
                WINE_TRACE("CreateFence failed: 0x%08x\n", (unsigned)hr);
            }
            ID3D11Device5_Release(device5);
        }
        else
        {
            WINE_TRACE("QueryInterface ID3D11Device5 failed: 0x%08x\n",
                       (unsigned)hr);
        }
    }

    return XR_SUCCESS;
}

static XrResult result_with_system_id_precedence(XrInstance instance,
                                                 XrSystemId system_id,
                                                 XrResult fallback)
{
    XrSystemProperties properties = {
        .type = XR_TYPE_SYSTEM_PROPERTIES,
    };
    struct xrGetSystemProperties_params params = {
        .instance = instance,
        .systemId = system_id,
        .properties = &properties,
    };
    NTSTATUS status = UNIX_CALL(xrGetSystemProperties, &params);

    if (status)
    {
        WINE_ERR("xrGetSystemProperties unix call failed: 0x%x\n", (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }

    /* Promote only SYSTEM_INVALID. Other native results fall back to the
     * caller's validation error because this is a probe, not the real call */
    return params.result == XR_ERROR_SYSTEM_INVALID ? XR_ERROR_SYSTEM_INVALID : fallback;
}

XrResult WINAPI xrCreateSession(XrInstance instance,
                                const XrSessionCreateInfo *createInfo,
                                XrSession *session)
{
    struct wine_XrInstance *wine_instance = wine_instance_from_handle(instance);
    struct wine_XrSession *wine_session;
    const XrBaseInStructure *chain;
    const XrGraphicsBindingD3D11KHR *d3d11_binding = NULL;
    XrSystemId queried;
    XrResult result;

    WINE_TRACE("instance=%p, createInfo=%p\n", wine_instance, createInfo);

    /* Reject a second concurrent session up front. Checked again after
     * create_session_d3d11 to catch a race with xrDestroyInstance */
    EnterCriticalSection(&primary_session_lock);
    if (primary_session)
    {
        LeaveCriticalSection(&primary_session_lock);
        WINE_WARN("xrCreateSession rejected: session already live\n");
        return XR_ERROR_LIMIT_REACHED;
    }
    if (wine_instance->destroying)
    {
        LeaveCriticalSection(&primary_session_lock);
        return XR_ERROR_INSTANCE_LOST;
    }
    LeaveCriticalSection(&primary_session_lock);

    for (chain = (const XrBaseInStructure *)createInfo->next; chain; chain = chain->next)
    {
        if (chain->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
        {
            d3d11_binding = (const XrGraphicsBindingD3D11KHR *)chain;
            break;
        }
    }

    if (!d3d11_binding)
    {
        /* Probe systemId first so invalid systems return XR_ERROR_SYSTEM_INVALID
         * before the missing D3D11 binding error */
        result = result_with_system_id_precedence(
            instance, createInfo->systemId, XR_ERROR_GRAPHICS_DEVICE_INVALID);
        if (result == XR_ERROR_GRAPHICS_DEVICE_INVALID)
            WINE_ERR("No D3D11 graphics binding in createInfo->next chain\n");
        else
            WINE_WARN("xrCreateSession failed: %d\n", result);
        return result;
    }

    EnterCriticalSection(&primary_session_lock);
    queried = wine_instance->d3d11_requirements_queried_for;
    LeaveCriticalSection(&primary_session_lock);

    if (queried != createInfo->systemId)
    {
        /* Probe systemId first so invalid systems return XR_ERROR_SYSTEM_INVALID
         * before GRAPHICS_REQUIREMENTS_CALL_MISSING */
        result = result_with_system_id_precedence(
            instance, createInfo->systemId, XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
        WINE_WARN("xrCreateSession failed: %d\n", result);
        return result;
    }

    wine_session = calloc(1, sizeof(*wine_session));
    if (!wine_session)
        return XR_ERROR_OUT_OF_MEMORY;

    wine_session->instance = wine_instance;
    InitializeCriticalSection(&wine_session->swapchain_lock);
    wine_session->swapchain_list.next = &wine_session->swapchain_list;
    wine_session->swapchain_list.prev = &wine_session->swapchain_list;

    result = create_session_d3d11(wine_instance, d3d11_binding,
                                  createInfo->systemId, wine_session);
    if (result != XR_SUCCESS)
    {
        WINE_WARN("xrCreateSession failed: %d\n", result);
        DeleteCriticalSection(&wine_session->swapchain_lock);
        free(wine_session);
        return result;
    }

    /* Publish under the lock after re-checking destroying. Either
     * xrDestroyInstance has not started yet (we publish, it observes us and
     * tears down), or it is in flight and we abandon. No lost-update window */
    EnterCriticalSection(&primary_session_lock);
    if (wine_instance->destroying || primary_session)
    {
        LeaveCriticalSection(&primary_session_lock);
        WINE_WARN("xrCreateSession raced teardown/duplicate, aborting\n");
        abandon_partial_session(wine_session);
        return wine_instance->destroying ? XR_ERROR_INSTANCE_LOST
                                         : XR_ERROR_LIMIT_REACHED;
    }
    primary_session = wine_session;
    LeaveCriticalSection(&primary_session_lock);

    *session = (XrSession)wine_session;
    WINE_TRACE("Created session %p (host=%p)\n",
               wine_session, (void *)(uintptr_t)wine_session->host_session);
    return XR_SUCCESS;
}

XrResult WINAPI xrDestroySession(XrSession session)
{
    struct wine_XrSession *wine_session = wine_session_from_handle(session);
    struct xrDestroySession_params params = {.session = session};
    NTSTATUS status;

    WINE_TRACE("session=%p\n", wine_session);

    EnterCriticalSection(&primary_session_lock);
    if (primary_session == wine_session)
        primary_session = NULL;
    LeaveCriticalSection(&primary_session_lock);

    status = UNIX_CALL(xrDestroySession, &params);
    if (status)
        WINE_ERR("xrDestroySession unix call failed: 0x%x\n", (unsigned)status);
    else if (params.result != XR_SUCCESS)
        WINE_WARN("xrDestroySession failed: %d\n", params.result);

    /* Commit to full destruction regardless of native result. A zombie
     * session with a live handle but a dead Metal device is worse than a
     * leaked handle */
    release_session_com_refs(wine_session);
    release_metal_session_handles(wine_session);
    free_session(wine_session);

    if (status)
        return XR_ERROR_RUNTIME_FAILURE;
    return params.result;
}

XrResult WINAPI xrCreateSwapchain(XrSession session,
                                  const XrSwapchainCreateInfo *createInfo,
                                  XrSwapchain *swapchain)
{
    struct wine_XrSession *wine_session = wine_session_from_handle(session);
    struct wine_XrSwapchain *wine_swapchain;
    struct xrCreateSwapchain_params params;
    NTSTATUS status;

    wine_swapchain = calloc(1, sizeof(*wine_swapchain));
    if (!wine_swapchain)
        return XR_ERROR_OUT_OF_MEMORY;

    wine_swapchain->session = wine_session;

    /* Capture just the fields PE and Unix actually read after create.
     * Avoids a dangling next-chain hazard from copying the full struct */
    wine_swapchain->create_info.width = createInfo->width;
    wine_swapchain->create_info.height = createInfo->height;
    wine_swapchain->create_info.arraySize = createInfo->arraySize;
    wine_swapchain->create_info.mipCount = createInfo->mipCount;
    wine_swapchain->create_info.format = createInfo->format;
    wine_swapchain->create_info.usageFlags = createInfo->usageFlags;

    params.session = session;
    params.createInfo = createInfo;
    params.swapchain = &wine_swapchain->host_swapchain;

    status = UNIX_CALL(xrCreateSwapchain, &params);
    if (status)
    {
        WINE_ERR("xrCreateSwapchain unix call failed: 0x%x\n", (unsigned)status);
        free(wine_swapchain);
        return XR_ERROR_RUNTIME_FAILURE;
    }

    if (params.result != XR_SUCCESS)
    {
        free(wine_swapchain);
        return params.result;
    }

    EnterCriticalSection(&wine_session->swapchain_lock);
    list_add_tail(&wine_session->swapchain_list, &wine_swapchain->entry);
    LeaveCriticalSection(&wine_session->swapchain_lock);

    *swapchain = (XrSwapchain)wine_swapchain;
    return XR_SUCCESS;
}

XrResult WINAPI xrDestroySwapchain(XrSwapchain swapchain)
{
    struct wine_XrSwapchain *wine_swapchain = wine_swapchain_from_handle(swapchain);
    struct wine_XrSession *wine_session = wine_swapchain->session;
    struct xrDestroySwapchain_params params = {.swapchain = swapchain};
    NTSTATUS status;

    status = UNIX_CALL(xrDestroySwapchain, &params);
    if (status)
        WINE_ERR("xrDestroySwapchain unix call failed: 0x%x\n", (unsigned)status);
    else if (params.result != XR_SUCCESS)
        WINE_WARN("xrDestroySwapchain native returned %d; committing to local teardown\n",
                  params.result);

    /* Once xrDestroySwapchain is called, local wrapper teardown proceeds even
     * if native destroy fails */
    EnterCriticalSection(&wine_session->swapchain_lock);
    list_remove(&wine_swapchain->entry);
    LeaveCriticalSection(&wine_session->swapchain_lock);

    if (wine_swapchain->images)
        release_imported_d3d11_textures((XrSwapchainImageD3D11KHR *)wine_swapchain->images,
                                        wine_swapchain->image_count);
    free(wine_swapchain->images);
    free(wine_swapchain);

    if (status)
        return XR_ERROR_RUNTIME_FAILURE;
    return params.result;
}

XrResult WINAPI xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                           uint32_t imageCapacityInput,
                                           uint32_t *imageCountOutput,
                                           XrSwapchainImageBaseHeader *images)
{
    struct wine_XrSwapchain *wine_swapchain = wine_swapchain_from_handle(swapchain);
    struct wine_XrSession *wine_session = wine_swapchain->session;

    if (imageCapacityInput == 0)
    {
        uint32_t cached_count = 0;
        struct xrEnumerateSwapchainImages_params enum_params;
        NTSTATUS status;

        EnterCriticalSection(&wine_session->swapchain_lock);
        if (wine_swapchain->images)
            cached_count = wine_swapchain->image_count;
        LeaveCriticalSection(&wine_session->swapchain_lock);

        if (cached_count)
        {
            *imageCountOutput = cached_count;
            return XR_SUCCESS;
        }

        memset(&enum_params, 0, sizeof(enum_params));
        enum_params.swapchain = swapchain;
        enum_params.imageCapacityInput = 0;
        enum_params.imageCountOutput = imageCountOutput;
        enum_params.images = NULL;

        status = UNIX_CALL(xrEnumerateSwapchainImages, &enum_params);
        if (status)
        {
            WINE_ERR("xrEnumerateSwapchainImages count query failed: 0x%x\n",
                     (unsigned)status);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        return enum_params.result;
    }

    if (!wine_swapchain->images)
    {
        /* Serialize first-fill against a concurrent caller on the same
         * swapchain. Double-checked after acquiring, and ImportMTLTexture2D
         * runs inside the lock so observers see a consistent set */
        EnterCriticalSection(&wine_session->swapchain_lock);
        if (!wine_swapchain->images)
        {
            struct export_metal_textures_params mt_params;
            uint64_t *mtl_textures;
            XrSwapchainImageD3D11KHR *d3d11_images;
            NTSTATUS status;
            uint32_t i;

            mtl_textures = calloc(imageCapacityInput, sizeof(*mtl_textures));
            if (!mtl_textures)
            {
                LeaveCriticalSection(&wine_session->swapchain_lock);
                return XR_ERROR_OUT_OF_MEMORY;
            }

            memset(&mt_params, 0, sizeof(mt_params));
            mt_params.swapchain = swapchain;
            mt_params.image_count = imageCapacityInput;
            mt_params.mtl_textures = mtl_textures;

            status = UNIX_CALL(export_metal_textures, &mt_params);
            if (status)
            {
                WINE_ERR("export_metal_textures unix call failed: 0x%x\n",
                         (unsigned)status);
                free(mtl_textures);
                LeaveCriticalSection(&wine_session->swapchain_lock);
                return XR_ERROR_RUNTIME_FAILURE;
            }

            if (mt_params.result != XR_SUCCESS)
            {
                free(mtl_textures);
                if (mt_params.result == XR_ERROR_SIZE_INSUFFICIENT)
                    *imageCountOutput = mt_params.image_count;
                LeaveCriticalSection(&wine_session->swapchain_lock);
                return mt_params.result;
            }

            d3d11_images = calloc(mt_params.image_count, sizeof(*d3d11_images));
            if (!d3d11_images)
            {
                free(mtl_textures);
                LeaveCriticalSection(&wine_session->swapchain_lock);
                return XR_ERROR_OUT_OF_MEMORY;
            }

            for (i = 0; i < mt_params.image_count; i++)
            {
                D3D11_TEXTURE2D_DESC1 desc;
                ID3D11Texture2D *texture = NULL;
                HRESULT hr;

                memset(&desc, 0, sizeof(desc));
                desc.Width = mt_params.width;
                desc.Height = mt_params.height;
                /* Must match the MTLTexture's mipmapLevelCount; Monado's Metal
                 * allocator honors create_info.mipCount, and DXMT's
                 * ImportMTLTexture2D rejects the import on any mismatch */
                desc.MipLevels = wine_swapchain->create_info.mipCount
                    ? wine_swapchain->create_info.mipCount : 1;
                desc.ArraySize = mt_params.array_size ? mt_params.array_size : 1;
                desc.Format = (DXGI_FORMAT)dxgi_typeless_parent(mt_params.dxgi_format);
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;

                /* Fall back only when no app usage bit maps to a D3D11 bind flag */
                desc.BindFlags = d3d11_bind_flags_from_usage(
                    wine_swapchain->create_info.usageFlags);
                if (!desc.BindFlags)
                {
                    desc.BindFlags = is_depth_dxgi_format(mt_params.dxgi_format)
                        ? D3D11_BIND_DEPTH_STENCIL
                        : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
                }

                hr = wine_session->dxmt_device->lpVtbl->ImportMTLTexture2D(
                    wine_session->dxmt_device, &desc, mtl_textures[i], &texture);
                if (FAILED(hr))
                {
                    WINE_ERR("ImportMTLTexture2D failed for image %u: 0x%08x\n",
                             i, (unsigned)hr);
                    release_imported_d3d11_textures(d3d11_images, i);
                    free(d3d11_images);
                    free(mtl_textures);
                    LeaveCriticalSection(&wine_session->swapchain_lock);
                    return XR_ERROR_RUNTIME_FAILURE;
                }

                d3d11_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
                d3d11_images[i].next = NULL;
                d3d11_images[i].texture = texture;

                WINE_TRACE("Imported D3D11 texture %p from MTLTexture 0x%llx "
                           "(index %u)\n",
                           texture, (unsigned long long)mtl_textures[i], i);
            }

            free(mtl_textures);

            wine_swapchain->image_count = mt_params.image_count;
            wine_swapchain->images = (XrSwapchainImageBaseHeader *)d3d11_images;
        }
        LeaveCriticalSection(&wine_session->swapchain_lock);
    }

    {
        XrSwapchainImageD3D11KHR *d3d11_images =
            (XrSwapchainImageD3D11KHR *)wine_swapchain->images;
        XrSwapchainImageD3D11KHR *out = (XrSwapchainImageD3D11KHR *)images;
        uint32_t copy_count = wine_swapchain->image_count;
        uint32_t i;

        if (copy_count > imageCapacityInput)
        {
            *imageCountOutput = copy_count;
            return XR_ERROR_SIZE_INSUFFICIENT;
        }

        for (i = 0; i < copy_count; i++)
        {
            out[i].type = d3d11_images[i].type;
            out[i].next = d3d11_images[i].next;
            out[i].texture = d3d11_images[i].texture;
        }

        *imageCountOutput = copy_count;
        return XR_SUCCESS;
    }
}

XrResult WINAPI xrReleaseSwapchainImage(XrSwapchain swapchain,
                                        const XrSwapchainImageReleaseInfo *releaseInfo)
{
    struct wine_XrSwapchain *wine_swapchain = wine_swapchain_from_handle(swapchain);
    struct wine_XrSession *wine_session = wine_swapchain->session;
    struct xrReleaseSwapchainImage_params params = {
        .swapchain = swapchain, .releaseInfo = releaseInfo,
    };
    NTSTATUS status;

    if (wine_session->d3d11_context)
    {
        if (wine_session->gpu_fence)
        {
            LONGLONG value = InterlockedIncrement64(&wine_session->gpu_fence_value);
            HRESULT hr = E_FAIL;

            if (wine_session->d3d11_context4)
                hr = wine_session->d3d11_context4->lpVtbl->Signal(
                    wine_session->d3d11_context4,
                    (ID3D11Fence *)wine_session->gpu_fence, value);

            if (SUCCEEDED(hr))
                InterlockedExchange64(&wine_swapchain->pending_fence_value, value);
            else
                WINE_WARN("GPU fence signal failed: 0x%08x\n", (unsigned)hr);
        }
    }

    status = UNIX_CALL(xrReleaseSwapchainImage, &params);
    if (status)
    {
        WINE_ERR("xrReleaseSwapchainImage unix call failed: 0x%x\n", (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return params.result;
}

typedef union CompositionLayer {
    XrCompositionLayerBaseHeader base;
    XrCompositionLayerProjection projection;
    XrCompositionLayerQuad quad;
    XrCompositionLayerCylinderKHR cylinder;
    XrCompositionLayerEquirectKHR equirect;
    XrCompositionLayerEquirect2KHR equirect2;
    XrCompositionLayerCubeKHR cube;
} CompositionLayer;

static inline void collect_fence_value(struct wine_XrSwapchain *swapchain,
                                       uint64_t *latest_release_fence)
{
    if (swapchain)
    {
        uint64_t fence_value = InterlockedExchange64(&swapchain->pending_fence_value, 0);
        if (fence_value > *latest_release_fence)
            *latest_release_fence = fence_value;
    }
}

static void rewrite_subimage_swapchain(XrSwapchainSubImage *sub_image,
                                       uint64_t *latest_release_fence)
{
    struct wine_XrSwapchain *wine_swapchain;

    wine_swapchain = wine_swapchain_from_handle(sub_image->swapchain);
    sub_image->swapchain = wine_swapchain ? wine_swapchain->host_swapchain
                                          : sub_image->swapchain;
    collect_fence_value(wine_swapchain, latest_release_fence);
}

#define HANDLE_SUBIMAGE_LAYER(xr_type, field_name, src_type)                    \
    case xr_type:                                                                \
    {                                                                             \
        const src_type *src = (const src_type *)layer;                           \
        CompositionLayer *dst = &layer_storage[i];                               \
        dst->field_name = *src;                                                  \
        rewrite_subimage_swapchain(&dst->field_name.subImage,                    \
                                   &latest_release_fence);                       \
        host_layers[i] = &dst->base;                                             \
        break;                                                                    \
    }

XrResult WINAPI xrEndFrame(XrSession session,
                           const XrFrameEndInfo *frameEndInfo)
{
    struct wine_XrSession *wine_session = wine_session_from_handle(session);
    struct xrEndFrame_params params;
    XrFrameEndInfo our_end_info;
    CompositionLayer *layer_storage = NULL;
    const XrCompositionLayerBaseHeader **host_layers = NULL;
    XrCompositionLayerProjectionView *host_projection_views = NULL;
    XrCompositionLayerDepthInfoKHR *host_depth_infos = NULL;
    uint32_t layer_count;
    size_t total_views = 0;
    size_t view_offset = 0;
    uint64_t latest_release_fence = 0;
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    NTSTATUS status;
    uint32_t i;

    memset(&params, 0, sizeof(params));

    if (!frameEndInfo)
        return XR_ERROR_VALIDATION_FAILURE;

    if (!frameEndInfo->layerCount || !frameEndInfo->layers)
    {
        params.session = session;
        params.frameEndInfo = frameEndInfo;
        status = UNIX_CALL(xrEndFrame, &params);
        if (status)
        {
            WINE_ERR("xrEndFrame unix call failed: 0x%x\n", (unsigned)status);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        return params.result;
    }

    layer_count = frameEndInfo->layerCount;

    for (i = 0; i < layer_count; i++)
    {
        const XrCompositionLayerBaseHeader *layer = frameEndInfo->layers[i];
        if (!layer)
            return XR_ERROR_LAYER_INVALID;
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            total_views += ((const XrCompositionLayerProjection *)layer)->viewCount;
    }

    layer_storage = malloc(layer_count * sizeof(*layer_storage));
    host_layers = malloc(layer_count * sizeof(*host_layers));
    host_projection_views = total_views ? malloc(total_views * sizeof(*host_projection_views)) : NULL;
    host_depth_infos = total_views ? malloc(total_views * sizeof(*host_depth_infos)) : NULL;
    if (!layer_storage || !host_layers
        || (total_views && (!host_projection_views || !host_depth_infos)))
        goto cleanup;

    for (i = 0; i < layer_count; i++)
    {
        const XrCompositionLayerBaseHeader *layer = frameEndInfo->layers[i];

        switch (layer->type)
        {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
        {
            const XrCompositionLayerProjection *src =
                (const XrCompositionLayerProjection *)layer;
            CompositionLayer *dst = &layer_storage[i];
            uint32_t v;

            dst->projection = *src;

            for (v = 0; v < src->viewCount; v++)
            {
                const XrCompositionLayerProjectionView *src_view = &src->views[v];
                XrCompositionLayerProjectionView *dst_view = &host_projection_views[view_offset + v];

                /* Shallow copy preserves allowed next-chain payloads */
                *dst_view = *src_view;
                rewrite_subimage_swapchain(&dst_view->subImage, &latest_release_fence);

                /* Rewrite depth only at the ProjectionView next head. Extension
                 * policy must hide any app-visible view next payload that can
                 * precede a depth subImage */
                if (dst_view->next
                    && ((const XrBaseInStructure *)dst_view->next)->type
                       == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR)
                {
                    const XrCompositionLayerDepthInfoKHR *src_depth =
                        (const XrCompositionLayerDepthInfoKHR *)dst_view->next;
                    XrCompositionLayerDepthInfoKHR *di =
                        &host_depth_infos[view_offset + v];

                    *di = *src_depth;
                    rewrite_subimage_swapchain(&di->subImage, &latest_release_fence);
                    dst_view->next = di;
                }
            }

            dst->projection.views = &host_projection_views[view_offset];
            view_offset += src->viewCount;

            host_layers[i] = &dst->base;
            break;
        }
        HANDLE_SUBIMAGE_LAYER(XR_TYPE_COMPOSITION_LAYER_QUAD,
                              quad, XrCompositionLayerQuad)
        HANDLE_SUBIMAGE_LAYER(XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR,
                              cylinder, XrCompositionLayerCylinderKHR)
        HANDLE_SUBIMAGE_LAYER(XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR,
                              equirect, XrCompositionLayerEquirectKHR)
        HANDLE_SUBIMAGE_LAYER(XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR,
                              equirect2, XrCompositionLayerEquirect2KHR)
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
        {
            /* Cube uses .swapchain directly rather than a subImage */
            const XrCompositionLayerCubeKHR *src =
                (const XrCompositionLayerCubeKHR *)layer;
            CompositionLayer *dst = &layer_storage[i];
            struct wine_XrSwapchain *wine_swapchain;

            dst->cube = *src;
            wine_swapchain = wine_swapchain_from_handle(src->swapchain);
            dst->cube.swapchain = wine_swapchain ? wine_swapchain->host_swapchain
                                                 : src->swapchain;
            collect_fence_value(wine_swapchain, &latest_release_fence);

            host_layers[i] = &dst->base;
            break;
        }
        default:
            WINE_WARN("unsupported composition layer type %u\n", layer->type);
            result = XR_ERROR_LAYER_INVALID;
            goto cleanup;
        }
    }

    our_end_info = *frameEndInfo;
    our_end_info.layers = host_layers;

    params.session = session;
    params.frameEndInfo = &our_end_info;

    if (latest_release_fence && wine_session->mtl_shared_event)
    {
        params.gpu_fence_value = latest_release_fence;
        params.mtl_shared_event = wine_session->mtl_shared_event;
    }

    status = UNIX_CALL(xrEndFrame, &params);
    if (status)
    {
        WINE_ERR("xrEndFrame unix call failed: 0x%x\n", (unsigned)status);
        goto cleanup;
    }
    result = params.result;

cleanup:
    free(host_depth_infos);
    free(host_projection_views);
    free(host_layers);
    free(layer_storage);
    return result;
}

static XrSession wine_session_for_host(XrSession host_session)
{
    XrSession result = XR_NULL_HANDLE;
    EnterCriticalSection(&primary_session_lock);
    if (primary_session && primary_session->host_session == host_session)
        result = (XrSession)primary_session;
    LeaveCriticalSection(&primary_session_lock);
    return result;
}

static BOOL rewrite_event_session(XrEventDataBuffer *eventData, XrSession *session)
{
    XrBaseOutStructure *base = (XrBaseOutStructure *)eventData;
    XrSession *event_session;
    unsigned int i;

    for (i = 0; i < sizeof(xr_session_events) / sizeof(xr_session_events[0]); i++)
    {
        if (xr_session_events[i].type != base->type)
            continue;
        event_session = (XrSession *)((char *)eventData + xr_session_events[i].session_offset);
        *session = wine_session_for_host(*event_session);
        *event_session = *session;
        return TRUE;
    }

    return FALSE;
}

XrResult WINAPI xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData)
{
    struct xrPollEvent_params params;
    NTSTATUS status;
    unsigned int retry;

    /* Suppress a bounded run of stale host-session events that can arrive
     * after the PE session wrapper was destroyed */
    for (retry = 0; retry < 64; retry++)
    {
        params.instance = instance;
        params.eventData = eventData;
        status = UNIX_CALL(xrPollEvent, &params);
        if (status)
        {
            WINE_ERR("xrPollEvent unix call failed: 0x%x\n", (unsigned)status);
            return XR_ERROR_RUNTIME_FAILURE;
        }

        if (params.result != XR_SUCCESS)
            return params.result;

        {
            XrBaseOutStructure *base = (XrBaseOutStructure *)eventData;
            XrSession session = XR_NULL_HANDLE;

            if (!rewrite_event_session(eventData, &session))
                return params.result;
            if (session)
                return params.result;

            WINE_TRACE("suppressing event type %u for unknown session\n", base->type);
        }
    }

    return XR_EVENT_UNAVAILABLE;
}

XrResult WINAPI xrGetD3D11GraphicsRequirementsKHR(XrInstance instance,
                                                  XrSystemId systemId,
                                                  XrGraphicsRequirementsD3D11KHR *graphicsRequirements)
{
    struct wine_XrInstance *wine_instance;
    struct xrGetD3D11GraphicsRequirementsKHR_params params = {
        .instance = instance, .systemId = systemId,
        .graphicsRequirements = graphicsRequirements,
    };
    NTSTATUS status;

    if (!graphicsRequirements)
        return XR_ERROR_VALIDATION_FAILURE;

    wine_instance = wine_instance_from_handle(instance);
    status = UNIX_CALL(xrGetD3D11GraphicsRequirementsKHR, &params);
    if (status)
    {
        WINE_ERR("xrGetD3D11GraphicsRequirementsKHR unix call failed: 0x%x\n",
                 (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (params.result == XR_SUCCESS)
    {
        EnterCriticalSection(&primary_session_lock);
        wine_instance->d3d11_requirements_queried_for = systemId;
        LeaveCriticalSection(&primary_session_lock);
    }
    return params.result;
}

XrResult WINAPI xrConvertTimeToWin32PerformanceCounterKHR(XrInstance instance,
                                                          XrTime time,
                                                          LARGE_INTEGER *performanceCounter)
{
    struct xrConvertTimeToWin32PerformanceCounterKHR_params params = {
        .instance = instance, .time = time,
        .performanceCounter = performanceCounter,
    };
    NTSTATUS status = UNIX_CALL(xrConvertTimeToWin32PerformanceCounterKHR, &params);
    if (status)
    {
        WINE_ERR("xrConvertTimeToWin32PerformanceCounterKHR unix call failed: 0x%x\n",
                 (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return params.result;
}

XrResult WINAPI xrConvertWin32PerformanceCounterToTimeKHR(XrInstance instance,
                                                          const LARGE_INTEGER *performanceCounter,
                                                          XrTime *time)
{
    struct xrConvertWin32PerformanceCounterToTimeKHR_params params = {
        .instance = instance, .performanceCounter = performanceCounter,
        .time = time,
    };
    NTSTATUS status = UNIX_CALL(xrConvertWin32PerformanceCounterToTimeKHR, &params);
    if (status)
    {
        WINE_ERR("xrConvertWin32PerformanceCounterToTimeKHR unix call failed: 0x%x\n",
                 (unsigned)status);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return params.result;
}
