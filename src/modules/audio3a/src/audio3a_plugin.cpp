/**
 * @file src/modules/audio3a/src/audio3a_plugin.cpp
 * @brief PluginAdapter + NullPluginFactory implementation.
 *
 * Per ADR-001:
 *   "Concrete implementations (built-in 'standard' plugins) live in their
 *    respective src/modules/. Those modules link nimrtc_plugins PUBLIC so
 *    users who link any module transitively get the interface headers."
 *
 * This file registers NullPluginFactory with core::PluginRegistry under the
 * id "webrtc" so EngineConfig::audio3a_name = "webrtc" (default) resolves to
 * a PluginAdapter wrapping NullAudio3A.
 *
 * @note P1.1 (R2). Engine-side wiring (replace direct `audio3a::NullAudio3A`
 *       instantiation with registry lookup) is a separate step.
 */

#include <nimrtc/audio3a/audio3a_plugin.hpp>

#include <cstdint>
#include <cstring>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::audio3a {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/** Translate plugins::Audio3AConfig → audio3a::Config (concrete side).
 *  Field rename: aec_delay_est_ms → aec_delay_headroom_ms. */
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
    : concrete_(impl ? impl : new NullAudio3A()) {
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

    audio3a::Frame frame;
    frame.samples        = samples;
    frame.num_samples    = num_samples;
    frame.num_channels   = num_channels;
    frame.sample_rate_hz = concrete_config_.sample_rate_hz;
    concrete_->process_capture(frame);

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

// ---------------------------------------------------------------------------
// NullPluginFactory
// ---------------------------------------------------------------------------

std::string_view NullPluginFactory::id() const noexcept {
    return "webrtc";   // matches EngineConfig::audio3a_name default
}

std::string_view NullPluginFactory::display_name() const noexcept {
    return "Audio 3A — WebRTC APM (stub: NullAudio3A until NIMRTC_VENDOR_WEBRTC_APM=ON)";
}

plugins::IAudio3A* NullPluginFactory::create() const {
    return new PluginAdapter(new NullAudio3A());
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
            static nimrtc::audio3a::NullPluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_audio3a(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

} // namespace nimrtc::audio3a
