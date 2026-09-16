/**
 * @file src/modules/audio3a/src/audio3a_plugin.cpp
 * @brief PluginAdapter + NullPluginFactory / WebRtcPluginFactory implementation.
 *
 * Per ADR-001:
 *   "Concrete implementations (built-in 'standard' plugins) live in their
 *    respective src/modules/. Those modules link nimrtc_plugins PUBLIC so
 *    users who link any module transitively get the interface headers."
 *
 * This file registers two built-in audio3a factories with core::PluginRegistry:
 *   - "webrtc":     NullPluginFactory  → NullAudio3A (passthrough stub)
 *   - "webrtc_apm": WebRtcPluginFactory → WebRtcAudio3A (real WebRTC APM)
 *
 * The engine default `EngineConfig::audio3a_name = "webrtc_apm"` resolves to
 * the real WebRTC APM-backed implementation. To revert to the stub pass
 * `engine.cfg.audio3a_name = "webrtc"` instead.
 *
 * @note P1.1 (R2). Engine-side wiring (replace direct `audio3a::NullAudio3A`
 *       instantiation with registry lookup) is a separate step.
 */

#include <nimrtc/audio3a/audio3a_plugin.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/plugin_id.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::audio3a {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/** Translate plugins::Audio3AConfig → audio3a::Config (concrete side).
 *  Field rename: aec_delay_est_ms → aec_delay_headroom_ms. */
[[maybe_unused]]
audio3a::Config to_concrete_config(const plugins::Audio3AConfig& p) noexcept {
    audio3a::Config c;
    c.sample_rate_hz          = p.sample_rate_hz;
    c.capture_channels        = p.capture_channels;
    c.render_channels         = p.render_channels;
    c.enable_aec              = p.enable_aec;
    c.aec_delay_headroom_ms   = p.aec_delay_est_ms;
    c.ans_level               = p.ans_level;
    c.enable_agc              = p.enable_agc;
    c.agc_target_dbfs         = p.agc_target_dbfs;
    c.enable_vad              = p.enable_vad;
    c.report_tx_level         = p.report_tx_level;
    return c;
}

/** Translate concrete LevelStats → plugin IAudio3A::Stats. */
plugins::IAudio3A::Stats to_plugin_stats(const LevelStats& s) noexcept {
    plugins::IAudio3A::Stats out;
    out.last_capture_level_dbfs = s.capture_level_dbfs;
    out.last_render_level_dbfs   = s.render_level_dbfs;
    out.vad_active              = s.vad_active;
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

PluginAdapter::PluginAdapter(audio3a::IAudio3A* impl)
    : concrete_(impl ? impl : new NullAudio3A())
    , pre_tap_(nullptr)
    , post_tap_(nullptr)
    , post_tap_i16_(nullptr)
    , tap_timestamp_us_(0) {
    core::log::Logger::instance().debug(
        "audio3a::PluginAdapter created (concrete impl owned)");
}

PluginAdapter::~PluginAdapter() {
    // concrete_ destruction handles the impl cleanup.
}

const char* PluginAdapter::name() const noexcept {
    return "nimrtc::audio3a::PluginAdapter (concrete 3A via plugin interface)";
}

plugins::Status PluginAdapter::open() noexcept {
    if (!concrete_) {
        if (on_error_) on_error_(plugins::kErrInternal, "audio3a plugin: null impl");
        return plugins::kErrInternal;
    }
    if (!configured_) {
        if (!concrete_->init(concrete_config_)) {
            if (on_error_) on_error_(plugins::kErrInternal,
                                     "audio3a plugin: concrete init failed");
            return plugins::kErrInternal;
        }
        configured_ = true;
    }
    return plugins::kOk;
}

void PluginAdapter::close() noexcept {
    if (concrete_) {
        concrete_->reset();
    }
    configured_ = false;
    render_delivered_samples_ = 0;
}

void PluginAdapter::set_callbacks(plugins::VadCallback        on_vad,
                                  plugins::LevelCallback        on_level,
                                  plugins::AudioErrorCallback  on_error) noexcept {
    on_vad_    = std::move(on_vad);
    on_level_  = std::move(on_level);
    on_error_  = std::move(on_error);
}

plugins::Status PluginAdapter::ensure_configured() noexcept {
    if (configured_) return plugins::kOk;
    if (!concrete_) return plugins::kErrInternal;

    // Lazy init: if concrete_config_ is still zero/empty, apply sensible
    // defaults matching NullAudio3A's contract (48 kHz, 1 ch).
    if (concrete_config_.sample_rate_hz == 0) concrete_config_.sample_rate_hz = 48000;
    if (concrete_config_.capture_channels == 0) concrete_config_.capture_channels = 1;
    if (concrete_config_.render_channels == 0)  concrete_config_.render_channels  = 1;

    if (!concrete_->init(concrete_config_)) {
        if (on_error_) on_error_(plugins::kErrInternal,
                                 "audio3a plugin: concrete init failed");
        return plugins::kErrInternal;
    }
    configured_ = true;
    return plugins::kOk;
}

plugins::Status PluginAdapter::process_capture(float*       samples,
                                               std::size_t  num_samples,
                                               std::size_t  num_channels) noexcept {
    if (!concrete_) return plugins::kErrInternal;
    if (!samples || num_samples == 0 || num_channels == 0) {
        return plugins::kErrInvalidParam;
    }
    plugins::Status rc = ensure_configured();
    if (rc != plugins::kOk) return rc;

    // ── Pre-tap: raw mic PCM before 3A (for wake-word / monitoring) ──
    invoke_pre_tap(samples, num_samples, num_channels);

    audio3a::Frame frame;
    frame.samples        = samples;
    frame.num_samples    = num_samples;
    frame.num_channels   = num_channels;
    frame.sample_rate_hz = concrete_config_.sample_rate_hz;
    concrete_->process_capture(frame);

    // ── Post-tap: 3A-cleaned PCM (for ASR consumption) ──
    invoke_post_tap(samples, num_samples, num_channels);

    maybe_fire_callbacks();
    return plugins::kOk;
}

plugins::Status PluginAdapter::process_render(const float* samples,
                                              std::size_t  num_samples,
                                              std::size_t  num_channels) noexcept {
    if (!concrete_) return plugins::kErrInternal;
    if (!samples || num_samples == 0 || num_channels == 0) {
        return plugins::kErrInvalidParam;
    }
    plugins::Status rc = ensure_configured();
    if (rc != plugins::kOk) return rc;

    audio3a::Frame frame;
    // concrete::process_render takes non-const Frame ( Frame& not const Frame& ).
    frame.samples        = const_cast<float*>(samples);
    frame.num_samples    = num_samples;
    frame.num_channels   = num_channels;
    frame.sample_rate_hz = concrete_config_.sample_rate_hz;
    concrete_->process_render(frame);
    return plugins::kOk;
}

void PluginAdapter::set_render_delivered(std::size_t num_samples) noexcept {
    render_delivered_samples_ += num_samples;
    if (concrete_) {
        concrete_->on_render_delivered(render_delivered_samples_);
    }
}

void PluginAdapter::request_vad_report() noexcept {
    if (concrete_) concrete_->request_vad_report();
}

void PluginAdapter::request_level_report() noexcept {
    if (concrete_) concrete_->request_level_report();
}

plugins::IAudio3A::Stats PluginAdapter::stats() const noexcept {
    if (!concrete_) return {};
    return to_plugin_stats(concrete_->stats());
}

void PluginAdapter::maybe_fire_callbacks() noexcept {
    if (!concrete_) return;
    const LevelStats s = concrete_->stats();
    if (on_vad_)   on_vad_(s.vad_active);
    if (on_level_) on_level_(s.capture_level_dbfs);
}

// -------------------------------------------------------------------------
// PCM Taps (pre/post 3A, §8.7)
// -------------------------------------------------------------------------

void PluginAdapter::invoke_pre_tap(float* samples, std::size_t num_samples,
                                   std::size_t num_channels) noexcept {
    plugins::PcmTapCallback cb;
    {
        std::lock_guard<std::mutex> lk(tap_mu_);
        cb = pre_tap_;
    }
    if (!cb) return;
    plugins::PcmFrameMetadata meta{};
    meta.sample_rate_hz = concrete_config_.sample_rate_hz ? concrete_config_.sample_rate_hz : 48000;
    meta.num_samples     = num_samples;
    meta.num_channels   = static_cast<std::uint8_t>(num_channels);
    meta.timestamp_us    = tap_timestamp_us_;
    tap_timestamp_us_ += static_cast<std::int64_t>(num_samples * 1000000ULL / meta.sample_rate_hz);
    cb(samples, meta);
}

void PluginAdapter::invoke_post_tap(float* samples, std::size_t num_samples,
                                    std::size_t num_channels) noexcept {
    plugins::PcmTapCallback    cb_f32;
    plugins::PcmTapCallbackI16 cb_i16;
    {
        std::lock_guard<std::mutex> lk(tap_mu_);
        cb_f32 = post_tap_;
        cb_i16 = post_tap_i16_;
    }
    if (!cb_f32 && !cb_i16) return;
    plugins::PcmFrameMetadata meta{};
    meta.sample_rate_hz = concrete_config_.sample_rate_hz ? concrete_config_.sample_rate_hz : 48000;
    meta.num_samples     = num_samples;
    meta.num_channels   = static_cast<std::uint8_t>(num_channels);
    meta.timestamp_us    = tap_timestamp_us_;
    // Note: tap_timestamp_us_ is NOT advanced here — the frame boundary
    // is the same as the pre-tap; only one tick per process_capture call.

    if (cb_f32) {
        cb_f32(samples, meta);
    }
    if (cb_i16) {
        // Convert float → int16_t inline for ASR consumers.
        //
        // Reuse int16_tap_buf_ (PluginAdapter member) to avoid heap
        // allocation on every process_capture() call.  Capacity grows
        // monotonically — first call after install allocates; subsequent
        // calls within the same capacity reuse the storage.  std::vector
        // guarantees no reallocation while size() <= capacity() and the
        // capacity itself doesn't shrink on resize() to a smaller value.
        const std::size_t needed = num_samples * num_channels;
        if (int16_tap_buf_.size() < needed) {
            // resize (not reserve) so size() == needed and data() returns
            // exactly N valid int16_t values below.  When the first frame
            // is the largest typical frame (e.g. 480 samples @ 48 kHz mono
            // = 20 ms), subsequent frames within capacity reuse storage
            // and incur only the per-sample conversion loop below.
            int16_tap_buf_.resize(needed);
        }
        std::int16_t* out = int16_tap_buf_.data();
        for (std::size_t i = 0; i < needed; ++i) {
            float v = samples[i] * 32767.0f;
            v = std::max(-32768.0f, std::min(32767.0f, v));
            out[i] = static_cast<std::int16_t>(v);
        }
        cb_i16(out, meta);
    }
}

void PluginAdapter::set_pre_process_tap(plugins::PcmTapCallback tap) noexcept {
    std::lock_guard<std::mutex> lk(tap_mu_);
    pre_tap_ = std::move(tap);
}

void PluginAdapter::set_post_process_tap(plugins::PcmTapCallback    tap,
                                        plugins::PcmTapCallbackI16 tap_i16) noexcept {
    std::lock_guard<std::mutex> lk(tap_mu_);
    post_tap_   = std::move(tap);
    post_tap_i16_ = std::move(tap_i16);
}

// ---------------------------------------------------------------------------
// NullPluginFactory
// ---------------------------------------------------------------------------

std::string_view NullPluginFactory::id() const noexcept {
    return NIMRTC_PLUGIN_ID("webrtc");   // retained for back-compat; EngineConfig default switched to "webrtc_apm"
}

std::string_view NullPluginFactory::display_name() const noexcept {
    return "Audio 3A — Null stub (Passthrough; back-compat alias for EngineConfig.audio3a_name=\"webrtc\")";
}

plugins::IAudio3A* NullPluginFactory::create() const {
    return new PluginAdapter(new NullAudio3A());
}

// ---------------------------------------------------------------------------
// WebRtcPluginFactory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing PluginAdapter instances that wrap a WebRtcAudio3A.
 *
 * Registered as id "webrtc_apm". Requires NIMRTC_VENDORED_WEBRTC_APM=ON.
 * Falls back to NullAudio3A if WebRTC APM source is not populated.
 */

std::string_view WebRtcPluginFactory::id() const noexcept {
    return NIMRTC_PLUGIN_ID("webrtc_apm");
}

std::string_view WebRtcPluginFactory::display_name() const noexcept {
    return "Audio 3A — WebRTC APM (AEC/ANS/AGC2/VAD/HPF) — default since 0.9.0+";
}

plugins::IAudio3A* WebRtcPluginFactory::create() const {
    // Try to create WebRtcAudio3A; fall back to NullAudio3A if APM unavailable
    auto webrtc_impl = audio3a::create_webrtc_audio3a();
    // Check if WebRTC APM is actually available by trying to init
    audio3a::Config default_config{};
    if (!webrtc_impl->init(default_config)) {
        // APM not available, use null impl
        core::log::Logger::instance().warn(
            "WebRtcPluginFactory: WebRTC APM not available, falling back to NullAudio3A");
        return new PluginAdapter(new NullAudio3A());
    }
    return new PluginAdapter(webrtc_impl.release());
}

// ---------------------------------------------------------------------------
// Public registration entry point — replaces the anonymous-namespace static
// registrar that MSVC strips from static libraries (MSVC linker only pulls
// .objs from a .lib when symbols are ODR-used; anonymous-namespace globals
// are not ODR-used, so the registrar constructor never runs).
//
// The header declares `inline void register_default_plugins()` whose body
// ODR-uses `detail::do_register_default_plugins()` (defined here). When any
// TU calls register_default_plugins(), the linker pulls in this .obj, which
// runs the static-initializer of `s_registrar` exactly once.
//
// This is the same MSVC quirk that affects the ICE factory; when R3 ships a
// unified `core::register_all_default_plugins()` entry point, this per-module
// explicit call becomes redundant.
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    // static ⇒ address stable for process lifetime; runs once at first call.
    static const struct Registrar {
        Registrar() {
            static nimrtc::audio3a::NullPluginFactory s_null_factory{};
            static nimrtc::audio3a::WebRtcPluginFactory s_webrtc_factory{};
            auto& registry = nimrtc::core::PluginRegistry::instance();
            registry.register_audio3a(std::string_view{s_null_factory.id()}, &s_null_factory);
            registry.register_audio3a(std::string_view{s_webrtc_factory.id()}, &s_webrtc_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in audio3a_plugin.hpp) so the symbol is guaranteed
// in nimrtc_audio3a.lib for consumers that link via static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::audio3a
