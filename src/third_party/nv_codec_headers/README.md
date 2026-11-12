# nv-codec-headers (vendored)

NVIDIA Video Codec SDK headers — `nvEncodeAPI.h` (NVENC) and the
`nvcuvid` / CUVID headers (NVDEC).

## Provenance

| | |
|---|---|
| Upstream | <https://github.com/FFmpeg/nv-codec-headers> (mirror of <https://code.ffmpeg.org/ffmpeg/nv-codec-headers>) |
| Version | `n13.1.15.0` (Video Codec SDK 13.1.15) |
| Commit | `0a6fba9` (release tag `n13.1.15.0`) |
| License | MIT — the full notice is embedded at the top of every header |

These are NVIDIA's own headers, redistributed by the FFmpeg project under
the MIT license. `include/ffnvcodec/nvEncodeAPI.h` carries the NVIDIA
copyright notice and the MIT permission text verbatim.

## Why vendored

The NVIDIA Video Codec SDK itself is a large proprietary download that is
*not* redistributable. The headers, however, are MIT-licensed and are
exactly what a build needs: at runtime NimRTC loads `nvEncodeAPI64.dll` /
`nvcuvid.dll` from the installed NVIDIA driver with `LoadLibrary`, so no
NVIDIA binary is linked or shipped.

Vendoring the headers keeps the NVENC/NVDEC build reproducible on
machines without the SDK installed, and — more importantly — pins the
struct layout to a single known ABI. See the "ABI consistency" note below.

## ABI consistency — why the version macros must not be overridden

`nvEncodeAPI.h` defines the struct version macros for exactly one SDK
version. The struct *layouts* and the version *words* must come from the
same header set. If a translation unit includes this header but
`#undef`s a version macro and substitutes a different SDK's version word
(for example pinning `NV_ENC_PIC_PARAMS_VER` to the SDK 11.x type ID
`0x0B` while the struct in memory still has the SDK 13.1 layout), then:

* the driver reads the version word, decides the payload is an 11.x
  struct, and interprets the 13.x layout with 11.x offsets;
* field offsets disagree, the driver reads past the end of the struct it
  expects, and the process corrupts its heap.

NimRTC therefore uses the version macros from this header **as-is**, and
selects the SDK-13.1 struct sub-version that the *driver* accepts by
probing at runtime. See `src/modules/h264/src/nvenc_encoder.cpp` and
`docs/hw_plugin_seam.md` §12.1.

## Contents

```
include/ffnvcodec/
  nvEncodeAPI.h        — NVENC (encoder) API
  dynlink_nvcuvid.h    — CUVID (decoder) dynamic-link loader
  dynlink_cuviddec.h   — CUVID decoder entry points
  dynlink_cuda.h       — CUDA dynamic-link loader (needed by dynlink_*)
  dynlink_loader.h     — shared loader helpers
```
