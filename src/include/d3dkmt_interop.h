/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* Minimal D3DKMT surface needed to hand an externally created Metal texture to
 * a STOCK (unpatched) DXMT.
 *
 * DXMT shares a texture between D3D11 devices by registering the backing
 * MTLTexture's IOSurface mach port with launchd under a random service name,
 * then stashing that name (plus the D3D11 desc) in a D3DKMT resource's
 * *private runtime data*. ID3D11Device::OpenSharedResource reads the record
 * back, looks the name up and re-creates the MTLTexture from the port. We
 * create the same kind of record for a texture the OpenXR runtime allocated,
 * so the stock OpenSharedResource path imports it with no DXMT changes.
 *
 * The struct declarations mirror Wine's (via DXMT's src/util/util_d3dkmt.h) so
 * the binary layouts match what gdi32 expects. Only what the bridge calls is
 * declared. Functions are resolved from gdi32.dll at runtime rather than
 * imported, so a Wine build without them degrades to a clear error instead of
 * failing to load the DLL.
 *
 * The same record carries the D3DKMT handle of an optional keyed mutex, and a
 * keyed mutex's own private data is just the mach service name of the
 * MTLSharedEvent behind it. That is how the bridge gets a Metal event stock
 * DXMT will signal on its queue: it creates a 1x1 keyed-mutex texture and uses
 * IDXGIKeyedMutex::ReleaseSync as the GPU signal. (ID3D11Fence sharing would be
 * the obvious route, but D3DKMTQueryResourceInfoFromNtHandle rejects sync-object
 * handles on CrossOver's Wine, which breaks DXMT's own OpenSharedFence too.)
 *
 * dxmt_shared_resource_data is DXMT-private and NOT a stable ABI. The bridge
 * never trusts it blindly: session_create_sync_carrier() has stock DXMT write a
 * record for that 1x1 texture and checks every field reads back where we
 * expect, so a layout change fails loudly at session creation. */

#ifndef __WINE_OPENXR_D3DKMT_INTEROP_H
#define __WINE_OPENXR_D3DKMT_INTEROP_H

#include <windows.h>
#include <d3d11_4.h>

typedef UINT D3DKMT_HANDLE;
typedef UINT64 D3DGPU_VIRTUAL_ADDRESS;
typedef UINT D3DDDI_VIDEO_PRESENT_SOURCE_ID;

typedef struct _D3DKMT_CREATEDEVICEFLAGS
{
    UINT LegacyMode : 1;
    UINT RequestVSync : 1;
    UINT DisableGpuTimeout : 1;
    UINT Reserved : 29;
} D3DKMT_CREATEDEVICEFLAGS;

typedef struct _D3DDDI_ALLOCATIONLIST
{
    D3DKMT_HANDLE hAllocation;
    union {
        struct { UINT WriteOperation : 1; UINT DoNotRetireInstance : 1;
                 UINT OfferPriority : 3; UINT Reserved : 27; } s;
        UINT Value;
    } u;
} D3DDDI_ALLOCATIONLIST;

typedef struct _D3DDDI_PATCHLOCATIONLIST
{
    UINT AllocationIndex;
    union {
        struct { UINT SlotId : 24; UINT Reserved : 8; } s;
        UINT Value;
    } u;
    UINT DriverId;
    UINT AllocationOffset;
    UINT PatchOffset;
    UINT SplitOffset;
} D3DDDI_PATCHLOCATIONLIST;

typedef struct _D3DKMT_CREATEDEVICE
{
    union {
        D3DKMT_HANDLE hAdapter;
        void *pAdapter;
    } u;
    D3DKMT_CREATEDEVICEFLAGS Flags;
    D3DKMT_HANDLE hDevice;
    void *pCommandBuffer;
    UINT CommandBufferSize;
    D3DDDI_ALLOCATIONLIST *pAllocationList;
    UINT AllocationListSize;
    D3DDDI_PATCHLOCATIONLIST *pPatchLocationList;
    UINT PatchLocationListSize;
} D3DKMT_CREATEDEVICE;

typedef struct _D3DKMT_DESTROYDEVICE
{
    D3DKMT_HANDLE hDevice;
} D3DKMT_DESTROYDEVICE;

typedef struct _D3DKMT_OPENADAPTERFROMLUID
{
    LUID AdapterLuid;
    D3DKMT_HANDLE hAdapter;
} D3DKMT_OPENADAPTERFROMLUID;

typedef struct _D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME
{
    WCHAR DeviceName[32];
    D3DKMT_HANDLE hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME;

typedef struct _D3DKMT_CLOSEADAPTER
{
    D3DKMT_HANDLE hAdapter;
} D3DKMT_CLOSEADAPTER;

typedef struct _D3DKMT_DESTROYALLOCATION
{
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hResource;
    const D3DKMT_HANDLE *phAllocationList;
    UINT AllocationCount;
} D3DKMT_DESTROYALLOCATION;

typedef struct _D3DKMT_QUERYRESOURCEINFO
{
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hGlobalShare;
    void *pPrivateRuntimeData;
    UINT PrivateRuntimeDataSize;
    UINT TotalPrivateDriverDataSize;
    UINT ResourcePrivateDriverDataSize;
    UINT NumAllocations;
} D3DKMT_QUERYRESOURCEINFO;

typedef struct _D3DDDI_OPENALLOCATIONINFO2
{
    D3DKMT_HANDLE hAllocation;
    const void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    D3DGPU_VIRTUAL_ADDRESS GpuVirtualAddress;
    ULONG_PTR Reserved[6];
} D3DDDI_OPENALLOCATIONINFO2;

typedef struct _D3DKMT_OPENRESOURCE
{
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hGlobalShare;
    UINT NumAllocations;
    union {
        void *pOpenAllocationInfo;
        D3DDDI_OPENALLOCATIONINFO2 *pOpenAllocationInfo2;
    } u;
    void *pPrivateRuntimeData;
    UINT PrivateRuntimeDataSize;
    void *pResourcePrivateDriverData;
    UINT ResourcePrivateDriverDataSize;
    void *pTotalPrivateDriverDataBuffer;
    UINT TotalPrivateDriverDataBufferSize;
    D3DKMT_HANDLE hResource;
} D3DKMT_OPENRESOURCE;

typedef struct _D3DKMT_OPENKEYEDMUTEX2
{
    D3DKMT_HANDLE hSharedHandle;
    D3DKMT_HANDLE hKeyedMutex;
    void *pPrivateRuntimeData;
    UINT PrivateRuntimeDataSize;
} D3DKMT_OPENKEYEDMUTEX2;

typedef struct _D3DKMT_CREATESTANDARDALLOCATIONFLAGS
{
    union { struct { UINT Reserved : 32; } s; UINT Value; } u;
} D3DKMT_CREATESTANDARDALLOCATIONFLAGS;

typedef enum _D3DKMT_STANDARDALLOCATIONTYPE
{
    D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP = 1,
} D3DKMT_STANDARDALLOCATIONTYPE;

typedef struct _D3DKMT_STANDARDALLOCATION_EXISTINGHEAP
{
    SIZE_T Size;
} D3DKMT_STANDARDALLOCATION_EXISTINGHEAP;

typedef struct _D3DKMT_CREATESTANDARDALLOCATION
{
    D3DKMT_STANDARDALLOCATIONTYPE Type;
    union { D3DKMT_STANDARDALLOCATION_EXISTINGHEAP ExistingHeapData; } u;
    D3DKMT_CREATESTANDARDALLOCATIONFLAGS Flags;
} D3DKMT_CREATESTANDARDALLOCATION;

typedef struct _D3DDDI_ALLOCATIONINFO
{
    D3DKMT_HANDLE hAllocation;
    const void *pSystemMem;
    void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
    union {
        struct { UINT Primary : 1; UINT Stereo : 1; UINT Reserved : 30; } s;
        UINT Value;
    } Flags;
} D3DDDI_ALLOCATIONINFO;

typedef struct _D3DDDI_ALLOCATIONINFO2
{
    D3DKMT_HANDLE hAllocation;
    union { HANDLE hSection; const void *pSystemMem; } u;
    void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
    union {
        struct { UINT Primary : 1; UINT Stereo : 1; UINT OverridePriority : 1;
                 UINT Reserved : 29; } s;
        UINT Value;
    } Flags;
    D3DGPU_VIRTUAL_ADDRESS GpuVirtualAddress;
    union { UINT Priority; ULONG_PTR Unused; } u2;
    ULONG_PTR Reserved[5];
} D3DDDI_ALLOCATIONINFO2;

typedef struct _D3DKMT_CREATEALLOCATIONFLAGS
{
    UINT CreateResource : 1;
    UINT CreateShared : 1;
    UINT NonSecure : 1;
    UINT CreateProtected : 1;
    UINT RestrictSharedAccess : 1;
    UINT ExistingSysMem : 1;
    UINT NtSecuritySharing : 1;
    UINT ReadOnly : 1;
    UINT CreateWriteCombined : 1;
    UINT CreateCached : 1;
    UINT SwapChainBackBuffer : 1;
    UINT CrossAdapter : 1;
    UINT OpenCrossAdapter : 1;
    UINT PartialSharedCreation : 1;
    UINT Zeroed : 1;
    UINT WriteWatch : 1;
    UINT StandardAllocation : 1;
    UINT ExistingSection : 1;
    UINT AllowNotZeroed : 1;
    UINT PhysicallyContiguous : 1;
    UINT Reserved : 12;
} D3DKMT_CREATEALLOCATIONFLAGS;

typedef struct _D3DKMT_CREATEALLOCATION
{
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hResource;
    D3DKMT_HANDLE hGlobalShare;
    const void *pPrivateRuntimeData;
    UINT PrivateRuntimeDataSize;
    union {
        D3DKMT_CREATESTANDARDALLOCATION *pStandardAllocation;
        const void *pPrivateDriverData;
    } u;
    UINT PrivateDriverDataSize;
    UINT NumAllocations;
    union {
        D3DDDI_ALLOCATIONINFO *pAllocationInfo;
        D3DDDI_ALLOCATIONINFO2 *pAllocationInfo2;
    } u2;
    D3DKMT_CREATEALLOCATIONFLAGS Flags;
    HANDLE hPrivateRuntimeResourceHandle;
} D3DKMT_CREATEALLOCATION;

/* DXMT's private per-resource runtime record. Mirrors
 * dxmt::SharedResourceData in src/d3d11/d3d11_texture_device.cpp.
 * Validated at runtime by d3dkmt_probe_shared_layout() */
#define DXMT_SHARED_NAME_LEN 54

struct dxmt_shared_resource_data
{
    char mach_port_name[DXMT_SHARED_NAME_LEN];
    D3D11_RESOURCE_DIMENSION dimension;
    union {
        D3D11_TEXTURE1D_DESC desc1d;
        D3D11_TEXTURE2D_DESC1 desc2d;
        D3D11_TEXTURE3D_DESC1 desc3d;
    } desc;
    D3DKMT_HANDLE mutex_handle;
};

struct d3dkmt_funcs
{
    HMODULE gdi32;
    NTSTATUS (WINAPI *OpenAdapterFromLuid)(D3DKMT_OPENADAPTERFROMLUID *);
    NTSTATUS (WINAPI *OpenAdapterFromGdiDisplayName)(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *);
    NTSTATUS (WINAPI *CloseAdapter)(const D3DKMT_CLOSEADAPTER *);
    NTSTATUS (WINAPI *CreateDevice)(D3DKMT_CREATEDEVICE *);
    NTSTATUS (WINAPI *DestroyDevice)(const D3DKMT_DESTROYDEVICE *);
    NTSTATUS (WINAPI *CreateAllocation2)(D3DKMT_CREATEALLOCATION *);
    NTSTATUS (WINAPI *DestroyAllocation)(const D3DKMT_DESTROYALLOCATION *);
    NTSTATUS (WINAPI *QueryResourceInfo)(D3DKMT_QUERYRESOURCEINFO *);
    NTSTATUS (WINAPI *OpenResource2)(D3DKMT_OPENRESOURCE *);
    NTSTATUS (WINAPI *OpenKeyedMutex2)(D3DKMT_OPENKEYEDMUTEX2 *);
};

#endif /* __WINE_OPENXR_D3DKMT_INTEROP_H */
