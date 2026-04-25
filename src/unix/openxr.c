/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XR_USE_GRAPHICS_API_METAL
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wine/debug.h"

#include "openxr_loader.h"
#include "loader_thunks.h"
#include "openxr_thunks.h"
#include "format_table.h"
#include "extension_table.h"
#include "extension_filter_table.h"

WINE_DEFAULT_DEBUG_CHANNEL(openxr);

/* OpenXR 1.1 mandates per-instance dispatch. Target apps create one
 * XrInstance per process, and a second concurrent wine_xrCreateInstance
 * returns XR_ERROR_LIMIT_REACHED */
struct openxr_instance_funcs g_xr_host_instance_dispatch_table;
static pthread_mutex_t g_instance_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_instance_live;

NTSTATUS wine_init(void *args)
{
    struct init_params *params = args;
    params->result = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

/* Map a bridge-synthesised function name to the host function the bridge
 * ultimately wraps. Returns NULL if the name is not a synthesised function.
 * The mapping lives in substitute_extensions so synthetic-function policy
 * and extension-substitution policy share one table */
static const char *synthetic_host_equivalent(const char *name)
{
    uint32_t i;
    for (i = 0; i < ARRAY_SIZE(substitute_extensions); i++)
    {
        const struct extension_substitution *sub = &substitute_extensions[i];
        const struct synthetic_function_mapping *mapping;

        if (!sub->synthetic_mappings)
            continue;
        for (mapping = sub->synthetic_mappings; mapping->synthetic_name; mapping++)
        {
            if (!strcmp(name, mapping->synthetic_name))
                return mapping->host_gate_name;
        }
    }
    return NULL;
}

NTSTATUS wine_is_available_instance_function(void *args)
{
    struct is_available_instance_function_params *params = args;
    struct wine_XrInstance *wine_instance;
    const char *host_name;
    XrInstance host_instance;
    PFN_xrVoidFunction pfn;
    XrResult res;

    /* PE-side xrGetInstanceProcAddr handles NULL-instance entry points before
     * calling us. A NULL instance here cannot be answered against an app-enabled
     * extension set, so refuse the probe instead of exposing extension-gated names */
    if (!params->instance)
    {
        params->available = FALSE;
        return STATUS_SUCCESS;
    }

    wine_instance = wine_instance_from_handle(params->instance);
    host_instance = wine_instance->host_instance;

    /* Bridge-synthesised functions mirror the host's gate on the equivalent
     * host function. The host's extension-enabled state is the app's view, so
     * a synthetic is available iff the app enabled the aliased Win32 extension */
    host_name = synthetic_host_equivalent(params->name);
    if (host_name)
    {
        res = xrGetInstanceProcAddr(host_instance, host_name, &pfn);
        params->available = (res == XR_SUCCESS && pfn != NULL);
        return STATUS_SUCCESS;
    }

    res = xrGetInstanceProcAddr(host_instance, params->name, &pfn);
    params->available = (res == XR_SUCCESS && pfn != NULL);
    return STATUS_SUCCESS;
}

/* blocked_extensions holds host extensions the bridge cannot expose safely.
 * Hide them from enumeration and reject them in xrCreateInstance */
static BOOL extension_is_blocked(const char *name)
{
    uint32_t i;
    for (i = 0; i < ARRAY_SIZE(blocked_extensions); i++)
        if (!strcmp(name, blocked_extensions[i]))
            return TRUE;
    return FALSE;
}

/* A host native_ext replaced by a Win32 alias is hidden from enumeration and
 * must be rejected in xrCreateInstance if the app asks for it directly. Per
 * OpenXR 1.1 xrCreateInstance, any name not returned by
 * xrEnumerateInstanceExtensionProperties MUST return
 * XR_ERROR_EXTENSION_NOT_PRESENT. Enumeration does NOT use this helper,
 * because apply_substitutions rewrites native_ext to win32_ext there */
static BOOL extension_is_hidden_native(const char *name)
{
    uint32_t i;
    for (i = 0; i < ARRAY_SIZE(substitute_extensions); i++)
    {
        const struct extension_substitution *sub = &substitute_extensions[i];
        if (sub->native_ext && !strcmp(name, sub->native_ext))
            return TRUE;
    }
    return FALSE;
}

static BOOL extension_is_rejected(const char *name)
{
    return extension_is_blocked(name) || extension_is_hidden_native(name);
}

static const char *translate_app_extension_name(const char *name)
{
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(substitute_extensions); i++)
    {
        const struct extension_substitution *sub = &substitute_extensions[i];
        if (sub->win32_ext && !strcmp(name, sub->win32_ext))
        {
            TRACE("Extension %s -> %s\n", name, sub->native_ext);
            return sub->native_ext;
        }
    }

    return name;
}

static void emit_extension(XrExtensionProperties *out,
                           uint32_t out_cap,
                           uint32_t *output_count,
                           const XrExtensionProperties *native,
                           const char *replacement_name,
                           uint32_t replacement_version)
{
    if (out && *output_count < out_cap)
    {
        out[*output_count] = *native;
        if (replacement_name)
        {
            strcpy(out[*output_count].extensionName, replacement_name);
            out[*output_count].extensionVersion = replacement_version;
        }
    }
    (*output_count)++;
}

/* Walks native_list once. If out is non-NULL, writes up to out_cap entries.
 * Always returns the full output count so the count and fill paths of
 * xrEnumerateInstanceExtensionProperties share one implementation and
 * cannot drift. Native aliases are replaced with Win32 names */
static uint32_t apply_substitutions(const XrExtensionProperties *native_list,
                                    uint32_t native_count,
                                    XrExtensionProperties *out,
                                    uint32_t out_cap)
{
    uint32_t i, j, output_count = 0;

    for (i = 0; i < native_count; i++)
    {
        const XrExtensionProperties *native = &native_list[i];
        BOOL handled = FALSE;

        if (extension_is_blocked(native->extensionName))
            continue;

        for (j = 0; j < ARRAY_SIZE(substitute_extensions); j++)
        {
            const struct extension_substitution *sub = &substitute_extensions[j];
            if (!sub->native_ext || strcmp(native->extensionName, sub->native_ext))
                continue;

            if (sub->win32_ext)
                emit_extension(out, out_cap, &output_count,
                               native, sub->win32_ext, sub->version);
            handled = TRUE;
        }

        if (!handled)
            emit_extension(out, out_cap, &output_count, native, NULL, 0);
    }

    return output_count;
}

NTSTATUS wine_xrEnumerateInstanceExtensionProperties(void *args)
{
    struct xrEnumerateInstanceExtensionProperties_params *params = args;
    XrExtensionProperties *native = NULL;
    uint32_t native_count = 0, total;
    XrResult res;

    res = xrEnumerateInstanceExtensionProperties(params->layerName, 0,
                                                 &native_count, NULL);
    if (res != XR_SUCCESS)
    {
        params->result = res;
        return STATUS_SUCCESS;
    }

    if (native_count)
    {
        uint32_t i;
        native = calloc(native_count, sizeof(*native));
        if (!native)
        {
            params->result = XR_ERROR_OUT_OF_MEMORY;
            return STATUS_SUCCESS;
        }
        for (i = 0; i < native_count; i++)
            native[i].type = XR_TYPE_EXTENSION_PROPERTIES;

        res = xrEnumerateInstanceExtensionProperties(params->layerName,
                                                     native_count, &native_count, native);
        if (res != XR_SUCCESS)
        {
            free(native);
            params->result = res;
            return STATUS_SUCCESS;
        }
    }

    total = apply_substitutions(native, native_count,
                                params->properties,
                                params->properties ? params->propertyCapacityInput : 0);
    free(native);

    *params->propertyCountOutput = total;
    TRACE("%u extensions (after substitution and filter)\n", total);

    if (params->properties && total > params->propertyCapacityInput)
    {
        params->result = XR_ERROR_SIZE_INSUFFICIENT;
        return STATUS_SUCCESS;
    }

    params->result = XR_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS wine_xrCreateInstance(void *args)
{
    struct xrCreateInstance_params *params = args;
    XrInstanceCreateInfo our_info;
    const char **new_list = NULL;
    uint32_t extension_index, translated_count = 0;
    XrResult res = XR_SUCCESS;

    TRACE("createInfo=%p, instance=%p\n", params->createInfo, params->instance);

    /* Hold g_instance_lock across the full check-create-publish so a second
     * concurrent caller serializes behind us. On unlock it either sees
     * g_instance_live=1 and returns XR_ERROR_LIMIT_REACHED, or retries when
     * this call failed */
    pthread_mutex_lock(&g_instance_lock);
    if (g_instance_live)
    {
        WARN("xrCreateInstance rejected: one instance already live\n");
        res = XR_ERROR_LIMIT_REACHED;
        goto out;
    }

    /* Reject any app-requested extension filtered out of enumeration. Per
     * spec an extension not visible through xrEnumerateInstanceExtensionProperties
     * is not present. This covers explicitly blocked names and host native
     * names hidden behind a Win32 alias */
    for (extension_index = 0;
         extension_index < params->createInfo->enabledExtensionCount;
         extension_index++)
    {
        const char *extension_name = params->createInfo->enabledExtensionNames[extension_index];
        if (extension_is_rejected(extension_name))
        {
            WARN("Rejecting extension not enumerated: %s\n", extension_name);
            res = XR_ERROR_EXTENSION_NOT_PRESENT;
            goto out;
        }
    }

    if (params->createInfo->enabledExtensionCount)
    {
        new_list = malloc(sizeof(*new_list) * params->createInfo->enabledExtensionCount);
        if (!new_list)
        {
            res = XR_ERROR_OUT_OF_MEMORY;
            goto out;
        }
    }

    for (extension_index = 0;
         extension_index < params->createInfo->enabledExtensionCount;
         extension_index++)
    {
        const char *extension_name = params->createInfo->enabledExtensionNames[extension_index];
        new_list[translated_count++] = translate_app_extension_name(extension_name);
    }

    our_info = *params->createInfo;
    our_info.enabledExtensionNames = (const char *const *)new_list;
    our_info.enabledExtensionCount = translated_count;
    /* Layers live on the PE side. The Unix-side statically linked Khronos
     * loader has its own registry search that cannot see PE-only layer DLLs,
     * so forwarding enabledApiLayerNames here would yield
     * XR_ERROR_API_LAYER_NOT_PRESENT for CTS and other layer-using apps */
    our_info.enabledApiLayerNames = NULL;
    our_info.enabledApiLayerCount = 0;

    TRACE("Creating instance with %u extensions\n", translated_count);
    res = xrCreateInstance(&our_info, params->instance);
    if (res != XR_SUCCESS)
    {
        WARN("xrCreateInstance failed: %d\n", res);
        goto out;
    }

    /* PE passes &wine_instance->host_instance as params->instance. Recover
     * the wrapper pointer, host_instance sits at offset 0, and populate the
     * single global dispatch table */
    {
        struct wine_XrInstance *wine_instance = (struct wine_XrInstance *)(void *)params->instance;
        XrInstance host_instance = wine_instance->host_instance;

#define USE_XR_FUNC(x) \
        if (xrGetInstanceProcAddr(host_instance, #x, \
                (PFN_xrVoidFunction *)&g_xr_host_instance_dispatch_table.p_##x) != XR_SUCCESS) \
            g_xr_host_instance_dispatch_table.p_##x = NULL;
        ALL_XR_INSTANCE_FUNCS()
#undef USE_XR_FUNC
    }

    g_instance_live = 1;
    TRACE("Instance created, dispatch table populated\n");

out:
    pthread_mutex_unlock(&g_instance_lock);
    free(new_list);
    params->result = res;
    return STATUS_SUCCESS;
}

NTSTATUS wine_xrDestroyInstance(void *args)
{
    struct xrDestroyInstance_params *params = args;
    struct wine_XrInstance *wine_instance = wine_instance_from_handle(params->instance);
    XrInstance host_instance = wine_instance->host_instance;
    XrResult (*destroy_fn)(XrInstance);

    /* Snapshot the native fn pointer, zero the dispatch table, and clear
     * g_instance_live under the lock before running native xrDestroyInstance.
     * Thunks do not hold g_instance_lock, so a racing thunk either sees a
     * live pointer into a still-live native instance, or a NULL slot that
     * trips the generator's NULL guard and returns XR_ERROR_FUNCTION_UNSUPPORTED.
     * Running native destroy under the lock is not enough, a thunk reads its
     * slot pointer before it takes any lock of its own.
     *
     * The OpenXR spec requires the app to serialize xrDestroyInstance
     * against all other instance-scoped calls, so a thunk during destroy is
     * app-side UB and this fix narrows, not closes, the window */
    pthread_mutex_lock(&g_instance_lock);
    destroy_fn = g_xr_host_instance_dispatch_table.p_xrDestroyInstance;
    memset(&g_xr_host_instance_dispatch_table, 0,
           sizeof(g_xr_host_instance_dispatch_table));
    g_instance_live = 0;
    pthread_mutex_unlock(&g_instance_lock);

    if (destroy_fn)
        params->result = destroy_fn(host_instance);
    else
        params->result = XR_ERROR_FUNCTION_UNSUPPORTED;

    if (params->result != XR_SUCCESS)
        WARN("xrDestroyInstance failed: %d\n", params->result);

    return STATUS_SUCCESS;
}

NTSTATUS wine_xrEnumerateSwapchainFormats(void *args)
{
    struct xrEnumerateSwapchainFormats_params *params = args;
    struct wine_XrSession *wine_session = wine_session_from_handle(params->session);
    struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
    int64_t *native_formats;
    uint32_t i, translated_count, native_count = 0;

    if (!funcs->p_xrEnumerateSwapchainFormats)
    {
        params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
        return STATUS_SUCCESS;
    }

    params->result = funcs->p_xrEnumerateSwapchainFormats(
        wine_session->host_session, 0, &native_count, NULL);
    if (params->result != XR_SUCCESS)
        return STATUS_SUCCESS;

    native_formats = malloc(native_count * sizeof(*native_formats));
    if (!native_formats)
    {
        params->result = XR_ERROR_OUT_OF_MEMORY;
        return STATUS_SUCCESS;
    }

    params->result = funcs->p_xrEnumerateSwapchainFormats(
        wine_session->host_session, native_count, &native_count, native_formats);
    if (params->result != XR_SUCCESS)
    {
        free(native_formats);
        return STATUS_SUCCESS;
    }

    translated_count = 0;
    for (i = 0; i < native_count; i++)
    {
        int64_t dxgi = mtl_format_to_dxgi(native_formats[i]);
        if (!dxgi)
            continue;
        if (params->formats && translated_count < params->formatCapacityInput)
            params->formats[translated_count] = dxgi;
        translated_count++;
    }
    free(native_formats);

    *params->formatCountOutput = translated_count;
    TRACE("Translated %u MTLPixelFormats to %u DXGI formats\n",
          native_count, translated_count);

    if (params->formats && translated_count > params->formatCapacityInput)
    {
        params->result = XR_ERROR_SIZE_INSUFFICIENT;
        return STATUS_SUCCESS;
    }

    params->result = XR_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS wine_xrCreateSwapchain(void *args)
{
    struct xrCreateSwapchain_params *params = args;
    struct wine_XrSession *wine_session = wine_session_from_handle(params->session);
    struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
    XrSwapchainCreateInfo our_info;
    int64_t mtl_fmt;

    if (!funcs->p_xrCreateSwapchain)
    {
        params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
        return STATUS_SUCCESS;
    }

    mtl_fmt = dxgi_format_to_mtl(params->createInfo->format);
    if (!mtl_fmt)
    {
        WARN("Unsupported DXGI format %lld\n", (long long)params->createInfo->format);
        params->result = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        return STATUS_SUCCESS;
    }

    our_info = *params->createInfo;
    our_info.format = mtl_fmt;

    TRACE("Translating DXGI %lld -> MTLPixelFormat %lld\n",
          (long long)params->createInfo->format, (long long)mtl_fmt);

    params->result = funcs->p_xrCreateSwapchain(
        wine_session->host_session, &our_info, params->swapchain);

    return STATUS_SUCCESS;
}

/* PE calls unix_create_d3d11_session instead of unix_xrCreateSession so the
 * D3D11 binding gets substituted for a Metal one on the Unix side. A direct
 * xrCreateSession dispatch would bypass that substitution and fail at native
 * session create. The stub exists so the __wine_unix_call_funcs slot is
 * populated, and any attempted dispatch is a loud no-op */
NTSTATUS wine_xrCreateSession(void *args)
{
    struct xrCreateSession_params *params = args;
    WARN("Direct wine_xrCreateSession dispatch is not supported; "
         "PE side must use unix_create_d3d11_session\n");
    params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
    return STATUS_SUCCESS;
}

#define NANOSECONDS_IN_A_SECOND 1000000000LL
#define TICKSPERSEC             10000000LL

/* On macOS Wine QPC and CLOCK_MONOTONIC advance from non-slewed monotonic
 * clocks during a live XR process. Sample the offset once via pthread_once.
 * Per-call sampling adds jitter, which breaks the CTS
 * XR_KHR_win32_convert_performance_counter_time roundtrip tolerance of
 * 1 QPC tick between QPC->time and time->QPC conversions */
static pthread_once_t qpc_offset_once = PTHREAD_ONCE_INIT;
static LONGLONG qpc_monotonic_offset;

static void qpc_offset_init(void)
{
    LARGE_INTEGER qpc;
    struct timespec ts;
    LONGLONG monotonic_qpc;

    NtQueryPerformanceCounter(&qpc, NULL);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    monotonic_qpc = (LONGLONG)ts.tv_sec * TICKSPERSEC
                  + ts.tv_nsec / (NANOSECONDS_IN_A_SECOND / TICKSPERSEC);

    qpc_monotonic_offset = qpc.QuadPart - monotonic_qpc;
}

static LONGLONG qpc_to_monotonic_offset(void)
{
    pthread_once(&qpc_offset_once, qpc_offset_init);
    return qpc_monotonic_offset;
}

NTSTATUS wine_xrConvertTimeToWin32PerformanceCounterKHR(void *args)
{
    struct xrConvertTimeToWin32PerformanceCounterKHR_params *params = args;
    struct wine_XrInstance *wine_instance = wine_instance_from_handle(params->instance);
    struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
    struct timespec ts;

    if (!funcs->p_xrConvertTimeToTimespecTimeKHR)
    {
        params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
        return STATUS_SUCCESS;
    }

    params->result = funcs->p_xrConvertTimeToTimespecTimeKHR(
        wine_instance->host_instance, params->time, &ts);

    if (params->result == XR_SUCCESS)
    {
        LONGLONG monotonic_qpc = (LONGLONG)ts.tv_sec * TICKSPERSEC
                               + ts.tv_nsec / (NANOSECONDS_IN_A_SECOND / TICKSPERSEC);
        params->performanceCounter->QuadPart = monotonic_qpc + qpc_to_monotonic_offset();
    }

    return STATUS_SUCCESS;
}

NTSTATUS wine_xrConvertWin32PerformanceCounterToTimeKHR(void *args)
{
    struct xrConvertWin32PerformanceCounterToTimeKHR_params *params = args;
    struct wine_XrInstance *wine_instance = wine_instance_from_handle(params->instance);
    struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
    struct timespec ts;
    LONGLONG monotonic_qpc;

    /* Bridge substitutes XR_KHR_win32_convert_performance_counter_time with
     * XR_KHR_convert_timespec_time, whose host-side xrConvertTimespecTimeToTimeKHR
     * does not reject non-positive inputs. Mirror Monado's native QPC validation
     * (oxr_api_instance.c rejects QuadPart <= 0 with XR_ERROR_TIME_INVALID) so
     * the substituted path behaves like the direct one */
    if (params->performanceCounter->QuadPart <= 0)
    {
        params->result = XR_ERROR_TIME_INVALID;
        return STATUS_SUCCESS;
    }

    if (!funcs->p_xrConvertTimespecTimeToTimeKHR)
    {
        params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
        return STATUS_SUCCESS;
    }

    monotonic_qpc = params->performanceCounter->QuadPart - qpc_to_monotonic_offset();
    ts.tv_sec = (time_t)(monotonic_qpc / TICKSPERSEC);
    ts.tv_nsec = (long)((monotonic_qpc % TICKSPERSEC) * (NANOSECONDS_IN_A_SECOND / TICKSPERSEC));

    params->result = funcs->p_xrConvertTimespecTimeToTimeKHR(
        wine_instance->host_instance, &ts, params->time);

    return STATUS_SUCCESS;
}

extern int wait_gpu_fence(void *mtl_listener_ptr,
                          uint64_t mtl_shared_event_ptr,
                          uint64_t fence_value);

/* This TU and metal.m both cache WINEOPENXR_GPU_SYNC_STATS on first use, so
 * normal process startup configuration lands on the same value */
static int gpu_sync_stats_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
    {
        const char *env = getenv("WINEOPENXR_GPU_SYNC_STATS");
        cached = (env && env[0] == '1') ? 1 : 0;
    }
    return cached;
}

static double elapsed_ms(const struct timespec *start, const struct timespec *end)
{
    return ((end->tv_sec - start->tv_sec) * 1e3)
         + ((end->tv_nsec - start->tv_nsec) / 1e6);
}

NTSTATUS wine_xrEndFrame(void *args)
{
    struct xrEndFrame_params *params = args;
    struct wine_XrSession *wine_session = wine_session_from_handle(params->session);
    struct openxr_instance_funcs *funcs = &g_xr_host_instance_dispatch_table;
    struct timespec native_start, native_end;
    int gpu_sync_stats = gpu_sync_stats_enabled();

    if (params->gpu_fence_value && params->mtl_shared_event)
    {
        struct timespec fence_start, fence_end;
        int wait_result;
        if (gpu_sync_stats)
            clock_gettime(CLOCK_MONOTONIC, &fence_start);
        wait_result = wait_gpu_fence(wine_session->mtl_listener,
                                     params->mtl_shared_event,
                                     params->gpu_fence_value);
        if (wait_result < 0)
            WARN("GPU fence wait timed out (value=%llu), submitting anyway\n",
                 (unsigned long long)params->gpu_fence_value);
        if (gpu_sync_stats)
        {
            clock_gettime(CLOCK_MONOTONIC, &fence_end);
            TRACE("gpu_sync xrEndFrame fence_wait_ms=%.2f\n",
                  elapsed_ms(&fence_start, &fence_end));
        }
    }

    if (!funcs->p_xrEndFrame)
    {
        params->result = XR_ERROR_FUNCTION_UNSUPPORTED;
        return STATUS_SUCCESS;
    }

    if (gpu_sync_stats)
        clock_gettime(CLOCK_MONOTONIC, &native_start);

    params->result = funcs->p_xrEndFrame(wine_session->host_session,
                                         params->frameEndInfo);

    if (gpu_sync_stats)
    {
        clock_gettime(CLOCK_MONOTONIC, &native_end);
        TRACE("gpu_sync xrEndFrame native_ms=%.2f\n",
              elapsed_ms(&native_start, &native_end));
    }

    return STATUS_SUCCESS;
}
