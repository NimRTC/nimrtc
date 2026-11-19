/**
 * @file examples/remote-desktop/game_source.hpp
 * @brief The synthetic "whack-a-mole" game source (slice v1-game-source).
 *
 * ## What this is
 *
 * A *memory-frame* video capture device for the M1 single-machine demo:
 * it synthesises a 2D whack-a-mole game picture and pumps it into the
 * engine's capture pump (remote-desktop plan A10) exactly like the real
 * screen backends do — so the device-side capture chain is reused with
 * zero changes.
 *
 * It implements `plugins::IVideoCapture` (the same seam as DXGI / WGC /
 * ScreenCaptureKit / PipeWire) and produces plain 8-bit BGRA pixels, the
 * format every desktop compositor hands over natively.
 *
 * ## The picture
 *
 * A green play field with a 3x3 grid of holes; a mole pops out of a hole
 * chosen *deterministically* by the frame number; and a **frame-counter
 * overlay** in the top-left corner advances every frame.  The overlay is
 * what makes the picture change on every single frame, so the device's
 * dirty-rect report is honestly `full_frame = true`.
 *
 * ## The cadence
 *
 * `open()` starts a producer thread that renders one frame every
 * `1/fps` seconds — a **fixed-step** cadence.  The slice spec's risk
 * note says to drive the synthetic source at a fixed 60 fps first and
 * add pacing later; the thread therefore sleeps *to the frame deadline*
 * (and resyncs when it falls behind) instead of free-running, so the
 * delivered rate is the configured one.
 *
 * The capture seam's threading contract (RD-6) is preserved: the device
 * owns its own thread and fills its bounded queue; the engine only ever
 * *polls* that queue from `tick()`.
 *
 * ## Resolution / frame rate
 *
 * The default resolution is **1280x720** — the "720p floor" of the
 * slice spec, chosen so the demo picture stays readable.  Both the size
 * and the frame rate come from the `plugins::CaptureConfig` handed to
 * the factory, so a host can request any advertised geometry.
 *
 * @note The frame sequence number handed to the queue is the strictly
 *       increasing frame index — the slice's acceptance criterion.
 */

#ifndef NIMRTC_RD_GAME_SOURCE_HPP
#define NIMRTC_RD_GAME_SOURCE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>

namespace nimrtc::rd {

// ---------------------------------------------------------------------------
// GameSource
// ---------------------------------------------------------------------------

/**
 * @brief A capture device that synthesises a whack-a-mole game picture.
 *
 * Implements `plugins::IVideoCapture`.  Frames are produced on the
 * device's own producer thread at a fixed cadence (60 fps by default)
 * into the same bounded `video_capture::CaptureQueue` the screen
 * backends use, so the engine's capture pump drives them to the encoder
 * unchanged.
 */
class NIMRTC_API_STABLE GameSource final : public plugins::IVideoCapture {
public:
    /** Construct a game source with the given configuration. */
    explicit GameSource(plugins::CaptureConfig cfg) noexcept;

    ~GameSource() override;

    GameSource(const GameSource&)            = delete;
    GameSource& operator=(const GameSource&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "game-source"; }

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IVideoCapture ----------------------------------------------------
    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::CaptureCapabilities
    capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    [[nodiscard]] plugins::CaptureConfig config() const noexcept override;

    plugins::Status pop_frame(plugins::CaptureFrame& out) noexcept override;

    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override {
        return stats_;
    }

private:
    /** Render one game frame into `scratch_` and push it on the queue. */
    plugins::Status produce_frame() noexcept;

    /** The producer thread's fixed-step loop (1/fps per frame). */
    void producer_loop() noexcept;

    /** Draw the whole whack-a-mole picture for `frame_index`. */
    void render_frame(std::uint8_t* dst, std::size_t size,
                      std::uint32_t frame_index) const noexcept;

    // ---- Small drawing helpers (all BGRA, byte order B,G,R,A) -------------

    static void put_pixel(std::uint8_t* dst, std::uint32_t w, std::uint32_t h,
                          std::uint32_t x, std::uint32_t y,
                          std::uint8_t b, std::uint8_t g, std::uint8_t r) noexcept {
        if (x >= w || y >= h) return;
        std::uint8_t* p = dst + (static_cast<std::size_t>(y) * w + x) * 4u;
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = 0xFFu;
    }

    static void fill_rect(std::uint8_t* dst, std::uint32_t w, std::uint32_t h,
                          std::uint32_t x0, std::uint32_t y0,
                          std::uint32_t x1, std::uint32_t y1,
                          std::uint8_t b, std::uint8_t g, std::uint8_t r) noexcept {
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = x0; x < x1; ++x) {
                put_pixel(dst, w, h, x, y, b, g, r);
            }
        }
    }

    static void fill_circle(std::uint8_t* dst, std::uint32_t w, std::uint32_t h,
                            std::int32_t cx, std::int32_t cy,
                            std::int32_t radius,
                            std::uint8_t b, std::uint8_t g, std::uint8_t r) noexcept {
        for (std::int32_t dy = -radius; dy <= radius; ++dy) {
            for (std::int32_t dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy <= radius * radius) {
                    put_pixel(dst, w, h, static_cast<std::uint32_t>(cx + dx),
                              static_cast<std::uint32_t>(cy + dy), b, g, r);
                }
            }
        }
    }

    /** Draw one 5x7 bitmap-font digit, each font pixel scaled `scale`x. */
    static void draw_digit(std::uint8_t* dst, std::uint32_t w, std::uint32_t h,
                           std::uint32_t x, std::uint32_t y,
                           std::uint32_t digit, std::uint32_t scale) noexcept;

    /** Draw a 6-digit right-aligned frame-counter overlay at (x, y). */
    void draw_frame_counter(std::uint8_t* dst, std::uint32_t w, std::uint32_t h,
                            std::uint32_t x, std::uint32_t y,
                            std::uint32_t frame_index) const noexcept;

    // ---- State --------------------------------------------------------------

    plugins::CaptureConfig cfg_;
    nimrtc::video_capture::CaptureQueue queue_{2};
    plugins::CaptureFrameCallback cb_;
    plugins::CaptureStats stats_{};
    std::vector<std::uint8_t> scratch_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::uint32_t frame_index_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// GameSourceFactory
// ---------------------------------------------------------------------------

/** Factory for the game source.  Registers under the id `"game-source"`. */
class NIMRTC_API_STABLE GameSourceFactory final
    : public plugins::IVideoCaptureFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "game-source";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Synthetic whack-a-mole game source (M1 demo)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override {
        return new GameSource(std::move(cfg));
    }
};

// ===========================================================================
// Implementation (header-only — the example's whole slice lives in this file)
// ===========================================================================

namespace {

// The game picture's geometry is relative to the frame, so it stays a
// recognizable whack-a-mole scene at any advertised resolution.
//
// ── The implicit contract with the AI decision side ──────────────────
//
// This board (a `kHoleColumns` x `kHoleRows` grid — 3x3 = 9 holes) and the
// decision model's board (`ai_decision.hpp`'s `kHoles = 3`, a single row of
// left / center / right) are **two independent models of a whack-a-mole
// game**.  The two modules do not link each other: the decision demo is
// self-contained (pure C++20 arithmetic, no NimRTC linkage), and this file
// is a capture device that knows nothing about AI decisions.
//
// The consequence is deliberate but worth stating plainly:
//
//   * Changing `kHoleColumns` / `kHoleRows` here changes **only** what is
//     rendered.  It does not change the AI side's three-hole model, and
//     nothing at compile time will tell you so.
//   * Changing `kHoles` in `ai_decision.hpp` changes **only** the decision
//     model.  It does not change what is rendered.
//
// They agree today by coincidence of both being "3", not by construction.
// If a future change needs the two boards to be the *same* board, that is a
// contract change: it needs a shared constant (or a shared header) and an
// ADR — not a quiet edit to either number.
constexpr std::uint32_t kHoleColumns = 3;
constexpr std::uint32_t kHoleRows    = 3;

// The mole changes hole once per second (60 frames at 60 fps) and stays
// up for 25 of the 60 frames, so the demo shows constant motion without
// needing a wall clock.
constexpr std::uint32_t kMoleCycleFrames = 60;
constexpr std::uint32_t kMoleUpFrames    = 25;

/** The 5x7 bitmap-font glyphs for '0'..'9' (bit 4..0 = column 4..0). */
constexpr std::uint8_t kDigitGlyphs[10][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
};

/** Monotonic microsecond clock, the same epoch the engine uses. */
std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // anonymous namespace

inline GameSource::GameSource(plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {
    queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) queue_.set_depth(cfg_.queue_depth);
}

inline GameSource::~GameSource() { close(); }

plugins::Status GameSource::open() noexcept {
    if (open_) return plugins::kOk;

    // Zero-sized request means "the device's best", which for the game
    // source is the slice's 720p floor.
    if (cfg_.format.width == 0)  cfg_.format.width  = 1280;
    if (cfg_.format.height == 0) cfg_.format.height = 720;
    if (cfg_.format.fps == 0)    cfg_.format.fps    = 60;
    if (cfg_.format.fps > 120)   cfg_.format.fps    = 120;

    // The negotiated format must agree with the capability: the device
    // *does* report a dirty rect per frame (the frame-counter overlay
    // changes every frame), so the format has to say so.
    cfg_.format.has_dirty_rects = true;
    cfg_.format.zero_copy = false;  // a synthetic device has no GPU surface

    if (cfg_.format.width == 0 || cfg_.format.height == 0) {
        return plugins::kErrInvalidParam;
    }

    // The device id is opaque; an unknown one is an error, not a silent
    // fallback.
    if (!cfg_.device_id.empty() && cfg_.device_id != "game:0") {
        return plugins::kErrInvalidParam;
    }
    cfg_.device_id = "game:0";

    const std::size_t bytes = static_cast<std::size_t>(cfg_.format.width) *
                              static_cast<std::size_t>(cfg_.format.height) * 4u;
    scratch_.assign(bytes, 0u);
    frame_index_ = 0;
    open_ = true;

    // The producer thread starts here and stops in `close()`.  It fills
    // the bounded queue; the engine's pump polls that queue on its tick
    // thread (RD-6 — the codec is never touched from the device thread).
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { producer_loop(); });
    return plugins::kOk;
}

void GameSource::close() noexcept {
    if (!open_) return;
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    open_ = false;
    queue_.clear();
    scratch_.clear();
    scratch_.shrink_to_fit();
}

plugins::CaptureConfig GameSource::config() const noexcept { return cfg_; }

std::vector<plugins::CaptureDevice>
GameSource::enumerate_devices() const noexcept {
    plugins::CaptureDevice d;
    d.id   = "game:0";
    d.name = "Whack-a-mole (synthetic game source)";
    d.kind = plugins::CaptureDeviceKind::kScreen;
    d.max_width  = 1920;
    d.max_height = 1080;
    d.max_fps    = 120;
    return {d};
}

std::vector<plugins::CaptureFormat>
GameSource::supported_formats(const std::string& device_id) const noexcept {
    (void)device_id;
    std::vector<plugins::CaptureFormat> out;
    // The game source offers BGRA (what the capture pump converts to the
    // codec's canonical layout) at a few common sizes.
    const std::uint32_t sizes[][2] = {
        {640, 480}, {1280, 720}, {1920, 1080},
    };
    for (const auto& s : sizes) {
        plugins::CaptureFormat f;
        f.width  = s[0];
        f.height = s[1];
        f.fps    = 60;
        f.tag    = "bgra";
        f.zero_copy = false;   // a synthetic device has no GPU surface
        f.has_dirty_rects = true;
        out.push_back(std::move(f));
    }
    return out;
}

plugins::CaptureCapabilities GameSource::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    c.zero_copy = false;
    c.dirty_rects = true;          // full_frame=true per frame (the counter)
    c.cursor_composited = false;   // no pointer in a synthetic picture
    c.region_selection = false;    // the whole play field is rendered
    c.enumeration = true;
    c.max_pixels = 1920ull * 1080ull;
    c.max_fps    = 120;
    c.backend    = "game-source";
    return c;
}

plugins::Status GameSource::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!open_) return plugins::kErrNotReady;
    if (!queue_.pop(out)) return plugins::kErrNotReady;
    // The game source produces BGRA (see `render_frame`); declaring it
    // keeps the engine's BGRA → codec-layout conversion on the right
    // path instead of misreading a packed buffer as planar I420.
    out.format       = plugins::VideoPixelFormat::kBGRA;
    out.gpu_backend  = plugins::GpuBackend::kNone;
    out.has_gpu_handle = false;
    out.gpu_handle   = nullptr;
    return plugins::kOk;
}

plugins::Status GameSource::produce_frame() noexcept {
    if (!open_) return plugins::kErrNotReady;

    const std::uint32_t w = cfg_.format.width;
    const std::uint32_t h = cfg_.format.height;
    if (scratch_.size() < static_cast<std::size_t>(w) * h * 4u) {
        return plugins::kErrBufferTooSmall;
    }

    // The whole game picture changes every frame (the frame-counter
    // overlay), so the honest dirty report is "full frame".
    plugins::CaptureDirtyRects dirty;
    dirty.full_frame = true;

    render_frame(scratch_.data(), scratch_.size(), frame_index_);

    const std::int64_t ts = now_us();
    const auto outcome = queue_.push(scratch_.data(), scratch_.size(),
                                     static_cast<std::int32_t>(w * 4u),
                                     w, h, ts, frame_index_, dirty);
    switch (outcome) {
        case nimrtc::video_capture::CaptureQueue::PushResult::kQueued:
            ++stats_.frames_captured;
            break;
        case nimrtc::video_capture::CaptureQueue::PushResult::kDroppedOldest:
            // The frame itself is queued, but the oldest one was evicted
            // to make room — that eviction is the drop.
            ++stats_.frames_captured;
            ++stats_.frames_dropped;
            break;
        case nimrtc::video_capture::CaptureQueue::PushResult::kRejected:
            // The arrival was refused; the queue still holds what it had.
            ++stats_.frames_dropped;
            break;
    }

    // The frame sequence number is the strictly increasing frame index
    // (the slice's acceptance criterion: "帧序号单调递增").
    ++frame_index_;

    if (cb_) {
        plugins::CaptureFrame view;
        if (queue_.peek(view)) cb_(view);
    }
    return plugins::kOk;
}

void GameSource::producer_loop() noexcept {
    // A fixed-step cadence: one frame every 1/fps seconds.  The slice
    // spec's risk note says to drive the synthetic source at a fixed
    // 60 fps first and add pacing later, so the thread sleeps to the
    // frame deadline and resyncs when a hiccup makes it fall behind —
    // it never free-runs faster than the configured rate.
    const std::uint32_t fps =
        std::clamp(cfg_.format.fps, 1u, 120u);
    const auto period = std::chrono::microseconds(
        1'000'000 / static_cast<std::int64_t>(fps));
    auto next = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        produce_frame();
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (now < next) {
            std::this_thread::sleep_until(next);
        } else {
            // Fell behind (a pause, a scheduler hiccup): resync instead
            // of piling the missed frames onto the queue.
            next = now + period;
        }
    }
}

void GameSource::draw_digit(std::uint8_t* dst, std::uint32_t w, std::uint32_t h,
                            std::uint32_t x, std::uint32_t y,
                            std::uint32_t digit, std::uint32_t scale) noexcept {
    if (digit > 9) return;
    for (std::uint32_t row = 0; row < 7; ++row) {
        const std::uint8_t bits = kDigitGlyphs[digit][row];
        for (std::uint32_t col = 0; col < 5; ++col) {
            if ((bits >> (4u - col)) & 1u) {
                fill_rect(dst, w, h,
                          x + col * scale, y + row * scale,
                          x + (col + 1u) * scale, y + (row + 1u) * scale,
                          0xFFu, 0xFFu, 0xFFu);
            }
        }
    }
}

void GameSource::draw_frame_counter(std::uint8_t* dst, std::uint32_t w,
                                    std::uint32_t h, std::uint32_t x,
                                    std::uint32_t y,
                                    std::uint32_t frame_index) const noexcept {
    // A 6-digit, right-aligned counter.  The padding keeps the overlay
    // width constant so the picture does not jump as the digit count
    // grows — and the overlay is what makes every frame different.
    constexpr std::uint32_t kDigits   = 6;
    constexpr std::uint32_t kScale    = 2;
    constexpr std::uint32_t kCellW    = 5u * kScale;
    constexpr std::uint32_t kCellH    = 7u * kScale;
    constexpr std::uint32_t kPad      = 2u;
    const std::uint32_t box_w = kDigits * kCellW + 2u * kPad;
    const std::uint32_t box_h = kCellH + 2u * kPad;

    fill_rect(dst, w, h, x, y, x + box_w, y + box_h, 0x00u, 0x00u, 0x00u);

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06u", frame_index);
    for (std::uint32_t i = 0; i < kDigits; ++i) {
        const char c = buf[i];
        if (c >= '0' && c <= '9') {
            draw_digit(dst, w, h,
                       x + kPad + i * kCellW, y + kPad,
                       static_cast<std::uint32_t>(c - '0'), kScale);
        }
    }
}

void GameSource::render_frame(std::uint8_t* dst, std::size_t size,
                              std::uint32_t frame_index) const noexcept {
    const std::uint32_t w = cfg_.format.width;
    const std::uint32_t h = cfg_.format.height;
    if (dst == nullptr || size == 0 || w == 0 || h == 0) return;

    // ---- The grass-green play field, shaded a little by row ------------
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* row = dst + static_cast<std::size_t>(y) * w * 4u;
        const std::uint8_t shade = static_cast<std::uint8_t>(
            48u + ((y * 24u) / (h == 0 ? 1u : h)));
        for (std::uint32_t x = 0; x < w; ++x) {
            row[x * 4u + 0u] = 40u;                 // B
            row[x * 4u + 1u] = static_cast<std::uint8_t>(shade + 90u);  // G
            row[x * 4u + 2u] = 20u;                 // R
            row[x * 4u + 3u] = 0xFFu;               // A
        }
    }

    // ---- The 3x3 grid of holes ------------------------------------------
    const std::uint32_t hole_radius = std::max(2u, std::min(w, h) / 14u);
    struct Hole { std::int32_t cx; std::int32_t cy; };
    Hole holes[kHoleColumns * kHoleRows];
    for (std::uint32_t r = 0; r < kHoleRows; ++r) {
        for (std::uint32_t c = 0; c < kHoleColumns; ++c) {
            const std::int32_t cx =
                static_cast<std::int32_t>(w * (2u * c + 1u) / (2u * kHoleColumns));
            const std::int32_t cy =
                static_cast<std::int32_t>(h * (2u * r + 1u) / (2u * kHoleRows));
            holes[r * kHoleColumns + c] = Hole{cx, cy};
            // The hole: dark dirt with a slightly lighter rim so it reads
            // as a recess rather than a flat circle.
            fill_circle(dst, w, h, cx, cy, static_cast<std::int32_t>(hole_radius),
                        42u, 28u, 14u);
            fill_circle(dst, w, h, cx, cy - 1,
                        static_cast<std::int32_t>(hole_radius) - 2,
                        58u, 40u, 20u);
        }
    }

    // ---- The mole (deterministic: which hole + up/down phase) ----------
    const std::uint32_t phase      = frame_index % kMoleCycleFrames;
    const std::uint32_t hole_index = (frame_index / kMoleCycleFrames) %
                                     (kHoleColumns * kHoleRows);
    if (phase < kMoleUpFrames) {
        const Hole& hole = holes[hole_index];
        const std::int32_t head_radius =
            static_cast<std::int32_t>(hole_radius * 4u / 5u);
        const std::int32_t head_cy = hole.cy - head_radius;
        // The head (brown), peeking out of the hole.
        fill_circle(dst, w, h, hole.cx, head_cy, head_radius,
                    120u, 78u, 40u);
        // Two eyes so it reads as a face.
        const std::int32_t eye_dx = head_radius / 3;
        const std::int32_t eye_y  = head_cy - head_radius / 3;
        const std::int32_t eye_r  = std::max(1, head_radius / 6);
        fill_circle(dst, w, h, hole.cx - eye_dx, eye_y, eye_r,
                    0xFFu, 0xFFu, 0xFFu);
        fill_circle(dst, w, h, hole.cx + eye_dx, eye_y, eye_r,
                    0xFFu, 0xFFu, 0xFFu);
        const std::int32_t pupil_r = std::max(1, eye_r / 2);
        fill_circle(dst, w, h, hole.cx - eye_dx, eye_y, pupil_r,
                    10u, 10u, 10u);
        fill_circle(dst, w, h, hole.cx + eye_dx, eye_y, pupil_r,
                    10u, 10u, 10u);
    }

    // ---- The frame-counter overlay (top-left) ---------------------------
    draw_frame_counter(dst, w, h, 8u, 8u, frame_index);
}

}  // namespace nimrtc::rd
#endif  // NIMRTC_RD_GAME_SOURCE_HPP
