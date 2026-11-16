// modules/video_capture/src/platform/windows/mf_camera.cpp
// ----------------------------------------------------------------------------
// The Windows Media Foundation camera (remote-desktop plan A9).
//
// The camera path on Windows goes through **Media Foundation**'s
// source-reader API:
//
//   1. `MFEnumDeviceSources` (with an attribute store that selects the
//      video-capture source type) enumerates the video-capture
//      devices.
//   2. The chosen `IMFActivate` produces an `IMFMediaSource`.
//   3. `MFCreateSourceReaderFromMediaSource` wraps it in an
//      `IMFSourceReader`.
//   4. The reader's output format is set to **NV12**
//      (`MFVideoFormat_NV12`), which is the format the encoder path
//      wants and the format every UVC camera can deliver.
//   5. `ReadSample` produces an `IMFSample`, whose buffer is converted
//      to a contiguous byte range and handed to the portable layer.
//
// ## The zero-copy path
//
// Media Foundation can deliver a D3D11 texture instead of host memory
// when the source reader is created with the D3D11 manager.  This
// adapter keeps the CPU path — the portable layer's queue copies the
// bytes anyway — and the copy is counted honestly in
// `CaptureStats::bytes_copied`.
//
// ## The threading
//
// The camera device owns its own thread (RD-6): `next_frame()` blocks
// in `ReadSample` there, and the engine's tick thread only ever touches
// the bounded queue.
//
// @note Remote-desktop plan A9.  Platform adapter — the portable half
//       lives in `src/camera_capture.cpp`.
// ----------------------------------------------------------------------------

#include "platform/camera_factory.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace nimrtc::video_capture {

namespace {

using Microsoft::WRL::ComPtr;

/**
 * @brief `MF_E_ENDOFSTREAM`.
 *
 * The constant is defined in `mfapi.h` only when the Media Foundation
 * error header is included; it is spelled out here so that the adapter
 * does not depend on which of the two headers the SDK exposes.
 */
constexpr HRESULT kMfEndOfStream = static_cast<HRESULT>(0x0000000Cu);

/**
 * @brief The Media Foundation camera frame source.
 */
class MfCameraSource final : public ICameraFrameSource {
  public:
    MfCameraSource() = default;

    ~MfCameraSource() override {
        close();
        if (mf_started_) {
            ::MFShutdown();
            mf_started_ = false;
        }
    }

    MfCameraSource(const MfCameraSource&) = delete;
    MfCameraSource& operator=(const MfCameraSource&) = delete;

    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_cameras() const noexcept override {
        std::vector<plugins::CaptureDevice> devices;

        std::vector<ComPtr<IMFActivate>> activations;
        if (!enumerate_video_devices(activations)) return devices;

        for (const auto& activate : activations) {
            plugins::CaptureDevice device;
            device.id = attribute_string(activate.Get(),
                                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            device.name = attribute_string(activate.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            if (device.id.empty()) {
                // A device without a symbolic link cannot be opened by
                // id, so it is not offered.
                continue;
            }
            device.kind = plugins::CaptureDeviceKind::kCamera;

            // The native size is *not* an activation attribute on
            // Windows: it has to be read from the device's native media
            // types.  A device that cannot be activated is still
            // offered, with a conservative default size.
            device.max_width = 640;
            device.max_height = 480;
            device.max_fps = 30;
            query_native_geometry(activate.Get(), device.max_width, device.max_height,
                                  device.max_fps);

            devices.push_back(std::move(device));
        }

        return devices;
    }

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override {
        std::vector<plugins::CaptureFormat> formats;

        // The NV12 output at a few common ladder rungs.  A camera
        // delivers whatever the sensor offers; the negotiation clamps a
        // larger request down to the device's maximum, so the maximum
        // is what matters here.
        const auto devices = enumerate_cameras();
        std::uint32_t max_w = 0;
        std::uint32_t max_h = 0;
        std::uint32_t max_fps = 30;
        for (const auto& d : devices) {
            if (!device_id.empty() && d.id != device_id) continue;
            if (d.max_width > max_w) max_w = d.max_width;
            if (d.max_height > max_h) max_h = d.max_height;
            if (d.max_fps > max_fps) max_fps = d.max_fps;
        }
        if (max_w == 0 || max_h == 0) return formats;

        const std::uint32_t sizes[][2] = {{640, 480}, {1280, 720}, {max_w, max_h}};
        for (const auto& s : sizes) {
            if (s[0] > max_w || s[1] > max_h) continue;
            plugins::CaptureFormat format;
            format.width = s[0];
            format.height = s[1];
            format.fps = max_fps;
            format.tag = "nv12-mediafoundation";
            format.zero_copy = false;
            format.has_dirty_rects = false;
            formats.push_back(std::move(format));
        }
        return formats;
    }

    plugins::Status open(const std::string& device_id,
                         const plugins::CaptureFormat& format) noexcept override {
        close();

        if (!mf_started_) {
            if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
                return plugins::kErrHardwareError;
            }
            mf_started_ = true;
        }

        // ---- The device ---------------------------------------------------
        std::vector<ComPtr<IMFActivate>> activations;
        if (!enumerate_video_devices(activations)) {
            return plugins::kErrHardwareError;
        }

        ComPtr<IMFActivate> activate;
        for (const auto& candidate : activations) {
            if (attribute_string(candidate.Get(),
                                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) ==
                device_id) {
                activate = candidate;
                break;
            }
        }
        if (!activate) return plugins::kErrInvalidParam;

        ComPtr<IMFMediaSource> media_source;
        if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&media_source)))) {
            return plugins::kErrHardwareError;
        }

        ComPtr<IMFSourceReader> reader;
        if (FAILED(::MFCreateSourceReaderFromMediaSource(media_source.Get(), nullptr,
                                                         &reader))) {
            return plugins::kErrHardwareError;
        }

        // ---- The format ----------------------------------------------------
        //
        // The reader's output is set to NV12, which is what the encoder
        // path wants.  The device's native type is the starting point
        // (the reader rejects a type it cannot reach directly), so the
        // subtype is replaced and the attributes that describe the size
        // are kept.
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &native))) {
            return plugins::kErrHardwareError;
        }

        ComPtr<IMFMediaType> wanted;
        if (FAILED(::MFCreateMediaType(&wanted))) return plugins::kErrHardwareError;

        UINT32 width = 0;
        UINT32 height = 0;
        if (get_frame_size(native.Get(), width, height)) {
            if (format.width != 0 && format.height != 0) {
                width = format.width;
                height = format.height;
            }
            set_frame_size(wanted.Get(), width, height);
        }
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);

        // The reader may refuse the type outright (the device cannot
        // produce NV12 at that size).  In that case the native type is
        // used and the *actual* geometry is read back below.
        const HRESULT hr = reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, wanted.Get());
        if (FAILED(hr)) {
            if (FAILED(reader->SetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                    native.Get()))) {
                return plugins::kErrUnsupported;
            }
        }

        // The actual geometry, after the negotiation.
        ComPtr<IMFMediaType> actual;
        if (SUCCEEDED(reader->GetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &actual))) {
            UINT32 w = 0;
            UINT32 h = 0;
            if (get_frame_size(actual.Get(), w, h) && w != 0 && h != 0) {
                width = w;
                height = h;
            }
        }
        if (width == 0 || height == 0) return plugins::kErrUnsupported;

        width_ = width;
        height_ = height;
        // The NV12 stride: the luma plane is `width` bytes per row, the
        // chroma plane is `width` bytes per row as well (one UV pair
        // per two luma samples).
        stride_ = static_cast<std::int32_t>(width);
        frame_size_ = static_cast<std::size_t>(width) * height * 3u / 2u;

        reader_ = reader;
        media_source_ = media_source;
        return plugins::kOk;
    }

    void close() noexcept {
        reader_.Reset();
        media_source_.Reset();
        frame_size_ = 0;
        width_ = 0;
        height_ = 0;
    }

    plugins::Status next_frame(std::vector<std::uint8_t>& pixels, std::size_t& size,
                               std::int32_t& stride, std::uint32_t& width,
                               std::uint32_t& height, std::int64_t& ts) noexcept override {
        if (!reader_) return plugins::kErrNotReady;

        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader_->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags,
            &timestamp, &sample);
        if (hr == kMfEndOfStream) return plugins::kErrNotReady;
        if (FAILED(hr)) return plugins::kErrHardwareError;
        if (!sample) return plugins::kErrNotReady;

        // The sample's buffer is made contiguous, then copied out.
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            return plugins::kErrHardwareError;
        }

        BYTE* data = nullptr;
        DWORD data_size = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &data_size))) {
            return plugins::kErrHardwareError;
        }

        if (pixels.size() < data_size) pixels.resize(data_size);
        if (data != nullptr && data_size > 0) {
            std::memcpy(pixels.data(), data, data_size);
        }
        buffer->Unlock();

        size = data_size;
        stride = stride_;
        width = width_;
        height = height_;
        // Media Foundation's timestamps are in 100-nanosecond units;
        // the seam wants microseconds.
        ts = (timestamp >= 0) ? (timestamp / 10) : 0;
        return plugins::kOk;
    }

  private:
    /**
     * @brief Enumerate the video-capture devices.
     *
     * The device-source attribute store is built explicitly: it is the
     * documented way to select the video-capture source type, and it
     * works on every Windows version the engine supports.
     */
    static bool enumerate_video_devices(std::vector<ComPtr<IMFActivate>>& out) noexcept {
        ComPtr<IMFAttributes> attributes;
        if (FAILED(::MFCreateAttributes(&attributes, 1))) return false;
        if (FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
            return false;
        }

        IMFActivate** raw = nullptr;
        UINT32 count = 0;
        if (FAILED(::MFEnumDeviceSources(attributes.Get(), &raw, &count)) || raw == nullptr) {
            return false;
        }

        out.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            if (raw[i] != nullptr) out.emplace_back(raw[i]);
        }
        ::CoTaskMemFree(raw);
        return !out.empty();
    }

    /**
     * @brief Read the device's native geometry from its media types.
     *
     * The activation object has to be activated to expose its native
     * types, which is expensive; the *largest* native type is used as
     * the device's ceiling.  A failure here is not fatal: the caller
     * keeps its conservative default.
     */
    static void query_native_geometry(IMFActivate* activate, std::uint32_t& max_width,
                                       std::uint32_t& max_height,
                                       std::uint32_t& max_fps) noexcept {
        ComPtr<IMFMediaSource> media_source;
        if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&media_source)))) return;

        ComPtr<IMFSourceReader> reader;
        if (FAILED(::MFCreateSourceReaderFromMediaSource(media_source.Get(), nullptr,
                                                         &reader))) {
            return;
        }

        // The native types are enumerated until the end; the *largest*
        // one is the device's ceiling.
        for (DWORD index = 0; index < 64; ++index) {
            ComPtr<IMFMediaType> type;
            if (FAILED(reader->GetNativeMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), index, &type))) {
                break;
            }
            UINT32 width = 0;
            UINT32 height = 0;
            if (get_frame_size(type.Get(), width, height)) {
                if (width != 0) max_width = std::max(max_width, width);
                if (height != 0) max_height = std::max(max_height, height);
            }
            UINT32 fps_num = 0;
            UINT32 fps_den = 0;
            if (get_frame_rate(type.Get(), fps_num, fps_den) && fps_den != 0) {
                const UINT32 fps = fps_num / fps_den;
                if (fps != 0) max_fps = std::max(max_fps, fps);
            }
        }
    }

    /**
     * @brief Read the frame rate from a media type.
     *
     * Media Foundation stores the frame rate as a packed UINT64
     * (`MF_MT_FRAME_RATE`): the numerator in the high half and the
     * denominator in the low half.
     */
    static bool get_frame_rate(IMFMediaType* type, UINT32& numerator,
                                UINT32& denominator) noexcept {
        UINT64 packed = 0;
        if (FAILED(type->GetUINT64(MF_MT_FRAME_RATE, &packed))) return false;
        numerator = static_cast<UINT32>(packed >> 32);
        denominator = static_cast<UINT32>(packed & 0xFFFFFFFFull);
        return true;
    }

    /**
     * @brief Read the frame size from a media type.
     *
     * Media Foundation stores the frame size as a packed UINT64
     * (`MF_MT_FRAME_SIZE`); the width is in the high half and the
     * height in the low half.
     */
    static bool get_frame_size(IMFMediaType* type, UINT32& width, UINT32& height) noexcept {
        UINT64 packed = 0;
        if (FAILED(type->GetUINT64(MF_MT_FRAME_SIZE, &packed))) return false;
        width = static_cast<UINT32>(packed >> 32);
        height = static_cast<UINT32>(packed & 0xFFFFFFFFull);
        return true;
    }

    /** Write the frame size into a media type (packed UINT64). */
    static void set_frame_size(IMFMediaType* type, UINT32 width, UINT32 height) noexcept {
        const UINT64 packed = (static_cast<UINT64>(width) << 32) | static_cast<UINT64>(height);
        type->SetUINT64(MF_MT_FRAME_SIZE, packed);
    }

    static std::string attribute_string(IMFActivate* activate, const GUID& key) noexcept {
        UINT32 length = 0;
        if (FAILED(activate->GetStringLength(key, &length)) || length == 0) return {};
        std::wstring wide(length + 1, L'\0');
        UINT32 copied = 0;
        if (FAILED(activate->GetString(key, wide.data(), length + 1, &copied))) return {};
        if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
        if (wide.empty()) return {};

        // The symbolic link is a wide string; the device id is its
        // UTF-8 form (the engine's device ids are UTF-8 throughout).
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                                 static_cast<int>(wide.size()), nullptr, 0,
                                                 nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<std::size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                              out.data(), needed, nullptr, nullptr);
        return out;
    }

    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFMediaSource> media_source_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::int32_t stride_ = 0;
    std::size_t frame_size_ = 0;
    bool mf_started_ = false;
};

} // anonymous namespace

namespace detail {

plugins::IVideoCapture* create_camera_capture(plugins::CaptureConfig cfg, CameraBackend backend) {
    if (backend != CameraBackend::kMediaFoundation) {
        // The other backends are not available on Windows.
        return nullptr;
    }
    auto source = std::make_unique<MfCameraSource>();
    CameraCaptureOptions opts;
    opts.backend = backend;
    opts.pixel_format = plugins::VideoPixelFormat::kNV12;
    return new CameraCapture(std::move(cfg), std::move(source), opts);
}

} // namespace detail

} // namespace nimrtc::video_capture

#endif // defined(_WIN32)
