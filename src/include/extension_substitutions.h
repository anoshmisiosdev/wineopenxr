/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_EXTENSION_SUBSTITUTIONS_H
#define __WINE_OPENXR_EXTENSION_SUBSTITUTIONS_H

#include <stddef.h>
#include <stdint.h>

#include <openxr/openxr_platform.h>

#ifndef TRUE
#define TRUE 1
#define FALSE 0
typedef int BOOL;
#endif

/* Policy for Win32 aliases lives here. src/unix/openxr.c owns the mechanism.
 *
 * Every row maps the app-visible Win32 extension name to the host-visible
 * native name for the same feature. Enumeration rewrites native_ext to
 * win32_ext for the app, and xrCreateInstance rewrites win32_ext to native_ext
 * for the host.
 *
 * Native aliases stay hidden from app enumeration and are rejected if the app
 * passes them to xrCreateInstance. Exposing only the Win32 spelling avoids
 * duplicate host extension names after xrCreateInstance translation */
struct extension_substitution {
    const char *win32_ext;
    const char *native_ext;
    uint32_t version;
};

static const struct extension_substitution substitute_extensions[] = {
    {"XR_KHR_D3D11_enable", "XR_KHR_metal_enable",
     XR_KHR_D3D11_enable_SPEC_VERSION},
    /* D3D12 swapchain images are D3DMetal resources substituted with the
     * runtime's MTLTextures (src/unix/dmsubst.m); no DXMT D3D12 path */
    {"XR_KHR_D3D12_enable", "XR_KHR_metal_enable",
     XR_KHR_D3D12_enable_SPEC_VERSION},
};

/* Extensions the bridge implements entirely on its own side, with no native
 * host counterpart. Unlike substitute_extensions above, these are advertised
 * to the app unconditionally (not gated on the host reporting some native
 * alias), and never forwarded to the host's xrCreateInstance at all -
 * extension_is_bridge_only() in src/unix/openxr.c reads this table to
 * filter them out.
 *
 * XR_KHR_win32_convert_performance_counter_time lives here because OXRSys
 * does not implement XR_KHR_convert_timespec_time (the extension this used
 * to be substituted for), so it can never be advertised via the mechanism
 * above. The bridge instead implements the QPC<->XrTime math itself in
 * src/unix/openxr.c, pivoting through CLOCK_MONOTONIC, since empirically
 * OXRSys's XrTime values already run at CLOCK_MONOTONIC's rate (same
 * nanosecond unit, 1:1 rate, fixed but arbitrary epoch offset) */
static const struct extension_substitution bridge_only_extensions[] = {
    {"XR_KHR_win32_convert_performance_counter_time", NULL,
     XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION},
};

#endif /* __WINE_OPENXR_EXTENSION_SUBSTITUTIONS_H */
