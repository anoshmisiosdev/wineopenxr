/* SPDX-License-Identifier: LGPL-2.1-or-later */

#import <Metal/Metal.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "wine/debug_slim.h"

#include "openxr/openxr.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

#define STATUS_SUCCESS ((NTSTATUS)0)

/* Stubs for types the shared bridge.h forward-declares but this TU
 * never dereferences. They keep header chains from pulling in D3D11 or Win32
 * content that clashes with Metal.h */
typedef struct { long long QuadPart; } LARGE_INTEGER;
#define XR_USE_GRAPHICS_API_D3D11 1
#define XR_USE_GRAPHICS_API_D3D12 1
#define XR_USE_GRAPHICS_API_METAL 1
#define XR_USE_PLATFORM_WIN32 1
#define XR_USE_TIMESPEC 1
typedef int D3D_FEATURE_LEVEL;
typedef struct { unsigned int LowPart; int HighPart; } LUID;
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Texture2D ID3D11Texture2D;
typedef struct ID3D11DeviceContext4 ID3D11DeviceContext4;
typedef struct ID3D11Fence ID3D11Fence;
typedef struct IUnknown IUnknown;

#include "bridge.h"
#include "unixcall.h"
#include "dispatch.h"

extern struct openxr_instance_funcs g_xr_host_instance_dispatch_table;

/* Private Metal SPI, same pair DXMT's winemetal uses to move an IOSurface
 * (texture) or a shared event between processes/graphics stacks as a mach
 * send right. The bridge stays on the same mechanism so a STOCK DXMT can
 * import what the OpenXR runtime allocated */
@interface MTLSharedTextureHandle (OXRSysBridge)
- (mach_port_t)createMachPort;
@end

@protocol OXRSysMTLDeviceSPI <MTLDevice>
- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;
@end

/* bootstrap_register() is deprecated-with-no-replacement in the SDK headers but
 * is the only way to publish a send right under a name; DXMT relies on the same
 * call from its unix half, in this same task */
extern kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name,
                                         mach_port_t sp, int flags);

/* Publish `port` under a fresh launchd service name. Writes a NUL-terminated
 * name of at most OXR_MACH_NAME_LEN-1 chars into out_name. Returns 0 on
 * failure. The registration (like DXMT's) lives for the life of the process */
static int bridge_register_mach_port(mach_port_t port,
                                     char out_name[OXR_MACH_NAME_LEN])
{
    static uint64_t counter;
    mach_port_t bootstrap = MACH_PORT_NULL;
    kern_return_t kr;

    if (!port)
        return 0;

    if (task_get_bootstrap_port(mach_task_self(), &bootstrap) != KERN_SUCCESS)
        return 0;

    /* 20 + 8 + 1 + 16 + 1 = 46 bytes, inside the 54-byte field */
    snprintf(out_name, OXR_MACH_NAME_LEN, "OXRSys_bridge_shared_%08x_%016llx",
             (unsigned)getpid(), (unsigned long long)(++counter) ^ ((unsigned long long)arc4random() << 32));

    kr = bootstrap_register2(bootstrap, out_name, port, 0);
    mach_port_deallocate(mach_task_self(), bootstrap);

    if (kr != KERN_SUCCESS)
    {
        WINE_ERR("bootstrap_register2(%s) failed: 0x%x\n", out_name, (unsigned)kr);
        out_name[0] = 0;
        return 0;
    }
    return 1;
}

static mach_port_t bridge_look_up_mach_port(const char *name)
{
    mach_port_t bootstrap = MACH_PORT_NULL, port = MACH_PORT_NULL;
    kern_return_t kr;

    if (!name || !name[0])
        return MACH_PORT_NULL;

    if (task_get_bootstrap_port(mach_task_self(), &bootstrap) != KERN_SUCCESS)
        return MACH_PORT_NULL;

    kr = bootstrap_look_up(bootstrap, (char *)name, &port);
    mach_port_deallocate(mach_task_self(), bootstrap);

    if (kr != KERN_SUCCESS)
    {
        WINE_ERR("bootstrap_look_up(%s) failed: 0x%x\n", name, (unsigned)kr);
        return MACH_PORT_NULL;
    }
    return port;
}

NTSTATUS wine_create_d3d11_session(void *args)
{
    @autoreleasepool {
        struct create_d3d11_session_params *params = args;
        struct wine_XrInstance *wine_instance = wine_instance_from_handle(params->instance);
        XrInstance host_instance = wine_instance->host_instance;
        struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
        XrGraphicsRequirementsMetalKHR metal_reqs = {
            .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR,
        };
        XrResult res = XR_SUCCESS;
        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;

        if (!funcs->p_xrGetMetalGraphicsRequirementsKHR)
        {
            WINE_ERR("XR_KHR_metal_enable not supported by runtime\n");
            res = XR_ERROR_FUNCTION_UNSUPPORTED;
            goto out;
        }

        res = funcs->p_xrGetMetalGraphicsRequirementsKHR(
            host_instance, params->system_id, &metal_reqs);
        if (res != XR_SUCCESS)
        {
            WINE_ERR("xrGetMetalGraphicsRequirementsKHR failed: %d\n", res);
            goto out;
        }

        device = (id<MTLDevice>)metal_reqs.metalDevice;
        if (!device)
        {
            WINE_ERR("metalDevice is NULL\n");
            res = XR_ERROR_RUNTIME_FAILURE;
            goto out;
        }
        [device retain];

        queue = [device newCommandQueue];
        if (!queue)
        {
            WINE_ERR("newCommandQueue failed\n");
            res = XR_ERROR_RUNTIME_FAILURE;
            goto out;
        }

        {
            XrGraphicsBindingMetalKHR metal_binding = {
                .type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR,
                .next = NULL,
                .commandQueue = (void *)queue,
            };
            XrSessionCreateInfo session_info = {
                .type = XR_TYPE_SESSION_CREATE_INFO,
                .next = &metal_binding,
                .systemId = params->system_id,
            };

            res = funcs->p_xrCreateSession(
                host_instance, &session_info, params->session);
        }

        if (res != XR_SUCCESS)
        {
            WINE_ERR("xrCreateSession (Metal) failed: %d\n", res);
            goto out;
        }

        /* Open our own id<MTLSharedEvent> on the same underlying event the
         * app-side ID3D11Fence signals, so the release path can encode a GPU
         * wait on the runtime's queue. Stock DXMT publishes the event's mach
         * port under this name from ID3D11Fence::CreateSharedHandle */
        if (params->fence_mach_port_name[0])
        {
            mach_port_t port = bridge_look_up_mach_port(params->fence_mach_port_name);
            if (port)
            {
                id<MTLSharedEvent> event =
                    [(id<OXRSysMTLDeviceSPI>)device newSharedEventWithMachPort:port];
                if (event)
                    params->mtl_shared_event = (uint64_t)(uintptr_t)event;
                else
                    WINE_ERR("newSharedEventWithMachPort failed for %s\n",
                             params->fence_mach_port_name);
            }
        }

        params->mtl_device = (void *)device;
        params->mtl_command_queue = (void *)queue;
        device = nil;
        queue = nil;

out:
        [queue release];
        [device release];
        params->result = res;
        return STATUS_SUCCESS;
    }
}

NTSTATUS wine_release_metal_session(void *args)
{
    @autoreleasepool {
        struct release_metal_session_params *params = args;

        [(id<MTLSharedEvent>)(void *)(uintptr_t)params->mtl_shared_event release];
        [(id<MTLCommandQueue>)params->mtl_command_queue release];
        [(id<MTLDevice>)params->mtl_device release];

        return STATUS_SUCCESS;
    }
}

NTSTATUS wine_xrGetD3D11GraphicsRequirementsKHR(void *args)
{
    @autoreleasepool {
        struct xrGetD3D11GraphicsRequirementsKHR_params *params = args;
        struct wine_XrInstance *wine_instance = wine_instance_from_handle(params->instance);
        struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
        XrGraphicsRequirementsMetalKHR metal_reqs = {
            .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR,
        };

        if (!funcs->p_xrGetMetalGraphicsRequirementsKHR)
        {
            params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
            return STATUS_SUCCESS;
        }

        params->result = funcs->p_xrGetMetalGraphicsRequirementsKHR(
            wine_instance->host_instance, params->systemId, &metal_reqs);

        if (params->result == XR_SUCCESS)
        {
            params->graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
            params->graphicsRequirements->minFeatureLevel = 0xb000; /* D3D_FEATURE_LEVEL_11_0 */

            if (metal_reqs.metalDevice)
            {
                /* DXMT bit-casts bswap64(MTLDevice.registryID) into LUID */
                uint64_t reg_id = [(id<MTLDevice>)metal_reqs.metalDevice registryID];
                uint64_t swapped = __builtin_bswap64(reg_id);
                memcpy(&params->graphicsRequirements->adapterLuid, &swapped, sizeof(swapped));
            }
            else
            {
                WINE_ERR("metalDevice is NULL from requirements\n");
                params->result = XR_ERROR_RUNTIME_FAILURE;
            }
        }

        return STATUS_SUCCESS;
    }
}

/* Same as the D3D11 query; the PE side swaps the LUID for the DXGI adapter's
 * (D3DMetal reports its own) */
NTSTATUS wine_xrGetD3D12GraphicsRequirementsKHR(void *args)
{
    @autoreleasepool {
        struct xrGetD3D12GraphicsRequirementsKHR_params *params = args;
        struct wine_XrInstance *wine_instance = wine_instance_from_handle(params->instance);
        struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
        XrGraphicsRequirementsMetalKHR metal_reqs = {
            .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR,
        };

        if (!funcs->p_xrGetMetalGraphicsRequirementsKHR)
        {
            params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
            return STATUS_SUCCESS;
        }

        params->result = funcs->p_xrGetMetalGraphicsRequirementsKHR(
            wine_instance->host_instance, params->systemId, &metal_reqs);

        if (params->result == XR_SUCCESS)
        {
            params->graphicsRequirements->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
            params->graphicsRequirements->minFeatureLevel = 0xb000; /* D3D_FEATURE_LEVEL_11_0 */

            if (metal_reqs.metalDevice)
            {
                uint64_t reg_id = [(id<MTLDevice>)metal_reqs.metalDevice registryID];
                uint64_t swapped = __builtin_bswap64(reg_id);
                memcpy(&params->graphicsRequirements->adapterLuid, &swapped, sizeof(swapped));
            }
            else
            {
                WINE_ERR("metalDevice is NULL from requirements\n");
                params->result = XR_ERROR_RUNTIME_FAILURE;
            }
        }

        return STATUS_SUCCESS;
    }
}

NTSTATUS wine_export_metal_textures(void *args)
{
    @autoreleasepool {
        struct export_metal_textures_params *params = args;
        struct wine_XrSwapchain *wine_swapchain = wine_swapchain_from_handle(params->swapchain);
        struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
        XrSwapchain host_swapchain = wine_swapchain->host_swapchain;
        XrSwapchainImageMetalKHR *metal_images = NULL;
        uint32_t count = 0, i;
        XrResult res;

        if (!funcs->p_xrEnumerateSwapchainImages)
        {
            params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
            return STATUS_SUCCESS;
        }

        res = funcs->p_xrEnumerateSwapchainImages(host_swapchain, 0, &count, NULL);
        if (res != XR_SUCCESS)
        {
            params->result = res;
            return STATUS_SUCCESS;
        }

        if (count > params->image_count)
        {
            params->image_count = count;
            params->result = XR_ERROR_SIZE_INSUFFICIENT;
            return STATUS_SUCCESS;
        }

        metal_images = calloc(count, sizeof(*metal_images));
        if (!metal_images)
        {
            params->result = XR_ERROR_OUT_OF_MEMORY;
            return STATUS_SUCCESS;
        }

        for (i = 0; i < count; i++)
        {
            metal_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
            metal_images[i].next = NULL;
        }

        res = funcs->p_xrEnumerateSwapchainImages(
            host_swapchain, count, &count, (XrSwapchainImageBaseHeader *)metal_images);
        if (res != XR_SUCCESS)
        {
            free(metal_images);
            params->result = res;
            return STATUS_SUCCESS;
        }

        for (i = 0; i < count; i++)
            params->mtl_textures[i] = (uint64_t)(uintptr_t)metal_images[i].texture;

        /* Publish each image's IOSurface as a mach send right under a unique
         * launchd name. The PE side wraps the name in a D3DKMT shared-resource
         * record, and stock DXMT's ID3D11Device::OpenSharedResource looks it
         * back up and rebuilds the MTLTexture - no DXMT patch needed.
         * Requires the runtime to have allocated the images with
         * newSharedTextureWithDescriptor: (OXRSys does) */
        if (params->mach_port_names)
        {
            for (i = 0; i < count; i++)
            {
                id<MTLTexture> texture = (id<MTLTexture>)metal_images[i].texture;
                MTLSharedTextureHandle *handle = [texture newSharedTextureHandle];
                mach_port_t port = MACH_PORT_NULL;

                params->mach_port_names[i][0] = 0;

                if (!handle)
                {
                    WINE_ERR("swapchain image %u is not a Metal shared texture; "
                             "the OpenXR runtime must allocate swapchain images "
                             "with newSharedTextureWithDescriptor:\n", i);
                    free(metal_images);
                    params->result = XR_ERROR_RUNTIME_FAILURE;
                    return STATUS_SUCCESS;
                }

                port = [handle createMachPort];
                [handle release];

                if (!bridge_register_mach_port(port, params->mach_port_names[i]))
                {
                    free(metal_images);
                    params->result = XR_ERROR_RUNTIME_FAILURE;
                    return STATUS_SUCCESS;
                }
            }
        }

        free(metal_images);

        params->image_count = count;
        params->width = wine_swapchain->create_info.width;
        params->height = wine_swapchain->create_info.height;
        params->array_size = wine_swapchain->create_info.arraySize;
        params->dxgi_format = wine_swapchain->create_info.format;

        params->result = XR_SUCCESS;
        return STATUS_SUCCESS;
    }
}
