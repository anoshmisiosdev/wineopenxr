/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* Raw integer constants because PE lacks Metal enums and Unix lacks dxgi.h */

#ifndef __WINE_OPENXR_FORMAT_TABLE_H
#define __WINE_OPENXR_FORMAT_TABLE_H

#include <stdint.h>

struct format_pair {
    int64_t mtl_format;
    int64_t dxgi_format;
    int is_depth;
};

static const struct format_pair format_translation_table[] = {
    { 80,  87, 0 },  /* MTLPixelFormatBGRA8Unorm            <-> DXGI_FORMAT_B8G8R8A8_UNORM       */
    { 81,  91, 0 },  /* MTLPixelFormatBGRA8Unorm_sRGB       <-> DXGI_FORMAT_B8G8R8A8_UNORM_SRGB  */
    { 70,  28, 0 },  /* MTLPixelFormatRGBA8Unorm            <-> DXGI_FORMAT_R8G8B8A8_UNORM       */
    { 71,  29, 0 },  /* MTLPixelFormatRGBA8Unorm_sRGB       <-> DXGI_FORMAT_R8G8B8A8_UNORM_SRGB  */
    { 115, 10, 0 },  /* MTLPixelFormatRGBA16Float           <-> DXGI_FORMAT_R16G16B16A16_FLOAT   */
    { 252, 40, 1 },  /* MTLPixelFormatDepth32Float          <-> DXGI_FORMAT_D32_FLOAT            */
    { 260, 20, 1 },  /* MTLPixelFormatDepth32Float_Stencil8 <-> DXGI_FORMAT_D32_FLOAT_S8X24_UINT */
};

#define FORMAT_TABLE_SIZE (sizeof(format_translation_table) / sizeof(format_translation_table[0]))

static inline int is_depth_dxgi_format(int64_t dxgi_format)
{
    unsigned int i;
    for (i = 0; i < FORMAT_TABLE_SIZE; i++)
        if (format_translation_table[i].dxgi_format == dxgi_format)
            return format_translation_table[i].is_depth;
    return 0;
}

static inline int64_t mtl_format_to_dxgi(int64_t mtl)
{
    unsigned int i;
    for (i = 0; i < FORMAT_TABLE_SIZE; i++)
        if (format_translation_table[i].mtl_format == mtl)
            return format_translation_table[i].dxgi_format;
    return 0;
}

static inline int64_t dxgi_format_to_mtl(int64_t dxgi)
{
    unsigned int i;
    for (i = 0; i < FORMAT_TABLE_SIZE; i++)
        if (format_translation_table[i].dxgi_format == dxgi)
            return format_translation_table[i].mtl_format;
    return 0;
}

/* xrEnumerateSwapchainFormats advertises concrete DXGI formats. The backing
 * ID3D11Texture2D returned via xrEnumerateSwapchainImages must carry the
 * typeless parent so apps can create views of compatible concrete formats.
 * Formats without a typeless parent stay as-is */
static inline int64_t dxgi_typeless_parent(int64_t dxgi)
{
    switch (dxgi) {
    case 87: /* B8G8R8A8_UNORM         */ return 90; /* B8G8R8A8_TYPELESS      */
    case 91: /* B8G8R8A8_UNORM_SRGB    */ return 90; /* B8G8R8A8_TYPELESS      */
    case 28: /* R8G8B8A8_UNORM         */ return 27; /* R8G8B8A8_TYPELESS      */
    case 29: /* R8G8B8A8_UNORM_SRGB    */ return 27; /* R8G8B8A8_TYPELESS      */
    case 10: /* R16G16B16A16_FLOAT     */ return 9;  /* R16G16B16A16_TYPELESS  */
    case 40: /* D32_FLOAT              */ return 39; /* R32_TYPELESS           */
    case 20: /* D32_FLOAT_S8X24_UINT   */ return 19; /* R32G8X24_TYPELESS      */
    default: return dxgi;
    }
}

#endif /* __WINE_OPENXR_FORMAT_TABLE_H */
