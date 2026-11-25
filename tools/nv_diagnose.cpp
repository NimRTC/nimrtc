// A diagnostic for the NVIDIA video backends on *this* machine.
//
// It answers the two questions the NimRTC logs cannot:
//
//   1. Which adapters does this machine have, and which one is the
//      *default* that D3D11CreateDevice(nullptr, HARDWARE) picks?  On a
//      laptop with an Intel iGPU plus an NVIDIA dGPU, the default is
//      often the Intel one — and NVENC refuses to open a session on a
//      non-NVIDIA device.
//
//   2. What does the NVDEC capability query answer, on a *correctly
//      pushed* CUDA context, for each codec?  (The context has to be
//      pushed by value: `cuCtxPushCurrent(CUcontext)`, not a pointer.)
//
// The tool prints the driver version, the adapter list, the default
// adapter, and the caps answer for every adapter it can open a session
// on.  It is read-only: it opens a session, asks the driver questions,
// and closes it again.
//
// Build (from the tree root):
//   cl /EHsc /std:c++20 /I src\third_party\nv_codec_headers\include \
//      /I src\third_party\nv_codec_headers\include\ffnvcodec \
//      tools\nv_diagnose.cpp /Fe:nv_diagnose.exe /link d3d11.lib dxgi.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>

#define FFNV_LOG_FUNC(logctx, ...) ((void)0)
#define FFNV_DEBUG_LOG_FUNC(logctx, ...) ((void)0)
#include "dynlink_loader.h"

#include <ffnvcodec/nvEncodeAPI.h>

namespace {

using CapsFn = CUresult(CUDAAPI*)(CUVIDDECODECAPS*);

template <typename Fn>
Fn load(HMODULE dll, const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(dll, name));
}

/** The NVENC version the driver speaks, or 0 when it cannot be asked. */
std::uint32_t driver_max_version() {
    HMODULE dll = LoadLibraryA("nvEncodeAPI64.dll");
    if (!dll) return 0;
    auto fn = load<NVENCSTATUS(CUDAAPI*)(std::uint32_t*)>(
        dll, "NvEncodeAPIGetMaxSupportedVersion");
    std::uint32_t v = 0;
    if (fn && fn(&v) == NV_ENC_SUCCESS) {
        FreeLibrary(dll);
        return v;
    }
    FreeLibrary(dll);
    return 0;
}

void print_adapters() {
    printf("\n== the adapters this machine has ==\n");
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) ||
        !factory) {
        printf("  CreateDXGIFactory1 failed\n");
        return;
    }
    IDXGIAdapter1* def = nullptr;
    if (SUCCEEDED(factory->EnumAdapters1(0, &def))) {
        DXGI_ADAPTER_DESC1 d{};
        def->GetDesc1(&d);
        printf("  the DEFAULT adapter (what D3D11CreateDevice(nullptr) picks): "
               "\"%ls\" vendor=0x%04X device=0x%04X\n",
               d.Description, d.VendorId, d.DeviceId);
        def->Release();
    }
    IDXGIAdapter1* adapter = nullptr;
    int index = 0;
    while (factory->EnumAdapters1(index++, &adapter) == S_OK) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        const bool is_nvidia = (d.VendorId == 0x10DE);
        printf("  adapter %d: \"%ls\" vendor=0x%04X device=0x%04X%s\n",
               index - 1, d.Description, d.VendorId, d.DeviceId,
               is_nvidia ? "  <- NVIDIA" : "");
        adapter->Release();
    }
    factory->Release();
}

/** Try to open an NVENC session on one adapter. */
bool try_session(IDXGIAdapter1* adapter, void** session, NVENCSTATUS* status) {
    HMODULE dll = LoadLibraryA("nvEncodeAPI64.dll");
    if (!dll) return false;
    auto create = load<NVENCSTATUS(CUDAAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(
        dll, "NvEncodeAPICreateInstance");
    if (!create) { FreeLibrary(dll); return false; }

    NV_ENCODE_API_FUNCTION_LIST fn{};
    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create(&fn) != NV_ENC_SUCCESS) { FreeLibrary(dll); return false; }

    ID3D11Device* dev = nullptr;
    const HRESULT hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    if (FAILED(hr) || !dev) { FreeLibrary(dll); return false; }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS p{};
    p.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    p.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    p.device     = dev;
    p.apiVersion = NVENCAPI_VERSION;
    *status = fn.nvEncOpenEncodeSessionEx(&p, session);

    if (*status == NV_ENC_SUCCESS && fn.nvEncDestroyEncoder && *session) {
        fn.nvEncDestroyEncoder(*session);
        *session = nullptr;
    }
    dev->Release();
    FreeLibrary(dll);
    return true;
}

void print_sessions() {
    printf("\n== the NVENC session on each adapter ==\n");
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) ||
        !factory) {
        printf("  CreateDXGIFactory1 failed\n");
        return;
    }
    IDXGIAdapter1* adapter = nullptr;
    int index = 0;
    while (factory->EnumAdapters1(index++, &adapter) == S_OK) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        void* session = nullptr;
        NVENCSTATUS st = NV_ENC_ERR_GENERIC;
        if (try_session(adapter, &session, &st)) {
            printf("  adapter %d (\"%ls\"): status=%d %s\n",
                   index - 1, d.Description, int(st),
                   st == NV_ENC_SUCCESS ? "(OK)" : "");
        } else {
            printf("  adapter %d (\"%ls\"): no session could be attempted\n",
                   index - 1, d.Description);
        }
        adapter->Release();
    }
    factory->Release();
}

void print_caps() {
    printf("\n== the NVDEC capability query ==\n");
    HMODULE cuda = LoadLibraryA("nvcuda.dll");
    HMODULE cuvid = LoadLibraryA("nvcuvid.dll");
    if (!cuda || !cuvid) {
        printf("  nvcuda.dll=%p nvcuvid.dll=%p — cannot query\n",
               (void*)cuda, (void*)cuvid);
        return;
    }
    auto cuInit = load<CUresult(CUDAAPI*)(unsigned)>(cuda, "cuInit");
    auto cuDeviceGetCount = load<CUresult(CUDAAPI*)(int*)>(cuda, "cuDeviceGetCount");
    auto cuDeviceGet = load<CUresult(CUDAAPI*)(CUdevice*, int)>(cuda, "cuDeviceGet");
    auto cuDeviceGetAttribute =
        load<CUresult(CUDAAPI*)(int*, CUdevice_attribute, CUdevice)>(cuda, "cuDeviceGetAttribute");
    auto cuDevicePrimaryCtxRetain =
        load<CUresult(CUDAAPI*)(CUcontext*, CUdevice)>(cuda, "cuDevicePrimaryCtxRetain");
    // NOTE: cuCtxPushCurrent takes the context BY VALUE.
    auto cuCtxPushCurrent = load<CUresult(CUDAAPI*)(CUcontext)>(cuda, "cuCtxPushCurrent_v2");
    if (!cuCtxPushCurrent) {
        cuCtxPushCurrent = load<CUresult(CUDAAPI*)(CUcontext)>(cuda, "cuCtxPushCurrent");
    }
    auto cuCtxPopCurrent = load<CUresult(CUDAAPI*)(CUcontext*)>(cuda, "cuCtxPopCurrent_v2");
    if (!cuCtxPopCurrent) {
        cuCtxPopCurrent = load<CUresult(CUDAAPI*)(CUcontext*)>(cuda, "cuCtxPopCurrent");
    }
    auto caps = load<CapsFn>(cuvid, "cuvidGetDecoderCaps");

    cuInit(0);
    int count = 0;
    cuDeviceGetCount(&count);
    printf("  CUDA devices: %d\n", count);

    for (int i = 0; i < count; ++i) {
        CUdevice dev = 0;
        if (cuDeviceGet(&dev, i) != CUDA_SUCCESS) continue;
        char name[256] = {0};
        if (auto fn = load<CUresult(CUDAAPI*)(char*, int, CUdevice)>(cuda, "cuDeviceGetName")) {
            fn(name, sizeof(name), dev);
        }
        int major = 0, minor = 0;
        cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
        cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
        printf("  CUDA device %d: \"%s\" CC %d.%d\n", i, name, major, minor);

        CUcontext ctx = nullptr;
        if (cuDevicePrimaryCtxRetain(&ctx, dev) != CUDA_SUCCESS) {
            printf("    cuDevicePrimaryCtxRetain failed\n");
            continue;
        }
        const CUresult pr = cuCtxPushCurrent(ctx);   // by value!
        printf("    cuCtxPushCurrent -> %d\n", int(pr));

        const cudaVideoCodec kCodecs[] = {
            cudaVideoCodec_H264, cudaVideoCodec_HEVC, cudaVideoCodec_AV1};
        for (cudaVideoCodec codec : kCodecs) {
            CUVIDDECODECAPS c{};
            c.eCodecType      = codec;
            c.eChromaFormat   = cudaVideoChromaFormat_420;
            c.nBitDepthMinus8 = 0;
            const CUresult r = caps(&c);
            printf("    codec=%2d -> CUresult=%d bIsSupported=%u nNumNVDECs=%u "
                   "max=%ux%u\n",
                   int(codec), int(r), unsigned(c.bIsSupported),
                   unsigned(c.nNumNVDECs), c.nMaxWidth, c.nMaxHeight);
        }
        cuCtxPopCurrent(nullptr);
    }
}

} // namespace

int main() {
    printf("NVIDIA video backend diagnose\n");
    printf("this build speaks the NVENCAPI %d.%d ABI\n",
           NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    const std::uint32_t max = driver_max_version();
    if (max) {
        printf("the installed driver supports up to NVENCAPI %u.%u\n",
               (max >> 4) & 0xFF, max & 0x0F);
    } else {
        printf("the driver's maximum version could not be queried\n");
    }
    print_adapters();
    print_sessions();
    print_caps();
    printf("\ndone\n");
    return 0;
}
