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
    {"XR_KHR_win32_convert_performance_counter_time",
     "XR_KHR_convert_timespec_time",
     XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION},
};

#endif /* __WINE_OPENXR_EXTENSION_SUBSTITUTIONS_H */
