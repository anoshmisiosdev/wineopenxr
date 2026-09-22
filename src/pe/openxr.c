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
#include "d3dkmt_interop.h"
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

static void session_teardown(struct wine_XrSession *session);

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

    status = UNIX_CALL(xrDestroyInstance, &params);
    if (status)
        WINE_ERR("xrDestroyInstance unix call failed: 0x%x\n", (unsigned)status);
    if (params.result != XR_SUCCESS)
        WINE_WARN("xrDestroyInstance failed: %d\n", params.result);

    if (doomed)
        session_teardown(doomed);

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

/* ------------------------------------------------------------------------
 * Metal interop against a STOCK DXMT
 *
 * The OpenXR runtime allocates the swapchain images as Metal *shared*
 * textures and hands us their IOSurface mach ports (registered with launchd
 * under a unique name by the unix half). To turn one into an ID3D11Texture2D
 * we mint the same kind of D3DKMT shared-resource record DXMT writes for its
 * own shared textures, then call the completely standard
 * ID3D11Device::OpenSharedResource. DXMT reads the name out of the record,
 * looks the port up and rebuilds the MTLTexture around it.
 *
 * The record's layout (struct dxmt_shared_resource_data) is DXMT-private, so
 * before relying on it we make DXMT write one for a throwaway 4x4 texture and
 * check every field lands where we expect - see d3dkmt_probe_shared_layout.
 * ------------------------------------------------------------------------ */

static struct d3dkmt_funcs g_kmt;
static int g_kmt_load_attempted;

static int d3dkmt_load(void)
{
    HMODULE gdi32;

    if (g_kmt_load_attempted)
        return g_kmt.gdi32 != NULL;
    g_kmt_load_attempted = 1;

    gdi32 = LoadLibraryA("gdi32.dll");
    if (!gdi32)
    {
        WINE_ERR("LoadLibrary(gdi32.dll) failed\n");
        return 0;
    }

    g_kmt.OpenAdapterFromLuid = (void *)GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
    g_kmt.OpenAdapterFromGdiDisplayName =
        (void *)GetProcAddress(gdi32, "D3DKMTOpenAdapterFromGdiDisplayName");
    g_kmt.CloseAdapter = (void *)GetProcAddress(gdi32, "D3DKMTCloseAdapter");
    g_kmt.CreateDevice = (void *)GetProcAddress(gdi32, "D3DKMTCreateDevice");
    g_kmt.DestroyDevice = (void *)GetProcAddress(gdi32, "D3DKMTDestroyDevice");
    g_kmt.CreateAllocation2 = (void *)GetProcAddress(gdi32, "D3DKMTCreateAllocation2");
    g_kmt.DestroyAllocation = (void *)GetProcAddress(gdi32, "D3DKMTDestroyAllocation");
    g_kmt.QueryResourceInfo = (void *)GetProcAddress(gdi32, "D3DKMTQueryResourceInfo");
    g_kmt.OpenResource2 = (void *)GetProcAddress(gdi32, "D3DKMTOpenResource2");
    g_kmt.OpenKeyedMutex2 = (void *)GetProcAddress(gdi32, "D3DKMTOpenKeyedMutex2");

    if (!g_kmt.OpenAdapterFromLuid || !g_kmt.CloseAdapter || !g_kmt.CreateDevice ||
        !g_kmt.DestroyDevice || !g_kmt.CreateAllocation2 || !g_kmt.DestroyAllocation ||
        !g_kmt.QueryResourceInfo || !g_kmt.OpenResource2 || !g_kmt.OpenKeyedMutex2)
    {
        WINE_ERR("gdi32.dll is missing D3DKMT entry points needed for Metal interop\n");
        FreeLibrary(gdi32);
        return 0;
    }

    g_kmt.gdi32 = gdi32;
    return 1;
}

static void session_kmt_teardown(struct wine_XrSession *session)
{
    session->kmt_ready = 0;
    if (session->kmt_device && g_kmt.DestroyDevice)
    {
        D3DKMT_DESTROYDEVICE destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.hDevice = session->kmt_device;
        g_kmt.DestroyDevice(&destroy);
    }
    session->kmt_device = 0;
    if (session->kmt_adapter && g_kmt.CloseAdapter)
    {
        D3DKMT_CLOSEADAPTER close_adapter;
        memset(&close_adapter, 0, sizeof(close_adapter));
        close_adapter.hAdapter = session->kmt_adapter;
        g_kmt.CloseAdapter(&close_adapter);
    }
    session->kmt_adapter = 0;
}

/* Open a D3DKMT device. It only has to mint and read shared-resource records,
 * never render, so any adapter on this Wine will do - gdi32 keeps one resource
 * namespace per process. Wine's OpenAdapterFromLuid does not recognise the LUID
 * DXMT synthesises from the Metal registry ID, so try the GDI display name
 * first. */
static int session_kmt_init(struct wine_XrSession *session)
{
    D3DKMT_CREATEDEVICE create_device;
    unsigned attempt;

    if (!d3dkmt_load())
        return 0;

    for (attempt = 0; attempt < 5 && !session->kmt_adapter; attempt++)
    {
        if (g_kmt.OpenAdapterFromGdiDisplayName)
        {
            D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open_name;
            memset(&open_name, 0, sizeof(open_name));
            lstrcpynW(open_name.DeviceName, L"\\\\.\\DISPLAY1",
                      sizeof(open_name.DeviceName) / sizeof(WCHAR));
            if (!g_kmt.OpenAdapterFromGdiDisplayName(&open_name) && open_name.hAdapter)
            {
                session->kmt_adapter = open_name.hAdapter;
                break;
            }
        }
        {
            IDXGIDevice *dxgi_device = NULL;
            IDXGIAdapter *dxgi_adapter = NULL;
            DXGI_ADAPTER_DESC adapter_desc;
            D3DKMT_OPENADAPTERFROMLUID open_luid;

            if (FAILED(ID3D11Device_QueryInterface(session->d3d11_device, &IID_IDXGIDevice,
                                                   (void **)&dxgi_device)))
                break;
            if (FAILED(IDXGIDevice_GetAdapter(dxgi_device, &dxgi_adapter)))
            {
                IDXGIDevice_Release(dxgi_device);
                break;
            }
            IDXGIDevice_Release(dxgi_device);
            if (FAILED(IDXGIAdapter_GetDesc(dxgi_adapter, &adapter_desc)))
            {
                IDXGIAdapter_Release(dxgi_adapter);
                break;
            }
            IDXGIAdapter_Release(dxgi_adapter);

            memset(&open_luid, 0, sizeof(open_luid));
            open_luid.AdapterLuid = adapter_desc.AdapterLuid;
            if (!g_kmt.OpenAdapterFromLuid(&open_luid) && open_luid.hAdapter)
                session->kmt_adapter = open_luid.hAdapter;
        }
        if (!session->kmt_adapter)
            Sleep(50);
    }

    if (!session->kmt_adapter)
    {
        WINE_ERR("could not open a D3DKMT adapter\n");
        return 0;
    }

    memset(&create_device, 0, sizeof(create_device));
    create_device.u.hAdapter = session->kmt_adapter;
    if (g_kmt.CreateDevice(&create_device) || !create_device.hDevice)
    {
        WINE_ERR("D3DKMTCreateDevice failed\n");
        session_kmt_teardown(session);
        return 0;
    }
    session->kmt_device = create_device.hDevice;
    session->kmt_ready = 1;
    return 1;
}

/* Read back the DXMT private record of a legacy (global-share) shared resource.
 * D3DKMTQueryResourceInfo only reports sizes; OpenResource2 is what fills the
 * buffer, and it hands back a resource handle we must destroy again */
static int d3dkmt_read_shared_record(struct wine_XrSession *session, HANDLE shared,
                                     struct dxmt_shared_resource_data *out)
{
    D3DKMT_QUERYRESOURCEINFO query;
    D3DKMT_OPENRESOURCE open_resource;
    D3DDDI_OPENALLOCATIONINFO2 allocation;
    D3DKMT_DESTROYALLOCATION destroy;

    memset(out, 0, sizeof(*out));
    memset(&query, 0, sizeof(query));
    query.hDevice = session->kmt_device;
    query.hGlobalShare = (D3DKMT_HANDLE)(ULONG_PTR)shared;
    query.pPrivateRuntimeData = out;
    query.PrivateRuntimeDataSize = sizeof(*out);
    if (g_kmt.QueryResourceInfo(&query))
    {
        WINE_ERR("D3DKMTQueryResourceInfo failed\n");
        return 0;
    }
    if (query.PrivateRuntimeDataSize != sizeof(*out))
    {
        WINE_ERR("DXMT shared-resource record is %u bytes, expected %u; this DXMT "
                 "build changed its private layout and the bridge cannot drive it\n",
                 (unsigned)query.PrivateRuntimeDataSize, (unsigned)sizeof(*out));
        return 0;
    }

    memset(&open_resource, 0, sizeof(open_resource));
    memset(&allocation, 0, sizeof(allocation));
    open_resource.hDevice = session->kmt_device;
    open_resource.hGlobalShare = (D3DKMT_HANDLE)(ULONG_PTR)shared;
    open_resource.NumAllocations = 1;
    open_resource.u.pOpenAllocationInfo2 = &allocation;
    open_resource.pPrivateRuntimeData = out;
    open_resource.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
    if (g_kmt.OpenResource2(&open_resource))
    {
        WINE_ERR("D3DKMTOpenResource2 failed\n");
        return 0;
    }

    memset(&destroy, 0, sizeof(destroy));
    destroy.hDevice = session->kmt_device;
    destroy.hResource = open_resource.hResource;
    g_kmt.DestroyAllocation(&destroy);
    return 1;
}

/* Wrap a launchd-registered Metal shared-texture port in a D3DKMT resource and
 * let stock DXMT import it through ID3D11Device::OpenSharedResource */
static HRESULT import_shared_mtl_texture(struct wine_XrSession *session,
                                         const D3D11_TEXTURE2D_DESC1 *desc,
                                         const char *mach_port_name,
                                         ID3D11Texture2D **out)
{
    struct dxmt_shared_resource_data data;
    D3DKMT_CREATEALLOCATION create;
    D3DDDI_ALLOCATIONINFO2 allocation_info;
    D3DKMT_CREATESTANDARDALLOCATION standard_allocation;
    D3DDDI_ALLOCATIONINFO system_mem;
    D3DKMT_DESTROYALLOCATION destroy;
    HRESULT hr;

    *out = NULL;
    if (!session->kmt_ready)
        return E_FAIL;
    if (!mach_port_name || !mach_port_name[0])
        return E_INVALIDARG;

    memset(&data, 0, sizeof(data));
    lstrcpynA(data.mach_port_name, mach_port_name, sizeof(data.mach_port_name));
    data.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    data.desc.desc2d = *desc;
    data.mutex_handle = 0;

    /* Mirrors DXMT's own CreateDeviceTextureInternal shared path */
    memset(&create, 0, sizeof(create));
    memset(&allocation_info, 0, sizeof(allocation_info));
    memset(&standard_allocation, 0, sizeof(standard_allocation));
    memset(&system_mem, 0, sizeof(system_mem));

    create.hDevice = session->kmt_device;
    create.pPrivateRuntimeData = &data;
    create.PrivateRuntimeDataSize = sizeof(data);
    create.Flags.StandardAllocation = 1;
    create.NumAllocations = 1;
    create.u2.pAllocationInfo2 = &allocation_info;
    create.u.pStandardAllocation = &standard_allocation;
    standard_allocation.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    create.Flags.ExistingSysMem = 1;
    allocation_info.u.pSystemMem = &system_mem;
    create.Flags.CreateResource = 1;
    create.Flags.CreateShared = 1;

    if (g_kmt.CreateAllocation2(&create) || !create.hGlobalShare)
    {
        WINE_ERR("D3DKMTCreateAllocation2 failed for %s\n", wine_dbgstr_a(mach_port_name));
        return E_FAIL;
    }

    hr = ID3D11Device_OpenSharedResource(session->d3d11_device,
                                         (HANDLE)(ULONG_PTR)create.hGlobalShare,
                                         &IID_ID3D11Texture2D, (void **)out);

    /* DXMT's import keeps only the Metal texture (alive via the mach port);
     * the kernel resource has done its job as a carrier */
    memset(&destroy, 0, sizeof(destroy));
    destroy.hDevice = session->kmt_device;
    destroy.hResource = create.hResource;
    g_kmt.DestroyAllocation(&destroy);

    return hr;
}

/* Build the session's GPU sync carrier, and validate DXMT's private record
 * layout while we are at it.
 *
 * A 1x1 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX texture makes stock DXMT create a
 * keyed mutex backed by an MTLSharedEvent and write both the texture record and
 * the mutex's mach service name where D3DKMT can hand them back. Releasing that
 * mutex encodes a signal of the event on DXMT's queue, ordered after everything
 * the app encoded - exactly what ID3D11Fence::Signal would have done, except
 * that shared fences do not work on CrossOver's Wine.
 *
 * The record we read here is the same struct we later write for the runtime's
 * own textures, so checking it field by field is also the layout probe: if a
 * future DXMT moves anything, this fails loudly at session creation instead of
 * silently importing garbage */
static void session_create_sync_carrier(struct wine_XrSession *session,
                                        char name[OXR_MACH_NAME_LEN])
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *texture = NULL;
    IDXGIResource *resource = NULL;
    IDXGIKeyedMutex *mutex = NULL;
    HANDLE shared = NULL;
    struct dxmt_shared_resource_data record;
    const D3D11_TEXTURE2D_DESC1 *probe;
    D3DKMT_OPENKEYEDMUTEX2 open_mutex;
    char mutex_name[OXR_MACH_NAME_LEN];
    HRESULT hr;

    name[0] = 0;

    memset(&desc, 0, sizeof(desc));
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    /* KEYEDMUTEX is mutually exclusive with MISC_SHARED but still yields a
     * legacy global share handle */
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    hr = ID3D11Device_CreateTexture2D(session->d3d11_device, &desc, NULL, &texture);
    if (FAILED(hr))
    {
        WINE_ERR("sync carrier: CreateTexture2D(KEYEDMUTEX) failed: 0x%08x\n", (unsigned)hr);
        return;
    }

    hr = ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIResource, (void **)&resource);
    if (FAILED(hr))
    {
        WINE_ERR("sync carrier: no IDXGIResource: 0x%08x\n", (unsigned)hr);
        goto fail;
    }
    hr = IDXGIResource_GetSharedHandle(resource, &shared);
    IDXGIResource_Release(resource);
    resource = NULL;
    if (FAILED(hr) || !shared)
    {
        WINE_ERR("sync carrier: GetSharedHandle failed: 0x%08x\n", (unsigned)hr);
        goto fail;
    }

    if (!d3dkmt_read_shared_record(session, shared, &record))
        goto fail;

    if (!memchr(record.mach_port_name, 0, sizeof(record.mach_port_name)))
    {
        WINE_ERR("layout check: mach port name is not NUL-terminated\n");
        goto fail;
    }
    if (record.dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        WINE_ERR("layout check: dimension reads %d, expected TEXTURE2D\n",
                 (int)record.dimension);
        goto fail;
    }
    probe = &record.desc.desc2d;
    if (probe->Width != desc.Width || probe->Height != desc.Height ||
        probe->MipLevels != desc.MipLevels || probe->ArraySize != desc.ArraySize ||
        probe->Format != desc.Format || probe->SampleDesc.Count != desc.SampleDesc.Count ||
        probe->Usage != desc.Usage || probe->BindFlags != desc.BindFlags ||
        probe->MiscFlags != desc.MiscFlags)
    {
        WINE_ERR("layout check: desc mismatch (%ux%u mips=%u array=%u fmt=%d "
                 "usage=%d bind=0x%x misc=0x%x)\n",
                 (unsigned)probe->Width, (unsigned)probe->Height,
                 (unsigned)probe->MipLevels, (unsigned)probe->ArraySize,
                 (int)probe->Format, (int)probe->Usage,
                 (unsigned)probe->BindFlags, (unsigned)probe->MiscFlags);
        goto fail;
    }
    if (!(record.mutex_handle & 0xc0000000))
    {
        WINE_ERR("layout check: keyed mutex handle reads 0x%x\n",
                 (unsigned)record.mutex_handle);
        goto fail;
    }

    /* A keyed mutex's private runtime data is just the NUL-terminated mach
     * service name of its MTLSharedEvent. Leave our opened handle alone -
     * DXMT owns the mutex and we only wanted to read the name */
    memset(mutex_name, 0, sizeof(mutex_name));
    memset(&open_mutex, 0, sizeof(open_mutex));
    open_mutex.hSharedHandle = record.mutex_handle;
    open_mutex.pPrivateRuntimeData = mutex_name;
    open_mutex.PrivateRuntimeDataSize = sizeof(mutex_name);
    if (g_kmt.OpenKeyedMutex2(&open_mutex) ||
        !memchr(mutex_name, 0, sizeof(mutex_name)) || !mutex_name[0])
    {
        WINE_ERR("sync carrier: could not read the keyed mutex's shared event name\n");
        goto fail;
    }

    hr = ID3D11Texture2D_QueryInterface(texture, &IID_IDXGIKeyedMutex, (void **)&mutex);
    if (FAILED(hr))
    {
        WINE_ERR("sync carrier: no IDXGIKeyedMutex: 0x%08x\n", (unsigned)hr);
        goto fail;
    }

    session->sync_carrier = texture;
    session->sync_mutex = mutex;
    session->gpu_fence_value = 0;
    memcpy(name, mutex_name, sizeof(mutex_name));
    WINE_TRACE("sync carrier ready, shared event %s\n", wine_dbgstr_a(mutex_name));
    return;

fail:
    if (resource)
        IDXGIResource_Release(resource);
    ID3D11Texture2D_Release(texture);
    WINE_WARN("running without a GPU release fence; the runtime may read a "
              "swapchain image before the app has finished rendering it\n");
}

static void release_imported_d3d11_textures(XrSwapchainImageD3D11KHR *images,
                                            uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        ID3D11Texture2D_Release(images[i].texture);
}

static void release_dxmt_refs(struct wine_XrSession *sess)
{
    session_kmt_teardown(sess);
    if (sess->sync_mutex)
    {
        IDXGIKeyedMutex_Release(sess->sync_mutex);
        sess->sync_mutex = NULL;
    }
    if (sess->sync_carrier)
    {
        ID3D11Texture2D_Release(sess->sync_carrier);
        sess->sync_carrier = NULL;
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

static void session_teardown(struct wine_XrSession *session)
{
    struct list *link, *next_link;

    /* Serialize with any concurrent xrCreateSwapchain / xrDestroySwapchain
     * on this session. CRITICAL_SECTION is reentrant */
    EnterCriticalSection(&session->swapchain_lock);
    for (link = session->swapchain_list.next;
         link != &session->swapchain_list;
         link = next_link)
    {
        struct wine_XrSwapchain *swapchain =
            CONTAINING_RECORD(link, struct wine_XrSwapchain, entry);
        next_link = link->next;
        if (swapchain->images)
            release_imported_d3d11_textures((XrSwapchainImageD3D11KHR *)swapchain->images,
                                            swapchain->image_count);
        free(swapchain->images);
        free(swapchain);
    }
    LeaveCriticalSection(&session->swapchain_lock);

    release_dxmt_refs(session);

    if (session->mtl_command_queue || session->mtl_device || session->mtl_shared_event)
    {
        struct release_metal_session_params params = {
            .mtl_device = session->mtl_device,
            .mtl_command_queue = session->mtl_command_queue,
            .mtl_shared_event = session->mtl_shared_event,
        };
        UNIX_CALL(release_metal_session, &params);
    }

    DeleteCriticalSection(&session->swapchain_lock);
    free(session);
}

static XrResult create_session_d3d11(struct wine_XrInstance *wine_instance,
                                     const XrGraphicsBindingD3D11KHR *binding,
                                     XrSystemId system_id,
                                     struct wine_XrSession *wine_session)
{
    struct create_d3d11_session_params params;
    NTSTATUS status;

    if (!binding->device)
    {
        WINE_WARN("D3D11 binding has NULL device\n");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    wine_session->d3d11_device = binding->device;
    ID3D11Device_AddRef(wine_session->d3d11_device);
    ID3D11Device_GetImmediateContext(wine_session->d3d11_device,
                                     &wine_session->d3d11_context);

    /* Swapchain images arrive as Metal shared textures we import through
     * stock DXMT's OpenSharedResource; without a working D3DKMT path there
     * is nothing to hand the app */
    if (!session_kmt_init(wine_session))
    {
        WINE_ERR("Metal interop unavailable on this D3D11 device (DXMT missing "
                 "or incompatible)\n");
        release_dxmt_refs(wine_session);
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    memset(&params, 0, sizeof(params));
    params.instance = (XrInstance)wine_instance;
    params.system_id = system_id;
    params.session = &wine_session->host_session;

    /* Plumb the MTLSharedEvent behind the keyed-mutex sync carrier through to
     * the unix half, so per-release signals land in a native Metal event the
     * binding queue GPU-waits on before the host reads the swapchain */
    session_create_sync_carrier(wine_session, params.fence_mach_port_name);

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
    wine_session->mtl_shared_event = params.mtl_shared_event;

    if (wine_session->sync_mutex && !wine_session->mtl_shared_event)
    {
        WINE_WARN("the unix half could not open the sync carrier's shared "
                  "event; running without a GPU release fence\n");
        IDXGIKeyedMutex_Release(wine_session->sync_mutex);
        wine_session->sync_mutex = NULL;
        ID3D11Texture2D_Release(wine_session->sync_carrier);
        wine_session->sync_carrier = NULL;
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
        struct xrDestroySession_params destroy_params = {.session = (XrSession)wine_session};
        NTSTATUS destroy_status;

        LeaveCriticalSection(&primary_session_lock);
        WINE_WARN("xrCreateSession raced teardown/duplicate, aborting\n");
        destroy_status = UNIX_CALL(xrDestroySession, &destroy_params);
        if (destroy_status)
            WINE_ERR("xrDestroySession unix call failed during abandon: 0x%x\n",
                     (unsigned)destroy_status);
        session_teardown(wine_session);
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
    session_teardown(wine_session);

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
            char (*mach_port_names)[OXR_MACH_NAME_LEN];
            XrSwapchainImageD3D11KHR *d3d11_images;
            NTSTATUS status;
            uint32_t i;

            mtl_textures = calloc(imageCapacityInput, sizeof(*mtl_textures));
            mach_port_names = calloc(imageCapacityInput, sizeof(*mach_port_names));
            if (!mtl_textures || !mach_port_names)
            {
                free(mtl_textures);
                free(mach_port_names);
                LeaveCriticalSection(&wine_session->swapchain_lock);
                return XR_ERROR_OUT_OF_MEMORY;
            }

            memset(&mt_params, 0, sizeof(mt_params));
            mt_params.swapchain = swapchain;
            mt_params.image_count = imageCapacityInput;
            mt_params.mtl_textures = mtl_textures;
            mt_params.mach_port_names = mach_port_names;

            status = UNIX_CALL(export_metal_textures, &mt_params);
            if (status)
            {
                WINE_ERR("export_metal_textures unix call failed: 0x%x\n",
                         (unsigned)status);
                free(mtl_textures);
                free(mach_port_names);
                LeaveCriticalSection(&wine_session->swapchain_lock);
                return XR_ERROR_RUNTIME_FAILURE;
            }

            if (mt_params.result != XR_SUCCESS)
            {
                free(mtl_textures);
                free(mach_port_names);
                if (mt_params.result == XR_ERROR_SIZE_INSUFFICIENT)
                    *imageCountOutput = mt_params.image_count;
                LeaveCriticalSection(&wine_session->swapchain_lock);
                return mt_params.result;
            }

            d3d11_images = calloc(mt_params.image_count, sizeof(*d3d11_images));
            if (!d3d11_images)
            {
                free(mtl_textures);
                free(mach_port_names);
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
                /* Must match the MTLTexture's mipmapLevelCount: the desc is
                 * what DXMT builds its D3D11-side view descriptors from, so a
                 * mismatch would make it address levels the image does not have */
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

                hr = import_shared_mtl_texture(wine_session, &desc,
                                               mach_port_names[i], &texture);
                if (FAILED(hr))
                {
                    WINE_ERR("importing swapchain image %u failed: 0x%08x\n",
                             i, (unsigned)hr);
                    release_imported_d3d11_textures(d3d11_images, i);
                    free(d3d11_images);
                    free(mtl_textures);
                    free(mach_port_names);
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
            free(mach_port_names);

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

    /* Take and drop the sync carrier's keyed mutex. DXMT encodes a wait for the
     * value we last signalled and then a signal of value+1, both on its own
     * queue behind everything the app has encoded - the stock-DXMT equivalent
     * of ID3D11DeviceContext4::Signal on a shared fence. Values are a plain
     * count because nothing else ever touches this mutex */
    if (wine_session->sync_mutex) {
        HRESULT hr;

        EnterCriticalSection(&wine_session->swapchain_lock);
        hr = IDXGIKeyedMutex_AcquireSync(wine_session->sync_mutex, 0, 1000);
        if (SUCCEEDED(hr)) {
            hr = IDXGIKeyedMutex_ReleaseSync(wine_session->sync_mutex, 0);
            if (SUCCEEDED(hr))
                params.gpu_fence_value = (uint64_t)++wine_session->gpu_fence_value;
        }
        LeaveCriticalSection(&wine_session->swapchain_lock);

        if (FAILED(hr))
            WINE_WARN("sync carrier signal failed: 0x%08x\n", (unsigned)hr);
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

static void rewrite_subimage_swapchain(XrSwapchainSubImage *sub_image)
{
    struct wine_XrSwapchain *wine_swapchain;

    wine_swapchain = wine_swapchain_from_handle(sub_image->swapchain);
    sub_image->swapchain = wine_swapchain ? wine_swapchain->host_swapchain
                                          : sub_image->swapchain;
}

#define HANDLE_SUBIMAGE_LAYER(xr_type, field_name, src_type)                    \
    case xr_type:                                                                \
    {                                                                             \
        const src_type *src = (const src_type *)layer;                           \
        CompositionLayer *dst = &layer_storage[i];                               \
        dst->field_name = *src;                                                  \
        rewrite_subimage_swapchain(&dst->field_name.subImage);                   \
        host_layers[i] = &dst->base;                                             \
        break;                                                                    \
    }

XrResult WINAPI xrEndFrame(XrSession session,
                           const XrFrameEndInfo *frameEndInfo)
{
    struct xrEndFrame_params params;
    XrFrameEndInfo our_end_info;
    CompositionLayer *layer_storage = NULL;
    const XrCompositionLayerBaseHeader **host_layers = NULL;
    XrCompositionLayerProjectionView *host_projection_views = NULL;
    XrCompositionLayerDepthInfoKHR *host_depth_infos = NULL;
    uint32_t layer_count;
    size_t total_views = 0;
    size_t view_offset = 0;
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
                rewrite_subimage_swapchain(&dst_view->subImage);

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
                    rewrite_subimage_swapchain(&di->subImage);
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
