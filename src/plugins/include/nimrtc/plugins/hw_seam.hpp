/**
 * @file nimrtc/plugins/hw_seam.hpp
 * @brief Hardware-accelerated plugin seam — abstract base interfaces for
 *        out-of-tree HW plugins (NDK MediaCodec, VideoToolbox, NVDEC,
 *        NVENC, VAAPI, DirectX Video Acceleration, Intel Quick Sync).
 *
 * ## Why this header exists
 *
 * The four "reference" plugin interfaces (IVideoSource / IVideoSink /
 * IVideoReceiver / IVideoSender) are codec- and platform-agnostic — they
 * describe the data flow without committing to any specific HW backend.
 * HW vendors implementing real-world integrations (e.g. an NDK MediaCodec
 * surface plugin that decodes H.264 into an Android Surface) need more:
 *   - a way to advertise WHICH backend they target (e.g. via an enum);
 *   - a way to expose HW-specific surfaces (Android ANativeWindow *,
 *     CVPixelBufferRef, ID3D11Texture2D*, VASurfaceID, CUarray, ...) without
 *     forcing the plugin-seam header to depend on platform SDKs;
 *   - a way to be queried by the engine for "do you support codec X on
 *     platform Y?".
 *
 * This header provides those primitives. Concrete HW plugins SHOULD inherit
 * from the appropriate abstract base class AND the matching
 * IVideo{Source,Sink,Receiver,Sender} interface (multiple inheritance) so
 * they participate in the generic plugin registry while still advertising
 * HW-specific capabilities.
 *
 * ## Registration pattern
 *
 * Out-of-tree HW plugins register themselves under a distinct id
 * (e.g. `"mediacodec"`, `"videotoolbox"`, `"nvdec"`) via
 * `core::PluginRegistry::register_video_*()`. Users select HW acceleration
 * by setting `EngineConfig::video_*_name = "mediacodec"` etc.
 *
 * ## Capability flags (R3-Batch)
 *
 * The reference interfaces already expose
 *   `is_hardware_accelerated() -> bool`
 *   `hardware_backend() -> string_view`
 * as virtual methods with `software` defaults. HW plugins override both.
 *
 * @note P1.1 (R3-Batch). Header-only, no implementation in core — vendor
 *       plugins live in separate repos / modules.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/core/error.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Backend identifier (R3-Batch)
// ---------------------------------------------------------------------------

/** Stable identifier for the HW backend a plugin targets.
 *  `Software` is the default; HW plugins MUST return one of the others. */
enum class HwBackend : std::uint16_t {
    Software         = 0,
    MediaCodec       = 1,    // Android NDK MediaCodec
    VideoToolbox     = 2,    // Apple VideoToolbox
    Nvdec            = 3,    // NVIDIA NVDEC
    Nvenc            = 4,    // NVIDIA NVENC
    Vaapi            = 5,    // VA-API (Linux Intel/AMD)
    Dxva             = 6,    // DirectX Video Acceleration (Windows)
    Qsv              = 7,    // Intel Quick Sync Video
    Amf              = 8,    // AMD AMF / VCN
    V4l2             = 9,    // V4L2 M2M (Linux)
    AvFoundation     = 10,   // Apple AVFoundation capture
    DirectShow       = 11,   // Windows DirectShow capture
    AndroidCamera2   = 12,   // Android Camera2 capture
    OpenH264         = 100,  // software but HW-tuned
    Libvpx           = 101,
    Custom           = 0xFFFE,
    Unknown          = 0xFFFF,
};

/** Map HwBackend → short string identifier (matches
 *  IVideo*::hardware_backend() convention). */
inline constexpr std::string_view hw_backend_name(HwBackend b) noexcept {
    switch (b) {
        case HwBackend::Software:        return "software";
        case HwBackend::MediaCodec:      return "mediacodec";
        case HwBackend::VideoToolbox:    return "videotoolbox";
        case HwBackend::Nvdec:           return "nvdec";
        case HwBackend::Nvenc:           return "nvenc";
        case HwBackend::Vaapi:           return "vaapi";
        case HwBackend::Dxva:            return "dxva";
        case HwBackend::Qsv:             return "qsv";
        case HwBackend::Amf:             return "amf";
        case HwBackend::V4l2:            return "v4l2";
        case HwBackend::AvFoundation:    return "avfoundation";
        case HwBackend::DirectShow:      return "directshow";
        case HwBackend::AndroidCamera2:  return "android-camera2";
        case HwBackend::OpenH264:        return "openh264";
        case HwBackend::Libvpx:          return "libvpx";
        case HwBackend::Custom:          return "custom";
        case HwBackend::Unknown:         return "unknown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// IHwVideoEncoder — abstract base for HW encoder plugins
// ---------------------------------------------------------------------------

/** Common interface for HW video encoders. Concrete HW encoders
 *  (MediaCodec encoder surface, VideoToolbox VTCompressionSession,
 *  NvEnc API, VAAPI encode, Quick Sync, AMF) inherit from BOTH this
 *  and `IVideoSender` so they participate in the generic plugin
 *  registry while advertising HW capabilities.
 *
 *  Pure abstract — implementations must:
 *    1. Override `IVideoSender` methods to packetize output.
 *    2. Override `backend()` to return the HW identifier.
 *    3. Override `target_surface_handle()` to expose the HW surface
 *       (Android ANativeWindow*, CVPixelBufferRef, VASurfaceID, ...) —
 *       callers may use it for zero-copy GPU rendering.
 */
class IHwVideoEncoder {
public:
    virtual ~IHwVideoEncoder() = default;

    /** Backend identifier. */
    virtual HwBackend backend() const noexcept = 0;

    /** Human-readable backend name (delegates to hw_backend_name()). */
    std::string_view backend_name() const noexcept {
        return hw_backend_name(backend());
    }

    /** Opaque HW surface handle, or nullptr if not applicable.
     *  Caller is responsible for casting to the platform-specific type. */
    virtual void* target_surface_handle() noexcept = 0;

    /** True iff this encoder can encode the requested codec on the
     *  current device (e.g. H.264 1080p30 may be supported but 4K60 not). */
    virtual bool supports(VideoCodecKind codec,
                          std::uint32_t  width,
                          std::uint32_t  height,
                          std::uint32_t  fps) const noexcept = 0;

    /** Latency from input frame to first emitted packet (in microseconds).
     *  HW encoders typically report 0–10 ms for H.264 baseline. */
    virtual std::uint32_t encoding_latency_us() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// IHwVideoDecoder — abstract base for HW decoder plugins
// ---------------------------------------------------------------------------

/** Common interface for HW video decoders. See IHwVideoEncoder doc for
 *  the inheritance pattern; here the partner is `IVideoReceiver`. */
class IHwVideoDecoder {
public:
    virtual ~IHwVideoDecoder() = default;

    /** Backend identifier. */
    virtual HwBackend backend() const noexcept = 0;

    std::string_view backend_name() const noexcept {
        return hw_backend_name(backend());
    }

    /** Opaque HW surface handle (output surface). */
    virtual void* output_surface_handle() noexcept = 0;

    /** True iff this decoder can decode the requested codec on the
     *  current device. */
    virtual bool supports(VideoCodecKind codec,
                          std::uint32_t  width,
                          std::uint32_t  height,
                          std::uint32_t  fps) const noexcept = 0;

    /** Latency from packet arrival to decoded frame ready (µs). */
    virtual std::uint32_t decoding_latency_us() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// IHwVideoCapture — abstract base for HW camera/screen capture plugins
// ---------------------------------------------------------------------------

/** Common interface for HW video capture. Concrete HW capture plugins
 *  (V4L2, AVFoundation, NDK Camera2, DirectShow, ScreenCapture on
 *  Win32 / macOS / X11) inherit from BOTH this and `IVideoSource`.
 */
class IHwVideoCapture {
public:
    virtual ~IHwVideoCapture() = default;

    /** Backend identifier. */
    virtual HwBackend backend() const noexcept = 0;

    std::string_view backend_name() const noexcept {
        return hw_backend_name(backend());
    }

    /** Capture capability query: does this device expose a source matching
     *  the requested spec? `device_id` may be a camera index, a window
     *  handle, or a screen index — interpretation is backend-specific. */
    virtual bool supports_device(std::string_view device_id,
                                 std::uint32_t    width,
                                 std::uint32_t    height,
                                 std::uint32_t    fps) const noexcept = 0;

    /** True iff this source can provide zero-copy GPU buffers directly
     *  consumable by a downstream encoder/renderer. */
    virtual bool zero_copy_to_gpu() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Helper: is_hardware_accelerated() static bridge
// ---------------------------------------------------------------------------

/** Convenience helper for callers that want to check both the abstract
 *  IVideo*::is_hardware_accelerated() and the type-safe HwBackend
 *  identification of a plugin instance (downcast to IHwVideoEncoder etc.).
 *
 *  Returns true iff the plugin implements IHwVideoEncoder/Decoder/Capture
 *  AND its backend() != Software.
 */
template <class PluginT, class HwInterfaceT>
inline bool is_hw_accelerated(PluginT* plugin, HwInterfaceT* /*hint*/) noexcept {
    if (!plugin) return false;
    if (!plugin->is_hardware_accelerated()) return false;
    return plugin->hardware_backend() != "software";
}

} // namespace nimrtc::plugins
