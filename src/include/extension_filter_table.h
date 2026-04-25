/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_EXTENSION_FILTER_TABLE_H
#define __WINE_OPENXR_EXTENSION_FILTER_TABLE_H

/* Default policy is pass-through. Hide host extensions when the PE/Unix bridge
 * cannot expose app callbacks, unthunked commands or handles, unsupported
 * graphics bindings, or frame-submission payloads that xrEndFrame does not
 * rewrite */
static const char *const blocked_extensions[] = {
    "XR_FB_space_warp",
    "XR_EXT_frame_synthesis",
    "XR_MSFT_secondary_view_configuration",
    "XR_MSFT_composition_layer_reprojection",
    "XR_FB_passthrough",
    "XR_FB_passthrough_keyboard_hands",
    "XR_META_passthrough_color_lut",
    "XR_META_passthrough_layer_resumed_event",
    "XR_HTC_passthrough",
    "XR_ANDROID_composition_layer_passthrough_mesh",
    "XR_EXT_debug_utils",
    "XR_KHR_vulkan_enable",
    "XR_KHR_vulkan_enable2",
    "XR_KHR_opengl_enable",
    "XR_KHR_opengl_es_enable",
};

#endif /* __WINE_OPENXR_EXTENSION_FILTER_TABLE_H */
