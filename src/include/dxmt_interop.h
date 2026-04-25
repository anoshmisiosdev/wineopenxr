/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __WINE_OPENXR_DXMT_INTEROP_H
#define __WINE_OPENXR_DXMT_INTEROP_H

#include <unknwn.h>
#include <d3d11_4.h>

DEFINE_GUID(IID_IMTLD3D11InteropDevice,
    0x8b6fc874, 0x7429, 0x430d,
    0x82, 0x53, 0x52, 0x2e, 0x29, 0x6c, 0xd8, 0xe2);

#undef INTERFACE
#define INTERFACE IMTLD3D11InteropDevice
DECLARE_INTERFACE_(IMTLD3D11InteropDevice, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, void **ppvObject) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(ImportMTLTexture2D)(THIS_
        const D3D11_TEXTURE2D_DESC1 *pDesc,
        UINT64 mtlTexture,
        ID3D11Texture2D **ppTexture2D) PURE;

    STDMETHOD(GetFenceSharedEvent)(THIS_
        ID3D11Fence *pFence,
        UINT64 *pMtlSharedEvent) PURE;
};
#undef INTERFACE

_Static_assert(sizeof(IMTLD3D11InteropDeviceVtbl) == 5 * sizeof(void*), "vtable mismatch");

#endif /* __WINE_OPENXR_DXMT_INTEROP_H */
