/**
 * @file nimrtc/plugins/hw_video_backend.hpp
 * @brief Backend selector for video encoders/decoders — platform-aware,
 *        hardware-first, with automatic fallback.
 *
 * Design follows the Sunshine approach (`platf::Encoder` / `platf::EncoderFactory`)
 * with the addition of a NimRTC-idiomatic plugin-seam wrapper:
 *
 *   - Every backend is described by a single `Backend` struct that exposes:
 *       * id            — short name used in logs / EngineConfig
 *       * available()   — runtime capability probe (driver loaded? HW present?)
 *       * priority()    — higher value wins when multiple backends are usable
 *       * create()      — factory function (returns plugins::IVideoCodec*)
 *       * backend_kind  — HwBackend enum (for IVideo*::hardware_backend())
 *
 *   - The HwBackendSelector owns a list of backends, sorted by priority.
 *
 *   - Selection policy:
 *       1. If `preferred_id` is set and matches an available backend → use it.
 *       2. Otherwise, walk priority-sorted list, pick first `available()` one.
 *       3. If nothing is available, return the stub backend (always present).
 *
 *   - This lets the engine pick "NVENC > AMF > QSV > VAAPI > VT > software"
 *     automatically, while still allowing overrides from `EngineConfig`.
 *
 * Per ARCHITECTURE.md §6 the codec layer is plugin-only — this header lives
 * in plugins/ because backend selection is a plugin-seam concern, not a
 * core concern. Modules register their backends at static-init time.
 *
 * @note Sibling to hw_seam.hpp (which is about HW *capability advertisement*);
 *       this file is about HW *backend selection and dispatch*.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/hw_seam.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Backend descriptor — a single (id, available, priority, create, kind) entry
// ---------------------------------------------------------------------------

/**
 * @brief Description of one hardware (or software) backend.
 *
 * Host backends (NVENC / AMF / VAAPI / VideoToolbox / MediaCodec / QSV /
 * DXVA / libx264 / libx265 / OpenH264 / FFmpeg / stub) each populate one
 * such struct at static-init time; the selector picks the highest-priority
 * one whose `available()` returns true.
 *
 * `available()` is called once per engine-startup (NOT per encode) because
 * device enumeration can be expensive on some platforms (D3D11 device
 * creation, CUDA device probe, VideoToolbox session creation).
 */
struct VideoEncoderBackend {
    /// Short stable id used in logs and `EngineConfig::video_codec_name`.
    std::string_view id;

    /// Longer human-readable description (e.g. "NVIDIA NVENC H.264").
    std::string_view description;

    /// HwBackend kind — propagates into the created codec's
    /// `IVideoCodec::hardware_backend()`.
    HwBackend backend;

    /// Higher = preferred. Hardware backends should be > 100; software
    /// fallback backends should be < 50; the built-in stub is 0.
    int priority = 0;

    /// Selected codec family — selector matches this against cfg.codec.
    VideoCodecKind codec_kind = VideoCodecKind::kH264;

    /// True iff this backend is hardware-accelerated (sets
    /// `IVideoCodec::is_hardware_accelerated()` on the created instance).
    bool is_hw = false;

    /// Runtime probe — should NOT instantiate heavy resources; just
    /// check whether the HW/driver/library is reachable on this device.
    std::function<bool(const VideoCodecConfig&)> available;

    /// Factory function — must return a fully-constructed (but not yet
    /// `open()`-ed) IVideoCodec instance. Selector owns lifetime.
    std::function<std::unique_ptr<IVideoCodec>(VideoCodecConfig)> create;

    // ---- Zero-copy capability (R3-batch) ----------------------------------
    //
    // When the backend supports the GPU-first path, it sets `pool_factory`
    // and optionally `zero_copy_supported = true`. The engine probes the
    // backend's zero-copy capability once at startup; codecs that don't
    // expose `pool_factory` are forced onto the CPU-staging path.

    /// True if this backend can encode / decode without CPU staging.
    /// (For most HW backends this is always true; software backends set
    ///  false even though they technically "could" copy into their
    ///  internal buffers.)
    bool zero_copy_supported = false;

    /// Optional factory for the GPU surface pool this backend uses. The
    /// engine uses the pool to:
    ///   - decoder input: pool.acquire(w,h,format) → GpuBuffer → decode into
    ///   - encoder input: pool.acquire(w,h,format) → GpuBuffer → fill pixels
    ///                   from capture source → encode
    ///   - SFU forward:   pool.acquire() to make a writable copy that is
    ///                   then drained as the next peer's surface (avoiding
    ///                   reads while the source is still in use)
    /// When null, the codec operates in CPU-only mode (no pool).
    std::function<std::shared_ptr<GpuBufferPool>(VideoCodecConfig)> pool_factory;
};

/**
 * @brief Same as VideoEncoderBackend but for decoders. (Currently the same
 *        IVideoCodec interface covers both directions, so we share `create`,
 *        but the selector maintains two separate tables for clarity.)
 */
struct VideoDecoderBackend {
    std::string_view id;
    std::string_view description;
    HwBackend backend;
    int priority = 0;
    VideoCodecKind codec_kind = VideoCodecKind::kH264;
    bool is_hw = false;

    /// Mirrors `VideoEncoderBackend::zero_copy_supported`. Decoders that
    /// can output a GPU surface (DXVA, NVDEC, VAAPI, VideoToolbox, …)
    /// set this to true so the engine can wire a zero-copy
    /// `decoded_callback` to the display sink. Software decoders set it
    /// to false (the engine falls back to `decode()` + CPU staging).
    bool zero_copy_supported = false;

    /// Mirrors `VideoEncoderBackend::pool_factory`. Decoder-side pools
    /// are used to acquire the output surface that the decoder fills.
    std::function<std::shared_ptr<GpuBufferPool>(VideoCodecConfig)> pool_factory;

    std::function<bool(const VideoCodecConfig&)> available;
    std::function<std::unique_ptr<IVideoCodec>(VideoCodecConfig)> create;
};

// ---------------------------------------------------------------------------
// Selector — ordered list of backends + selection policy
// ---------------------------------------------------------------------------

/**
 * @brief Per-codec-kind backend registry.
 *
 * Thread-safety: registration is one-shot at static-init time. Selection
 * (`select_encoder` / `select_decoder`) can be called from multiple threads
 * concurrently; it only reads + sorts the registered list.
 *
 * Typical usage:
 *
 * @code
 *   // 1. Backend registers itself at static-init time:
 *   namespace {
 *     const bool kNvencRegistered = []{
 *       HwVideoBackendRegistry::instance().register_encoder(
 *           VideoEncoderBackend{ .id = "nvenc", .priority = 400, ... });
 *       return true;
 *     }();
 *   }
 *
 *   // 2. Engine calls at startup:
 *   auto* be = HwVideoBackendRegistry::instance().select_encoder(
 *       cfg, cfg.video_codec_name);
 *   auto codec = be->create(cfg);
 * @endcode
 */
class HwVideoBackendRegistry {
public:
    static HwVideoBackendRegistry& instance() noexcept;

    /// Register one encoder backend. Backend must outlive the registry
    /// (typically a static struct literal). NOT thread-safe; call from
    /// static initialisation only.
    void register_encoder(VideoEncoderBackend backend) noexcept;

    /// Register one decoder backend. Same constraints as register_encoder.
    void register_decoder(VideoDecoderBackend backend) noexcept;

    /// Register one pair (enc + dec share the same id). Convenience for
    /// HW backends where the same library provides both directions.
    void register_pair(VideoEncoderBackend enc, VideoDecoderBackend dec) noexcept;

    /// Select an encoder matching the codec kind and (optionally) preferred id.
    /// Returns nullptr ONLY if no backends are registered at all — a built-in
    /// "stub" backend is always present as ultimate fallback.
    const VideoEncoderBackend* select_encoder(VideoCodecKind kind,
                                              std::string_view preferred_id
                                                  = std::string_view{}) const noexcept;

    /// Same semantics for decoders.
    const VideoDecoderBackend* select_decoder(VideoCodecKind kind,
                                              std::string_view preferred_id
                                                  = std::string_view{}) const noexcept;

    /// Snapshot of all registered encoder backends, sorted by priority desc.
    std::vector<const VideoEncoderBackend*> list_encoders(
        std::optional<VideoCodecKind> kind_filter = std::nullopt) const noexcept;

    /// Snapshot of all registered decoder backends.
    std::vector<const VideoDecoderBackend*> list_decoders(
        std::optional<VideoCodecKind> kind_filter = std::nullopt) const noexcept;

    /// Force-clear all registered backends. Test-only.
    void clear_for_tests() noexcept;

private:
    HwVideoBackendRegistry() = default;
    HwVideoBackendRegistry(const HwVideoBackendRegistry&) = delete;
    HwVideoBackendRegistry& operator=(const HwVideoBackendRegistry&) = delete;

    mutable std::mutex mutex_;
    std::vector<VideoEncoderBackend> encoders_;
    std::vector<VideoDecoderBackend> decoders_;

    // Built-in stub — always available so `select_*` is never null.
    void install_stub_backends() noexcept;
};

// ---------------------------------------------------------------------------
// Auto-registration helper
// ---------------------------------------------------------------------------

/**
 * @brief Macro for backends to self-register at static-init time without
 *        repeating the boilerplate.
 *
 * Usage (inside a .cpp file):
 * @code
 *   NIMRTC_REGISTER_VIDEO_ENCODER_BACKEND(
 *       VideoEncoderBackend{
 *           .id = "nvenc",
 *           .description = "NVIDIA NVENC H.264 encoder",
 *           .backend = HwBackend::Nvenc,
 *           .priority = 400,
 *           .codec_kind = VideoCodecKind::kH264,
 *           .is_hw = true,
 *           .available = [](const VideoCodecConfig& c){ return nvenc::probe(c); },
 *           .create    = [](VideoCodecConfig c){ return std::make_unique<NvencEncoder>(c); }
 *       });
 * @endcode
 *
 * The macro creates a file-scope static object whose constructor registers
 * the backend before main() runs.
 */
#define NIMRTC_REGISTER_VIDEO_ENCODER_BACKEND(be_literal) \
    namespace { \
        struct NimrtcEncoderRegistrar_##__LINE__ { \
            NimrtcEncoderRegistrar_##__LINE__() noexcept { \
                ::nimrtc::plugins::HwVideoBackendRegistry::instance() \
                    .register_encoder(be_literal); \
            } \
        }; \
        static const NimrtcEncoderRegistrar_##__LINE__ \
            nimrtc_encoder_registrar_##__LINE__{}; \
    }

#define NIMRTC_REGISTER_VIDEO_DECODER_BACKEND(be_literal) \
    namespace { \
        struct NimrtcDecoderRegistrar_##__LINE__ { \
            NimrtcDecoderRegistrar_##__LINE__() noexcept { \
                ::nimrtc::plugins::HwVideoBackendRegistry::instance() \
                    .register_decoder(be_literal); \
            } \
        }; \
        static const NimrtcDecoderRegistrar_##__LINE__ \
            nimrtc_decoder_registrar_##__LINE__{}; \
    }

} // namespace nimrtc::plugins
