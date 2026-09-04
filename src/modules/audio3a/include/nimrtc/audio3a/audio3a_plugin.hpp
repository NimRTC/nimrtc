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

private:
    /** Concrete audio3a impl (audio3a::IAudio3A, NOT the plugin interface). */
    std::unique_ptr<audio3a::IAudio3A> concrete_;

    /** Plugin-side callbacks. */
    plugins::VadCallback       on_vad_;
    plugins::LevelCallback     on_level_;
    plugins::AudioErrorCallback on_error_;

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
/** Defined in audio3a_plugin.cpp. Calling this ODR-uses the symbol, which
 *  forces the .obj (and its static Audio3APluginRegistrar) to be linked
 *  into any consumer that calls register_default_plugins(). */
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
 */
inline void register_default_plugins() noexcept {
    // Latch via static-init in the body. The reference to
    // detail::do_register_default_plugins() ensures the .obj containing
    // the actual registration work is pulled into the link.
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::audio3a
