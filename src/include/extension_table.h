/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_EXTENSION_TABLE_H
#define __WINE_OPENXR_EXTENSION_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include <openxr/openxr_platform.h>

#ifndef TRUE
#define TRUE 1
#define FALSE 0
typedef int BOOL;
#endif

/* Policy for Win32 aliases and bridge-synthesised function gates lives here.
 * src/unix/openxr.c owns the mechanism.
 *
 * Every row maps the app-visible Win32 extension name to the host-visible
 * native name for the same feature. Enumeration rewrites native_ext to
 * win32_ext for the app, and xrCreateInstance rewrites win32_ext to native_ext
 * for the host.
 *
 * Native aliases stay hidden from app enumeration and are rejected if the app
 * passes them to xrCreateInstance. Exposing only the Win32 spelling avoids
 * duplicate host extension names after xrCreateInstance translation.
 *
 * synthetic_mappings is a NULL-terminated map from bridge-synthesised Win32
 * function names to the host functions the bridge wraps. xrGetInstanceProcAddr
 * forwards the mapped host name to preserve the app-enabled extension state */
struct synthetic_function_mapping {
    const char *synthetic_name;
    const char *host_gate_name;
};

struct extension_substitution {
    const char *win32_ext;
    const char *native_ext;
    uint32_t version;
    const struct synthetic_function_mapping *synthetic_mappings;
};

static const struct synthetic_function_mapping d3d11_synthetic_mappings[] = {
    {"xrGetD3D11GraphicsRequirementsKHR", "xrGetMetalGraphicsRequirementsKHR"},
    {NULL, NULL},
};

static const struct synthetic_function_mapping win32_time_synthetic_mappings[] = {
    {"xrConvertWin32PerformanceCounterToTimeKHR", "xrConvertTimespecTimeToTimeKHR"},
    {"xrConvertTimeToWin32PerformanceCounterKHR", "xrConvertTimeToTimespecTimeKHR"},
    {NULL, NULL},
};

static const struct extension_substitution substitute_extensions[] = {
    {"XR_KHR_D3D11_enable", "XR_KHR_metal_enable",
     XR_KHR_D3D11_enable_SPEC_VERSION,
     d3d11_synthetic_mappings},
    {"XR_KHR_win32_convert_performance_counter_time",
     "XR_KHR_convert_timespec_time",
     XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION,
     win32_time_synthetic_mappings},
};

#endif /* __WINE_OPENXR_EXTENSION_TABLE_H */
