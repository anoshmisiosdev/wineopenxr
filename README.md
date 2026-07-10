# wineopenxr

> [!WARNING]
> This is a vibe-coded OpenXR bridge for running D3D11 OpenXR apps on macOS through CrossOver. It works but has not been thoroughly tested. Use at your own risk.

The bridge supports apps that:

* Use OpenXR
* Use D3D11
* Run under Wine on Linux (if an app doesn't run there, it's unlikely to run here)

## How it works

wineopenxr forwards every call from an OpenXR D3D11 app to a native OpenXR runtime on the host. D3D11 swapchain textures are shared with the host runtime as `MTLTexture` handles without a GPU copy. DXMT's `IMTLD3D11InteropDevice` handles the D3D11 to Metal interop on the PE side.

```
OpenXR D3D11 app
        |
        v
  wineopenxr.dll
        |
        v
  wineopenxr.so
        |
        v
Native OpenXR runtime
```

The bridge ships as two halves. `wineopenxr.dll` runs as a Wine PE builtin inside the Windows process. `wineopenxr.so` runs on the host side (x86_64 under Rosetta) and talks to the native OpenXR loader. They communicate through Wine's `__wine_unix_call_dispatcher`.

## Prerequisites

* macOS 15+ on Apple Silicon
* Xcode with the Metal toolchain
* CrossOver 26 with the [DXMT fork](https://github.com/monofunc/dxmt) installed and selected as Graphics in your bottle
* An x86_64 OpenXR runtime that exposes `XR_KHR_metal_enable`, such as the [Monado fork](https://github.com/monofunc/monado)

Install build tools:

```bash
brew install cmake mingw-w64
```

## Building

```bash
git submodule update --init
cmake -B build
cmake --build build
```

This produces `build/src/pe/wineopenxr.dll` and `build/src/unix/wineopenxr.so`.

## Installing

Copy the artifacts into CrossOver's Wine tree:

```bash
CX_WINE=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine
cp build/src/pe/wineopenxr.dll "$CX_WINE/x86_64-windows/wineopenxr.dll"
cp build/src/unix/wineopenxr.so "$CX_WINE/x86_64-unix/wineopenxr.so"
```

Register the bridge as the active OpenXR runtime in your bottle:

```bash
BOTTLE=<BottleName>
WINEPREFIX="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
WINE=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine

cp build/src/pe/wineopenxr.dll "$WINEPREFIX/drive_c/windows/system32/wineopenxr.dll"

mkdir -p "$WINEPREFIX/drive_c/openxr"
cp manifests/wineopenxr64.json "$WINEPREFIX/drive_c/openxr/"

WINEPREFIX="$WINEPREFIX" CX_BOTTLE="$BOTTLE" "$WINE" reg add \
  'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ \
  /d 'C:\openxr\wineopenxr64.json' /f
```

## Running

Start your native OpenXR runtime, then launch the app through CrossOver GUI or Steam as usual. The bridge loads automatically via the bottle's registry entry.

## Diagnostics

Enable Wine's `+openxr` debug channel to capture bridge logs from both `wineopenxr.dll` and `wineopenxr.so`.

## License

LGPL-2.1-or-later. See `LICENSE` and `COPYING.LIB`.
