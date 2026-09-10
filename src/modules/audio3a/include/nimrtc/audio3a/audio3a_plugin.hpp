/**
 * @file nimrtc/audio3a/audio3a_plugin.hpp
 * @brief Plugin adapter: wraps a concrete audio3a::IAudio3A behind plugins::IAudio3A.
 *
 * Implements the plugin interface defined in <nimrtc/plugins/audio3a.hpp> by
 * delegating to an owned concrete implementation (NullAudio3A today;
 * WebRtcAudio3A in P1 once NIMRTC_VENDOR_WEBRTC_APM=ON lands).
 *
 * ## Adapter vs direct implementation
 *
 * ICE's IceTransport extends ITransport directly because the API shapes
 * align (both use raw BufferView pointers). The audio3a concrete API uses
 * `process_capture(Frame&)` where Frame bundles samples + channels + rate
 * + timestamp, while the plugin interface takes raw `(float*, size_t, size_t)`
 * plus has callbacks for VAD / level. A small adapter class keeps the two
 * APIs decoupled without forcing concrete modules to implement callbacks
 * they don't natively support.
 *
 * ## Registration
 *
 * `NullPluginFactory` is registered under id "webrtc" (matches the
 * EngineConfig default `audio3a_name = "webrtc"`). When WebRtcAudio3A ships,
 * a separate factory should register as "webrtc_apm" or similar; users then
 * pick by setting `EngineConfig::audio3a_name` accordingly.
 *
 * ## Important: explicit-registration required (MSVC quirk)
 *
 * MSVC's static linker strips anonymous-namespace `static` global initializers
 * from a .lib when no symbol from that .obj is ODR-used by the consumer .obj.
 * The audio3a_plugin.cpp registrar (`Audio3APluginRegistrar`) would otherwise
 * never run when statically linked. To avoid this, callers must explicitly invoke
 * `nimrtc::audio3a::register_default_plugins()` once at startup before any
 * `core::PluginRegistry::instance()` lookup of audio3a factories.
 *
 * The inline `register_default_plugins()` body references a function in
 * audio3a_plugin.cpp, which forces that .obj to be linked, which in turn
 * runs the registrar's constructor. This is documented in ARCHITECTURE.md
 * (future revision) and in `docs/zh/NimRTC-V2-技术文档.md` §2.x (TBD).
 *
 * Alternative path (FUTURE — P1.x): engine refactor migrates to a single
 * top-level `nimrtc::core::register_all_default_plugins()` that walks every
 * module; the per-module explicit call is then redundant.
 *
 * @note P1.1 (R2). Adapter only — engine refactor (drop direct concrete
 *       includes) is a separate step.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

#include <nimrtc/audio3a/audio3a.hpp>      // concrete audio3a::IAudio3A / NullAudio3A / Config
#include <nimrtc/plugins/audio3a.hpp>      // plugins::IAudio3A / Audio3AConfig / Stats

namespace nimrtc::audio3a {

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

/**
 * @brief Wraps a concrete `audio3a::IAudio3A` so it can be exposed via the
 *        `plugins::IAudio3A` interface and looked up by string ID through
 *        `core::PluginRegistry`.
 *
 * Ownership: takes ownership of the concrete impl pointer; deletes it on
 * destruction. The concrete impl is constructed lazily on first process_* call.
 *
 * Thread safety of the concrete impl is the same as audio3a::IAudio3A:
 * caller must serialize process_capture / process_render calls.
 */
class PluginAdapter : public plugins::IAudio3A {
public:
    /** Takes ownership of the concrete impl (may be nullptr → creates NullAudio3A). */
    explicit PluginAdapter(audio3a::IAudio3A* impl);
    ~PluginAdapter() override;

    PluginAdapter(const PluginAdapter&)            = delete;
    PluginAdapter& operator=(const PluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void          close() noexcept override;

    // ---- plugins::IAudio3A -------------------------------------------------

    void set_callbacks(plugins::VadCallback        on_vad,
                      plugins::LevelCallback        on_level,
                      plugins::AudioErrorCallback  on_error) noexcept override;

    /** Adapts the raw-ptr plugin API to the Frame-based concrete API:
     *   - construct a stack Frame wrapping the same buffer
     *   - call impl_->process_capture(frame)
     *   - on return, read impl_->stats() and fire level / VAD callbacks */
    plugins::Status process_capture(float*        samples,
                                   std::size_t   num_samples,
                                   std::size_t   num_channels) noexcept override;

    /** Mirror of process_capture() for the loudspeaker reference path. */
    plugins::Status process_render(const float* samples,
                                  std::size_t   num_samples,
                                  std::size_t   num_channels) noexcept override;

    void set_render_delivered(std::size_t num_samples) noexcept override;
    void request_vad_report() noexcept override;
    void request_level_report() noexcept override;

    plugins::IAudio3A::Stats stats() const noexcept override;

    // -------------------------------------------------------------------------
    // PCM Taps (pre/post 3A, §8.7)
    // -------------------------------------------------------------------------

    /** @see plugins::IAudio3A::set_pre_process_tap */
    void set_pre_process_tap(plugins::PcmTapCallback tap) noexcept override;

    /** @see plugins::IAudio3A::set_post_process_tap */
    void set_post_process_tap(plugins::PcmTapCallback    tap,
                             plugins::PcmTapCallbackI16 tap_i16) noexcept override;

private:
    /** Concrete audio3a impl (audio3a::IAudio3A, NOT the plugin interface). */
    std::unique_ptr<audio3a::IAudio3A> concrete_;

    /** Plugin-side callbacks. */
    plugins::VadCallback       on_vad_;
    plugins::LevelCallback     on_level_;
    plugins::AudioErrorCallback on_error_;

    /** PCM Tap callbacks (§8.7).  Fired from invoke_pre/post_tap.
     *  post_tap_i16_ is the int16 variant (ASR/LLM consumers prefer PCM16).
     *
     *  The IAudio3A spec calls out that set_pre/post_process_tap() may be
     *  called concurrently with process_capture(); see
     *  <nimrtc/plugins/audio3a.hpp>.  Without synchronization a tap swap
     *  mid-frame would race the in-flight process_capture call.  We guard
     *  the tap setters and tap invocation with `tap_mu_` (a dedicated
     *  mutex; not the audio path mutex to keep the critical section in
     *  process_capture as small as possible). */
    mutable std::mutex         tap_mu_;
    plugins::PcmTapCallback    pre_tap_;
    plugins::PcmTapCallback    post_tap_;
    plugins::PcmTapCallbackI16 post_tap_i16_;

    /** Monotonic frame timestamp for tap callbacks. Advanced by
     *  invoke_pre_tap() to mark the frame boundary; post_tap() uses the
     *  same value (single tick per process_capture call). */
    std::int64_t                tap_timestamp_us_ = 0;

    /** Reusable scratch buffer for the int16_t post-tap (§8.7).  Avoids
     *  heap allocation on every process_capture() call (audio path runs
     *  at 50–100 Hz; per-call allocation causes latency jitter and memory
     *  fragmentation).  Capacity grows monotonically — first call after
     *  install allocates; subsequent calls within the same capacity reuse
     *  the storage. */
    std::vector<std::int16_t>  int16_tap_buf_;

    /** Concrete-side config, populated on first process_* call if open()
     *  was called with no explicit config (lazy init). */
    audio3a::Config           concrete_config_{};
    bool                       configured_ = false;

    /** Tracks render delivery so AEC delay can be reported to concrete impl. */
    std::size_t               render_delivered_samples_ = 0;

    /** After every process_capture, read concrete stats and fire callbacks. */
    void maybe_fire_callbacks() noexcept;

    /** Lazy init: apply concrete_config_ to the concrete impl if not yet done. */
    plugins::Status ensure_configured() noexcept;

    /** Fire pre-/post-3A PCM taps (§8.7).  Called from process_capture. */
    void invoke_pre_tap(float* samples, std::size_t num_samples,
                         std::size_t num_channels) noexcept;
    void invoke_post_tap(float* samples, std::size_t num_samples,
                          std::size_t num_channels) noexcept;
};


// ---------------------------------------------------------------------------
// NullPluginFactory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing PluginAdapter instances that wrap a NullAudio3A.
 *
 * Registered as id "webrtc" (the EngineConfig default). Until WebRtcAudio3A
 * lands, "webrtc" really means "WebRTC-style 3A pipeline (currently stub)".
 */
class NullPluginFactory : public plugins::IAudio3AFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IAudio3A* create()   const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point (MSVC static-link workaround)
// ---------------------------------------------------------------------------

namespace detail {
/** Defined in audio3a_plugin.cpp. Forces .obj linkage on consumer call. */
void do_register_default_plugins() noexcept;
} // namespace detail

/**
 * @brief Register all built-in audio3a plugins with core::PluginRegistry.
 *
 * MUST be called once at program startup before any
 * `core::PluginRegistry::instance().get_audio3a(...)` lookup. Idempotent
 * (Meyer's-singleton latch inside the .cpp).
 *
 * Typical placement: main() entry, or any first call into the audio3a plugin
 * path.
 *
 * @note Not `inline` because the static-local latch would otherwise be
 *       emitted as a weak external symbol that the static lib doesn't
 *       carry; non-inline ensures the symbol is in `nimrtc_audio3a.lib`.
 */
void register_default_plugins() noexcept;

} // namespace nimrtc::audio3a
