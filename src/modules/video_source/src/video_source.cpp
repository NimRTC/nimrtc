/**
 * @file src/modules/video_source/src/video_source.cpp
 * @brief In-memory video frame source implementation.
 */

#include <nimrtc/video_source/video_source.hpp>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <nimrtc/core/log.hpp>

namespace nimrtc::video_source {

namespace {

constexpr auto kLog = core::log::Level::Debug;

// Format-string attribute required by Clang/GCC to suppress
// -Wformat-nonliteral on this variadic helper (the format string
// comes from the caller; the compiler cannot prove it is a literal).
// Forward-declare with the attribute, then define separately so GCC
// (which rejects attributes on inline function *definitions*) is
// happy with the same syntax as Clang.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void vsrc_debug(const char* fmt, ...);

inline void vsrc_debug(const char* fmt, ...) {
    if (core::log::Logger::instance().level() <= kLog) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        core::log::Logger::instance().debug(buf);
    }
}

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Pattern renderers
// ---------------------------------------------------------------------------

void render_solid(const SourceConfig& cfg, std::uint8_t* y,
                  std::uint8_t* u, std::uint8_t* v) noexcept {
    const std::uint32_t w = cfg.width;
    const std::uint32_t h = cfg.height;
    std::memset(y, cfg.solid_y, w * h);
    std::memset(u, cfg.solid_u, (w / 2) * (h / 2));
    std::memset(v, cfg.solid_v, (w / 2) * (h / 2));
}

/** SMPTE-style colour bars for a U/V row (chroma-only, full image). */
void render_bars_uv(const SourceConfig& cfg, std::uint8_t* u,
                    std::uint8_t* v) noexcept {
    const std::uint32_t w = cfg.width;
    const std::uint32_t h = cfg.height;
    const std::uint32_t half_w = w / 2;
    const std::uint32_t half_h = h / 2;

    // 7-bar palette (BT.601 limited range → U/V values).
    // Each entry: (U, V).
    // Generate a clear visual pattern: alternating 7 vertical bars with
    // distinct (U,V) pairs so we can clearly see changes when toggling
    // patterns.
    static constexpr std::uint8_t kU[7] = {128, 0, 0, 0, 255, 255, 128};
    static constexpr std::uint8_t kV[7] = {128, 0, 255, 0, 255, 0, 128};
    const std::uint32_t bar_w = std::max<std::uint32_t>(1, half_w / 7);
    for (std::uint32_t j = 0; j < half_h; ++j) {
        for (std::uint32_t i = 0; i < half_w; ++i) {
            std::uint32_t bar = std::min<std::uint32_t>(6, i / bar_w);
            u[j * half_w + i] = kU[bar];
            v[j * half_w + i] = kV[bar];
        }
    }
}

void render_bars_y(const SourceConfig& cfg, std::uint8_t* y) noexcept {
    const std::uint32_t w = cfg.width;
    const std::uint32_t h = cfg.height;
    // Y in BT.601 limited range: 16 (black) – 235 (white).
    static constexpr std::uint8_t kY[7] = {235, 219, 188, 173, 145, 113, 16};
    const std::uint32_t bar_w = std::max<std::uint32_t>(1, w / 7);
    for (std::uint32_t j = 0; j < h; ++j) {
        for (std::uint32_t i = 0; i < w; ++i) {
            std::uint32_t bar = std::min<std::uint32_t>(6, i / bar_w);
            y[j * w + i] = kY[bar];
        }
    }
}

void render_gradient(const SourceConfig& cfg, std::uint8_t* y,
                     std::uint8_t* u, std::uint8_t* v) noexcept {
    const std::uint32_t w = cfg.width;
    const std::uint32_t h = cfg.height;
    for (std::uint32_t j = 0; j < h; ++j) {
        for (std::uint32_t i = 0; i < w; ++i) {
            y[j * w + i] = static_cast<std::uint8_t>((i * 255) / std::max<std::uint32_t>(1, w - 1));
        }
    }
    const std::uint32_t half_w = w / 2;
    const std::uint32_t half_h = h / 2;
    for (std::uint32_t j = 0; j < half_h; ++j) {
        for (std::uint32_t i = 0; i < half_w; ++i) {
            u[j * half_w + i] = static_cast<std::uint8_t>((i * 255) / std::max<std::uint32_t>(1, half_w - 1));
            v[j * half_w + i] = static_cast<std::uint8_t>((j * 255) / std::max<std::uint32_t>(1, half_h - 1));
        }
    }
}

/** Frame counter overlay: encodes a small number into 8x8 blocks at the
 *  top-left corner.  Limited to 4 decimal digits. */
void overlay_frame_count(std::uint32_t frame_seq, std::uint8_t* y_plane,
                         std::uint32_t w, std::uint32_t h) noexcept {
    if (w < 8 * 4 || h < 8) return;
    static constexpr std::uint8_t kGlyph[10][8] = {
        {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},  // 0
        {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},  // 1
        {0x3E, 0x63, 0x03, 0x0E, 0x1C, 0x38, 0x7F, 0x00},  // 2
        {0x3E, 0x63, 0x03, 0x1C, 0x03, 0x63, 0x3E, 0x00},  // 3
        {0x06, 0x0E, 0x1E, 0x36, 0x66, 0x7F, 0x06, 0x00},  // 4
        {0x7F, 0x7F, 0x60, 0x7E, 0x03, 0x63, 0x3E, 0x00},  // 5
        {0x1C, 0x30, 0x60, 0x7E, 0x63, 0x63, 0x3E, 0x00},  // 6
        {0x7F, 0x63, 0x03, 0x06, 0x0C, 0x18, 0x18, 0x00},  // 7
        {0x3E, 0x63, 0x63, 0x3E, 0x63, 0x63, 0x3E, 0x00},  // 8
        {0x3E, 0x63, 0x63, 0x3F, 0x03, 0x06, 0x38, 0x00},  // 9
    };

    // Render the last 4 digits of the frame sequence number.
    for (int digit = 3; digit >= 0; --digit) {
        std::uint32_t d = frame_seq % 10;
        frame_seq /= 10;
        std::uint32_t x0 = static_cast<std::uint32_t>(3 - digit) * 8u;
        for (std::uint32_t y = 0; y < 8; ++y) {
            std::uint8_t row = kGlyph[d][y];
            for (std::uint32_t x = 0; x < 8; ++x) {
                if (row & (0x80 >> x)) {
                    // Each glyph pixel → 2x2 block to be visible.
                    for (std::uint32_t yy = 0; yy < 2; ++yy) {
                        for (std::uint32_t xx = 0; xx < 2; ++xx) {
                            std::uint32_t px = x0 + x * 2 + xx;
                            std::uint32_t py = y * 2 + yy;
                            if (px < w && py < h) {
                                y_plane[py * w + px] = 235;     // white
                            }
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// MemoryVideoSource — concrete impl
// ---------------------------------------------------------------------------

class MemoryVideoSource final : public IVideoSource {
public:
    explicit MemoryVideoSource(SourceConfig cfg) : cfg_(std::move(cfg)) {
        layout_ = video_frame::compute_layout(cfg_.format,
                                              cfg_.width,
                                              cfg_.height);
    }
    ~MemoryVideoSource() override {
        stop();
        if (thread_.joinable()) thread_.join();
    }

    core::Result<void> start() noexcept override {
        bool was_running = running_.exchange(true);
        if (was_running) return core::Result<void>::make_ok();
        // Reset cadence from now.
        next_emit_us_ = now_us();
        stop_flag_ = false;
        thread_ = std::thread([this] { cadence_loop(); });
        vsrc_debug("MemoryVideoSource: started (%ux%u @ %u fps, pattern=%s)",
                   cfg_.width, cfg_.height, cfg_.fps,
                   std::string(pattern_name(cfg_.pattern)).c_str());
        return core::Result<void>::make_ok();
    }

    void stop() noexcept override {
        if (!running_.exchange(false)) return;
        stop_flag_ = true;
        // The thread will exit on its own; we join in the destructor.
    }

    bool running() const noexcept override { return running_.load(); }

    void produce_one() noexcept override {
        do_produce();
    }

    SourceConfig config() const noexcept override { return cfg_; }

    void set_pattern(Pattern p) noexcept override { cfg_.pattern = p; }

    void set_callback(FrameCallback cb) noexcept override {
        std::lock_guard<std::mutex> lk(cb_mu_);
        cb_ = std::move(cb);
    }

    Stats stats() const noexcept override {
        Stats s;
        s.frames_produced = frames_produced_.load();
        s.frames_dropped  = frames_dropped_.load();
        s.ticks           = ticks_.load();
        return s;
    }

private:
    void cadence_loop() noexcept {
        using clock = std::chrono::steady_clock;
        const std::int64_t interval_us = (cfg_.fps == 0) ? 0 :
            static_cast<std::int64_t>(1'000'000 / cfg_.fps);
        while (!stop_flag_) {
            auto wakeup = clock::time_point(std::chrono::microseconds(next_emit_us_));
            std::this_thread::sleep_until(wakeup);
            if (stop_flag_) break;
            ticks_.fetch_add(1);
            do_produce();
            next_emit_us_ += interval_us;
            // If we fell behind, skip the missed slots (catch-up).
            std::int64_t now = now_us();
            if (next_emit_us_ < now - interval_us) {
                frames_dropped_.fetch_add(1);
                next_emit_us_ = now;
            }
        }
    }

    void do_produce() noexcept {
        video_frame::VideoFrameBuffer buffer(layout_);
        video_frame::VideoFrameInfo info{};
        auto st = render_pattern(cfg_, buffer, info);
        if (!st) {
            frames_dropped_.fetch_add(1);
            return;
        }
        buffer.set_capture_ts_us(info.capture_ts_us);
        buffer.set_frame_seq(info.frame_seq);
        frames_produced_.fetch_add(1);
        cfg_.start_frame_seq += 1;   // advance for next frame
        FrameCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            cb = cb_;
        }
        if (cb) cb(std::move(buffer), info);
    }

    SourceConfig cfg_;
    video_frame::VideoFrameLayout layout_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    std::thread thread_;
    std::int64_t  next_emit_us_ = 0;
    std::atomic<std::uint64_t> frames_produced_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> ticks_{0};
    std::mutex cb_mu_;
    FrameCallback cb_;
};

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::unique_ptr<IVideoSource> create_memory_source(SourceConfig cfg,
                                                   FrameCallback callback) noexcept {
    auto p = std::make_unique<MemoryVideoSource>(std::move(cfg));
    p->set_callback(std::move(callback));
    return p;
}

core::Result<void> render_pattern(const SourceConfig& cfg,
                            video_frame::VideoFrameBuffer& buffer,
                            video_frame::VideoFrameInfo& info) noexcept {
    if (!buffer.valid()) {
        return core::Result<void>::fail(core::ErrorCode::InvalidArgument,
                                        "render_pattern: buffer is empty");
    }
    if (buffer.format() != cfg.format) {
        return core::Result<void>::fail(core::ErrorCode::InvalidArgument,
                                        "render_pattern: buffer format mismatch");
    }
    if (buffer.width() != cfg.width || buffer.height() != cfg.height) {
        return core::Result<void>::fail(core::ErrorCode::InvalidArgument,
                                        "render_pattern: buffer dimensions mismatch");
    }

    std::uint8_t* y = buffer.plane(0);
    std::uint8_t* u = buffer.plane(1);
    std::uint8_t* v = buffer.plane(2);
    if (!y || !u || !v) {
        return core::Result<void>::fail(core::ErrorCode::InvalidArgument,
                                        "render_pattern: plane pointers null");
    }

    switch (cfg.pattern) {
        case Pattern::kSolidColor:
            render_solid(cfg, y, u, v);
            break;
        case Pattern::kColorBars:
            render_bars_y(cfg, y);
            render_bars_uv(cfg, u, v);
            break;
        case Pattern::kGradient:
            render_gradient(cfg, y, u, v);
            break;
        case Pattern::kFrameCount:
            render_gradient(cfg, y, u, v);
            overlay_frame_count(cfg.start_frame_seq, y,
                               cfg.width, cfg.height);
            break;
        default:
            return core::Result<void>::fail(core::ErrorCode::NotImplemented,
                                            "render_pattern: unknown pattern");
    }

    info.capture_ts_us = cfg.start_capture_ts_us;
    info.frame_seq     = cfg.start_frame_seq;
    // 90 kHz clock: frame_seq * (90000 / fps) is the canonical mapping,
    // but for the source we just give a counter-based stamp.
    info.rtp_timestamp = cfg.start_frame_seq * (90000u / std::max<std::uint32_t>(1u, cfg.fps));
    return core::Result<void>::make_ok();
}

std::string_view pattern_name(Pattern p) noexcept {
    switch (p) {
        case Pattern::kSolidColor: return "SolidColor";
        case Pattern::kColorBars:  return "ColorBars";
        case Pattern::kGradient:   return "Gradient";
        case Pattern::kFrameCount: return "FrameCount";
        default:                   return "Unknown";
    }
}

} // namespace nimrtc::video_source
