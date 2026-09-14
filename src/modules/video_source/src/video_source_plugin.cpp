/**
 * @file src/modules/video_source/src/video_source_plugin.cpp
 * @brief PluginAdapter + MemoryPluginFactory implementation.
 *
 * Per ADR-001: concrete implementations live in their respective modules.
 * This file registers MemoryPluginFactory with core::PluginRegistry under
 * the id "memory" so EngineConfig::video_source_name = "memory" (default)
 * resolves to a PluginAdapter wrapping MemoryVideoSource.
 *
 * @note P1 (R2-Batch2).
 */

#include <nimrtc/video_source/video_source_plugin.hpp>

#include <cstdint>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::video_source {

// ---------------------------------------------------------------------------
// Translation helpers (anonymous namespace — file-local)
// ---------------------------------------------------------------------------

namespace {

/** Translate plugin-level `plugins::VideoPixelFormat` → concrete
 *  `video_frame::PixelFormat`. */
video_frame::PixelFormat to_concrete_format(plugins::VideoPixelFormat f) noexcept {
    switch (f) {
        case plugins::VideoPixelFormat::kI420:
            return video_frame::PixelFormat::kI420;
        case plugins::VideoPixelFormat::kNV12:
            return video_frame::PixelFormat::kNV12;
        case plugins::VideoPixelFormat::kYV12:
            return video_frame::PixelFormat::kYV12;
        case plugins::VideoPixelFormat::kBGRA:
            return video_frame::PixelFormat::kBGRA;
        case plugins::VideoPixelFormat::kRGBA:
            return video_frame::PixelFormat::kRGBA;
        default:
            return video_frame::PixelFormat::kI420;
    }
}

/** Translate concrete `video_frame::PixelFormat` → plugin-level. */
plugins::VideoPixelFormat to_plugin_format(video_frame::PixelFormat f) noexcept {
    switch (f) {
        case video_frame::PixelFormat::kI420:
            return plugins::VideoPixelFormat::kI420;
        case video_frame::PixelFormat::kNV12:
            return plugins::VideoPixelFormat::kNV12;
        case video_frame::PixelFormat::kYV12:
            return plugins::VideoPixelFormat::kYV12;
        case video_frame::PixelFormat::kBGRA:
            return plugins::VideoPixelFormat::kBGRA;
        case video_frame::PixelFormat::kRGBA:
            return plugins::VideoPixelFormat::kRGBA;
        case video_frame::PixelFormat::kRGB24:
        case video_frame::PixelFormat::kUnknown:
        default:
            return plugins::VideoPixelFormat::kUnknown;
    }
}

/** Translate plugin pattern → concrete pattern. The plugin layer exposes
 *  a `kCustom` pattern that has no concrete-side equivalent; we map it to
 *  `kSolidColor` as a fallback (real custom sources would replace
 *  MemoryVideoSource entirely, not just reroute through this adapter). */
Pattern to_concrete_pattern(plugins::VideoSourcePattern p) noexcept {
    switch (p) {
        case plugins::VideoSourcePattern::kCustom:
            return Pattern::kSolidColor;
        case plugins::VideoSourcePattern::kSolid:
            return Pattern::kSolidColor;
        case plugins::VideoSourcePattern::kColorBars:
            return Pattern::kColorBars;
        case plugins::VideoSourcePattern::kGradient:
            return Pattern::kGradient;
        case plugins::VideoSourcePattern::kFrameCount:
            return Pattern::kFrameCount;
        default:
            return Pattern::kColorBars;
    }
}

/** Translate plugin-level VideoSourceConfig → concrete SourceConfig. */
SourceConfig to_concrete(const plugins::VideoSourceConfig& p) noexcept {
    SourceConfig c;
    c.width  = p.width;
    c.height = p.height;
    c.fps    = p.fps;
    c.ssrc   = p.ssrc;
    c.format = to_concrete_format(p.format);
    c.pattern = to_concrete_pattern(p.pattern);
    c.solid_y = p.solid_y;
    c.solid_u = p.solid_u;
    c.solid_v = p.solid_v;
    c.start_capture_ts_us = static_cast<std::int64_t>(p.start_capture_ts_us);
    c.start_frame_seq = p.start_frame_seq;
    return c;
}

/** Translate concrete frame → plugin-level view. */
plugins::VideoSourceFrame
to_plugin_view(const video_frame::VideoFrameBuffer& buf,
               const video_frame::VideoFrameInfo&   info) noexcept {
    plugins::VideoSourceFrame f{};
    f.width  = buf.width();
    f.height = buf.height();
    f.format = to_plugin_format(buf.format());

    const auto& lay = buf.layout();
    f.stride_y = lay.strides[0];
    f.stride_u = lay.strides[1];
    f.stride_v = lay.strides[2];
    f.plane_offset_y = lay.plane_offsets[0];
    f.plane_offset_u = lay.plane_offsets[1];
    f.plane_offset_v = lay.plane_offsets[2];

    const std::uint8_t* const base = buf.data();
    f.plane_y = base + lay.plane_offsets[0];
    f.plane_u = (lay.num_planes >= 2) ? (base + lay.plane_offsets[1]) : nullptr;
    f.plane_v = (lay.num_planes >= 3) ? (base + lay.plane_offsets[2]) : nullptr;
    f.buffer_size = buf.size();

    f.capture_ts_us = static_cast<plugins::TimestampUs>(info.capture_ts_us);
    f.frame_seq    = info.frame_seq;
    f.rtp_timestamp = info.rtp_timestamp;
    return f;
}

/** Translate concrete SourceConfig → plugin-level VideoSourceConfig. */
plugins::VideoSourceConfig to_plugin(const SourceConfig& c) noexcept {
    plugins::VideoSourceConfig p{};
    p.width  = c.width;
    p.height = c.height;
    p.fps    = c.fps;
    p.ssrc   = c.ssrc;
    p.format = to_plugin_format(c.format);
    p.solid_y = c.solid_y;
    p.solid_u = c.solid_u;
    p.solid_v = c.solid_v;
    p.start_capture_ts_us = static_cast<plugins::TimestampUs>(c.start_capture_ts_us);
    p.start_frame_seq = c.start_frame_seq;
    // Concrete Pattern has no plugin equivalent (kCustom maps to kSolidColor
    // on the inbound side, but outgoing we report kCustom to mean "concrete
    // is authoritative"). Plugin-side callers won't switch on this anyway;
    // use of `config()` is for diagnostics.
    p.pattern = plugins::VideoSourcePattern::kCustom;
    return p;
}

/** Translate concrete stats → plugin stats. */
plugins::IVideoSource::Stats to_plugin_stats(const IVideoSource::Stats& s) noexcept {
    plugins::IVideoSource::Stats out{};
    out.frames_produced = s.frames_produced;
    out.frames_dropped  = s.frames_dropped;
    out.ticks           = s.ticks;
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

PluginAdapter::PluginAdapter(std::unique_ptr<video_source::IVideoSource> impl) noexcept
    : concrete_(std::move(impl)) {}

PluginAdapter::~PluginAdapter() = default;

const char* PluginAdapter::name() const noexcept {
    return "nimrtc::video_source::PluginAdapter (wraps concrete IVideoSource)";
}

plugins::Status PluginAdapter::open() noexcept {
    if (!concrete_) {
        // Lazy creation: make a MemoryVideoSource with defaults.
        SourceConfig cfg{};
        concrete_ = create_memory_source(cfg, {});
    }
    opened_ = true;
    return plugins::kOk;
}

void PluginAdapter::close() noexcept {
    if (concrete_) concrete_.reset();
    user_cb_ = {};
    registered_with_concrete_ = false;
    opened_ = false;
}

plugins::Status PluginAdapter::start() noexcept {
    if (!concrete_) return plugins::kErrNotReady;
    if (!registered_with_concrete_) {
        registered_with_concrete_ = true;
        auto wrapped_cb = [this](video_frame::VideoFrameBuffer buf,
                                 video_frame::VideoFrameInfo info) {
            if (user_cb_) {
                user_cb_(to_plugin_view(buf, info));
            }
        };
        concrete_->set_callback(std::move(wrapped_cb));
    }
    auto rc = concrete_->start();
    return (rc.ok()) ? plugins::kOk : plugins::kErrInternal;
}

void PluginAdapter::stop() noexcept {
    if (concrete_) concrete_->stop();
}

bool PluginAdapter::running() const noexcept {
    return concrete_ && concrete_->running();
}

void PluginAdapter::produce_one() noexcept {
    if (concrete_) concrete_->produce_one();
}

plugins::VideoSourceConfig PluginAdapter::config() const noexcept {
    if (!concrete_) return {};
    return to_plugin(concrete_->config());
}

void PluginAdapter::set_pattern(plugins::VideoSourcePattern p) noexcept {
    if (concrete_) concrete_->set_pattern(to_concrete_pattern(p));
}

void PluginAdapter::set_callback(plugins::VideoSourceFrameCallback cb) noexcept {
    user_cb_ = std::move(cb);
    // The translator is registered in start(); here we just store the
    // user's callback for the translator to invoke.
}

plugins::IVideoSource::Stats PluginAdapter::stats() const noexcept {
    if (!concrete_) return {};
    return to_plugin_stats(concrete_->stats());
}

// ---------------------------------------------------------------------------
// MemoryPluginFactory
// ---------------------------------------------------------------------------

std::string_view MemoryPluginFactory::id() const noexcept {
    return "memory";
}

std::string_view MemoryPluginFactory::display_name() const noexcept {
    return "In-memory test pattern source (MemoryVideoSource)";
}

plugins::IVideoSource* MemoryPluginFactory::create(plugins::VideoSourceConfig cfg) const {
    auto concrete = create_memory_source(to_concrete(cfg), {});
    return new PluginAdapter(std::move(concrete));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static MemoryPluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_video_source(
                s_factory.id(), &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::video_source
