// src/modules/rd_input/tests/test_rd_input.cpp
// ----------------------------------------------------------------------------
// The remote-desktop input module's unit tests.
//
// Three suites, one per acceptance criterion:
//
//   G3 (the input event codec)     — input_event_codec_*
//   G4 (coalescing / rate limiting) — input_coalescer_*
//   G7 (the input safety filter)   — input_safety_filter_*
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <nimrtc/core/log.hpp>
#include <nimrtc/rd_input/coalescer.hpp>
#include <nimrtc/rd_input/event_codec.hpp>
#include <nimrtc/rd_input/input_event.hpp>
#include <nimrtc/rd_input/safety_filter.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

using nimrtc::rd_input::Axis;
using nimrtc::rd_input::AxisRange;
using nimrtc::rd_input::ButtonAction;
using nimrtc::rd_input::DecodeOptions;
using nimrtc::rd_input::EncodeOptions;
using nimrtc::rd_input::FilterResult;
using nimrtc::rd_input::FilterVerdict;
using nimrtc::rd_input::InputCoalescer;
using nimrtc::rd_input::InputEvent;
using nimrtc::rd_input::InputEventType;
using nimrtc::rd_input::KeyAction;
using nimrtc::rd_input::MouseButton;
using nimrtc::rd_input::SafetyFilter;
using nimrtc::rd_input::SafetyFilterConfig;

// ---------------------------------------------------------------------------
// A log sink that captures the lines, so a test can assert that a denial
// was logged (G7's acceptance criterion).
// ---------------------------------------------------------------------------
class CapturingSink final : public nimrtc::core::log::Sink {
public:
    void write(nimrtc::core::log::Level level, std::string_view msg) noexcept override {
        if (level < nimrtc::core::log::Level::Warn) return;
        lines_.emplace_back(msg);
    }

    [[nodiscard]] const std::vector<std::string>& lines() const noexcept { return lines_; }

    [[nodiscard]] bool contains(std::string_view needle) const noexcept {
        for (const std::string& line : lines_) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    void clear() noexcept { lines_.clear(); }

private:
    std::vector<std::string> lines_;
};

// A scoped install of the capturing sink (and a restore of the level).
class ScopedLogCapture {
public:
    explicit ScopedLogCapture(std::shared_ptr<CapturingSink> sink)
        : sink_{std::move(sink)},
          previous_level_{nimrtc::core::log::Logger::instance().level()} {
        nimrtc::core::log::Logger::instance().set_sink(sink_);
        nimrtc::core::log::Logger::instance().set_level(nimrtc::core::log::Level::Debug);
    }

    ~ScopedLogCapture() {
        nimrtc::core::log::Logger::instance().set_level(previous_level_);
        nimrtc::core::log::Logger::instance().set_sink(nullptr);
    }

private:
    std::shared_ptr<CapturingSink> sink_;
    nimrtc::core::log::Level       previous_level_;
};

/// Round-trip one event through the codec and assert it survived.
void expect_round_trip(const InputEvent& event,
                        const EncodeOptions& options = EncodeOptions{}) {
    auto encoded = nimrtc::rd_input::encode_event(event, options);
    ASSERT_TRUE(encoded.ok()) << encoded.error().message();
    EXPECT_EQ(encoded.value().size(), nimrtc::rd_input::encoded_size(event, options))
        << "encoded_size() disagrees with the encoder";

    auto decoded = nimrtc::rd_input::decode_event(
        nimrtc::core::ByteSpan{encoded.value().data(), encoded.value().size()});
    ASSERT_TRUE(decoded.ok()) << decoded.error().message();
    EXPECT_EQ(decoded.value(), event);
}

} // namespace

// ===========================================================================
// G3 — the input event codec
// ===========================================================================

TEST(input_event_codec, round_trips_every_event_type) {
    // Mouse move.
    expect_round_trip(nimrtc::rd_input::make_mouse_move(7, -3));
    expect_round_trip(nimrtc::rd_input::make_mouse_move(0, 0));
    expect_round_trip(nimrtc::rd_input::make_mouse_move(-1, 1));
    expect_round_trip(nimrtc::rd_input::make_mouse_move(100000, -100000));

    // Mouse button.
    expect_round_trip(nimrtc::rd_input::make_mouse_button(
        MouseButton::kLeft, ButtonAction::kDown, 100, 200));
    expect_round_trip(nimrtc::rd_input::make_mouse_button(
        MouseButton::kRight, ButtonAction::kUp, -5, -7));
    expect_round_trip(nimrtc::rd_input::make_mouse_button(
        MouseButton::kMiddle, ButtonAction::kDown));

    // Wheel.
    expect_round_trip(nimrtc::rd_input::make_mouse_wheel(0, 120));
    expect_round_trip(nimrtc::rd_input::make_mouse_wheel(0, -120));
    expect_round_trip(nimrtc::rd_input::make_mouse_wheel(1, -1));

    // Key down / key up.
    expect_round_trip(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kDown));
    expect_round_trip(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kUp));
    expect_round_trip(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kEscape, KeyAction::kDown,
        static_cast<std::uint32_t>(nimrtc::rd_input::Modifier::kControl)
            | static_cast<std::uint32_t>(nimrtc::rd_input::Modifier::kShift),
        /*repeat=*/true));

    // Text.
    expect_round_trip(nimrtc::rd_input::make_text("hello"));
    expect_round_trip(nimrtc::rd_input::make_text(""));
    expect_round_trip(nimrtc::rd_input::make_text("\xe4\xbd\xa0\xe5\xa5\xbd"));
}

TEST(input_event_codec, mouse_move_delta_is_a_handful_of_bytes) {
    // G3's acceptance criterion: "the encoding is compact (a mouse-move
    // delta is a handful of bytes)".
    const InputEvent event = nimrtc::rd_input::make_mouse_move(3, -2);
    auto encoded = nimrtc::rd_input::encode_event(event);
    ASSERT_TRUE(encoded.ok());

    // 3 header bytes + 1 byte per zigzag varint (the deltas are small).
    EXPECT_EQ(encoded.value().size(), 5u);
    EXPECT_LE(encoded.value().size(), 8u);

    // A key press is compact too.
    const InputEvent key = nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kDown);
    auto encoded_key = nimrtc::rd_input::encode_event(key);
    ASSERT_TRUE(encoded_key.ok());
    EXPECT_LE(encoded_key.value().size(), 8u);
}

TEST(input_event_codec, carries_the_timestamp_on_request) {
    const InputEvent event = nimrtc::rd_input::make_mouse_move(1, 1, 1234567890ull);

    EncodeOptions with_ts;
    with_ts.include_timestamp = true;
    expect_round_trip(event, with_ts);

    auto encoded = nimrtc::rd_input::encode_event(event, with_ts);
    ASSERT_TRUE(encoded.ok());
    // The timestamp costs its varint length on top of the header.
    EXPECT_GT(encoded.value().size(), 5u);

    // Without the option the timestamp is not on the wire.
    EncodeOptions without_ts;
    without_ts.include_timestamp = false;
    auto plain = nimrtc::rd_input::encode_event(event, without_ts);
    ASSERT_TRUE(plain.ok());
    EXPECT_EQ(plain.value().size(), 5u);
}

TEST(input_event_codec, rejects_an_unknown_version) {
    EncodeOptions options;
    options.version = 99;  // a version this build does not know
    auto encoded = nimrtc::rd_input::encode_event(
        nimrtc::rd_input::make_mouse_move(1, 1), options);
    ASSERT_TRUE(encoded.ok());

    auto decoded = nimrtc::rd_input::decode_event(
        nimrtc::core::ByteSpan{encoded.value().data(), encoded.value().size()});
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), nimrtc::core::ErrorCode::ProtocolError);

    // A decoder that explicitly opts in accepts it.
    DecodeOptions permissive;
    permissive.allow_newer_version = true;
    auto permissive_decoded = nimrtc::rd_input::decode_event(
        nimrtc::core::ByteSpan{encoded.value().data(), encoded.value().size()},
        permissive);
    EXPECT_TRUE(permissive_decoded.ok());
}

TEST(input_event_codec, rejects_a_truncated_event) {
    auto encoded = nimrtc::rd_input::encode_event(
        nimrtc::rd_input::make_mouse_move(100, 100));
    ASSERT_TRUE(encoded.ok());

    for (std::size_t len = 0; len < encoded.value().size(); ++len) {
        auto decoded = nimrtc::rd_input::decode_event(
            nimrtc::core::ByteSpan{encoded.value().data(), len});
        EXPECT_FALSE(decoded.ok()) << "a " << len << "-byte prefix decoded";
    }
}

TEST(input_event_codec, rejects_a_text_payload_over_the_limit) {
    DecodeOptions options;
    options.max_text_bytes = 4;

    auto encoded = nimrtc::rd_input::encode_event(
        nimrtc::rd_input::make_text("far too long"));
    ASSERT_TRUE(encoded.ok());

    auto decoded = nimrtc::rd_input::decode_event(
        nimrtc::core::ByteSpan{encoded.value().data(), encoded.value().size()},
        options);
    EXPECT_FALSE(decoded.ok());
}

TEST(input_event_codec, round_trips_a_batch) {
    std::vector<InputEvent> events;
    events.push_back(nimrtc::rd_input::make_mouse_move(1, 2));
    events.push_back(nimrtc::rd_input::make_mouse_button(
        MouseButton::kLeft, ButtonAction::kDown, 10, 20));
    events.push_back(nimrtc::rd_input::make_mouse_wheel(0, 120));
    events.push_back(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kReturn, KeyAction::kDown));
    events.push_back(nimrtc::rd_input::make_text("ok"));

    auto encoded = nimrtc::rd_input::encode_batch(events);
    ASSERT_TRUE(encoded.ok());

    auto decoded = nimrtc::rd_input::decode_batch(
        nimrtc::core::ByteSpan{encoded.value().data(), encoded.value().size()});
    ASSERT_TRUE(decoded.ok());
    ASSERT_EQ(decoded.value().size(), events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(decoded.value()[i], events[i]) << "event " << i;
    }
}

TEST(input_event_codec, rejects_an_over_large_batch_count) {
    // A batch whose declared count exceeds the limit must be rejected
    // before the receiver allocates for it.
    std::vector<std::uint8_t> bytes{0xff, 0xff, 0xff, 0xff, 0x0f};
    auto decoded = nimrtc::rd_input::decode_batch(
        nimrtc::core::ByteSpan{bytes.data(), bytes.size()});
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), nimrtc::core::ErrorCode::InvalidArgument);
}

// ===========================================================================
// G4 — the coalescer / rate limiter
// ===========================================================================

TEST(input_coalescer, a_1000hz_pointer_stream_produces_at_most_one_event_per_frame) {
    // G4's acceptance criterion: "A synthetic 1000 Hz pointer stream
    // produces at most one event per display frame".
    nimrtc::rd_input::CoalescerConfig config;
    config.display_hz = 60;
    InputCoalescer coalescer{config};

    // One second of a 1000 Hz pointer.
    constexpr std::uint64_t kSampleIntervalUs = 1000;  // 1 kHz
    constexpr std::size_t kSamples = 1000;

    std::size_t emitted = 0;
    std::uint64_t now_us = 0;
    for (std::size_t i = 0; i < kSamples; ++i) {
        coalescer.push(nimrtc::rd_input::make_mouse_move(1, 0), now_us);
        now_us += kSampleIntervalUs;

        auto out = coalescer.drain(now_us);
        emitted += out.size();

        // The invariant: no more than one pointer event per display
        // interval.
        const std::uint64_t interval = config.frame_interval_us();
        ASSERT_LE(out.size(), 1u) << "more than one event at " << now_us << " us";
        (void)interval;
    }

    // 1000 samples in, at most ~60 events out — the pointer is coalesced
    // to the display rate, not the device rate.
    EXPECT_LE(emitted, 61u) << "the pointer was not coalesced to the display rate";
    EXPECT_GT(emitted, 0u) << "the pointer produced no events at all";
    EXPECT_EQ(coalescer.stats().pointer_events_in, kSamples);
}

TEST(input_coalescer, the_coalesced_pointer_keeps_the_total_motion) {
    nimrtc::rd_input::CoalescerConfig config;
    config.display_hz = 60;
    InputCoalescer coalescer{config};

    std::int32_t total_dx = 0;
    std::int32_t total_dy = 0;
    std::uint64_t now_us = 0;
    for (int i = 0; i < 1000; ++i) {
        coalescer.push(nimrtc::rd_input::make_mouse_move(2, -1), now_us);
        now_us += 1000;
        for (const InputEvent& e : coalescer.drain(now_us)) {
            if (e.type == InputEventType::kMouseMove) {
                total_dx += e.dx;
                total_dy += e.dy;
            }
        }
    }
    // Drain the tail.
    for (const InputEvent& e : coalescer.drain(now_us + 1000000)) {
        if (e.type == InputEventType::kMouseMove) {
            total_dx += e.dx;
            total_dy += e.dy;
        }
    }

    // The pointer still ends up in the same place: coalescing must not
    // lose motion.
    EXPECT_EQ(total_dx, 2000);
    EXPECT_EQ(total_dy, -1000);
}

TEST(input_coalescer, key_events_are_lossless) {
    // G4's acceptance criterion: "key events are lossless (asserted)".
    nimrtc::rd_input::CoalescerConfig config;
    config.display_hz = 60;
    InputCoalescer coalescer{config};

    constexpr std::size_t kKeyEvents = 500;

    std::uint64_t now_us = 0;
    for (std::size_t i = 0; i < kKeyEvents; ++i) {
        // Alternate down / up, with a different key each time.
        const KeyAction action = (i % 2 == 0) ? KeyAction::kDown : KeyAction::kUp;
        coalescer.push(
            nimrtc::rd_input::make_key(static_cast<std::uint32_t>(i), action), now_us);
        // A pointer sample between every key event, so the coalescer has
        // something to coalesce.
        coalescer.push(nimrtc::rd_input::make_mouse_move(1, 1), now_us);
        now_us += 1000;

        auto out = coalescer.drain(now_us);
        for (const InputEvent& e : out) {
            if (e.type == InputEventType::kKey) {
                // The key events come out in order.
                EXPECT_EQ(e.key_code, static_cast<std::uint32_t>(i));
            }
        }
    }

    // Every key event was emitted — none was dropped or merged.
    EXPECT_EQ(coalescer.stats().key_events_in, kKeyEvents);
    EXPECT_EQ(coalescer.stats().essential_events_out, kKeyEvents);
    EXPECT_EQ(coalescer.stats().essential_events_in, kKeyEvents);
}

TEST(input_coalescer, every_key_event_reaches_the_output_in_order) {
    nimrtc::rd_input::CoalescerConfig config;
    config.display_hz = 60;
    InputCoalescer coalescer{config};

    // Push a run of key events and collect them all.
    for (std::uint32_t i = 0; i < 100; ++i) {
        coalescer.push(nimrtc::rd_input::make_key(i, KeyAction::kDown), i);
    }

    std::vector<std::uint32_t> seen;
    for (const InputEvent& e : coalescer.drain(1000000)) {
        if (e.type == InputEventType::kKey) seen.push_back(e.key_code);
    }

    ASSERT_EQ(seen.size(), 100u);
    for (std::uint32_t i = 0; i < 100; ++i) {
        EXPECT_EQ(seen[i], i);
    }
}

TEST(input_coalescer, the_rate_limit_bounds_the_pointer_path) {
    // G4: "the input path has its own rate limit so a stuck device cannot
    // flood the link".
    nimrtc::rd_input::CoalescerConfig config;
    config.display_hz = 1000;             // no display-rate coalescing
    config.max_pointer_events_per_second = 10;
    InputCoalescer coalescer{config};

    // A stuck device reporting at 1000 Hz.  Two full seconds, so that the
    // per-second bound is observed twice rather than at a window edge.
    std::uint64_t now_us = 0;
    std::uint32_t emitted_in_first_second = 0;
    for (int i = 0; i < 2000; ++i) {
        coalescer.push(nimrtc::rd_input::make_mouse_move(1, 1), now_us);
        now_us += 1000;
        for (const InputEvent& e : coalescer.drain(now_us)) {
            if (e.type == InputEventType::kMouseMove && now_us < 1000000u) {
                ++emitted_in_first_second;
            }
        }
    }

    EXPECT_LE(emitted_in_first_second, 10u)
        << "the input path's own rate limit did not bound the pointer";
    EXPECT_GT(coalescer.stats().pointer_events_dropped, 0u)
        << "the surplus pointer events were not accounted for as dropped";
    // Two seconds of a 1000 Hz device, 10 events a second: at most 20 out.
    EXPECT_LE(coalescer.stats().pointer_events_out, 21u);
    EXPECT_EQ(coalescer.stats().pointer_events_in, 2000u);
}

TEST(input_coalescer, reset_discards_the_pending_state) {
    nimrtc::rd_input::CoalescerConfig config;
    config.display_hz = 60;
    InputCoalescer coalescer{config};

    coalescer.push(nimrtc::rd_input::make_mouse_move(5, 5), 1000);
    coalescer.push(nimrtc::rd_input::make_key(1, KeyAction::kDown), 1000);
    coalescer.reset();

    EXPECT_TRUE(coalescer.drain(100000).empty());
}

// ===========================================================================
// G7 — the input safety filter
// ===========================================================================

TEST(input_safety_filter, a_denied_key_never_reaches_the_remote_side) {
    // G7's acceptance criterion: "A denied key never reaches the remote
    // side".
    SafetyFilterConfig config;
    config.denied_keys.insert(nimrtc::rd_input::keys::kMeta);
    config.denied_keys.insert(nimrtc::rd_input::keys::kF4);

    SafetyFilter filter{config};

    const InputEvent denied = nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kMeta, KeyAction::kDown);
    const FilterResult result = filter.check(denied);
    EXPECT_EQ(result.verdict, FilterVerdict::kDeny);
    EXPECT_FALSE(result.allowed());
    EXPECT_FALSE(result.reason.empty());

    // A key that is not denied passes.
    const InputEvent allowed = nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kDown);
    EXPECT_TRUE(filter.check(allowed).allowed());
}

TEST(input_safety_filter, the_denial_is_logged) {
    // G7's acceptance criterion: "the denial is logged".
    auto sink = std::make_shared<CapturingSink>();
    ScopedLogCapture capture{sink};

    SafetyFilterConfig config;
    config.denied_keys.insert(nimrtc::rd_input::keys::kMeta);
    SafetyFilter filter{config};

    const InputEvent denied = nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kMeta, KeyAction::kDown);
    (void)filter.check(denied);

    ASSERT_FALSE(sink->lines().empty()) << "the denial was not logged";
    EXPECT_TRUE(sink->contains("denied"));
    EXPECT_TRUE(sink->contains("key"));

    // An allowed event is not logged as a denial.
    sink->clear();
    (void)filter.check(nimrtc::rd_input::make_key(nimrtc::rd_input::keys::kA, KeyAction::kDown));
    EXPECT_TRUE(sink->lines().empty());
}

TEST(input_safety_filter, the_deny_list_wins_over_the_allow_list) {
    SafetyFilterConfig config;
    config.allowed_keys = {nimrtc::rd_input::keys::kA, nimrtc::rd_input::keys::kMeta};
    config.denied_keys.insert(nimrtc::rd_input::keys::kMeta);
    SafetyFilter filter{config};

    EXPECT_TRUE(filter.check(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kDown)).allowed());
    EXPECT_FALSE(filter.check(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kMeta, KeyAction::kDown)).allowed());
}

TEST(input_safety_filter, a_non_empty_allow_list_denies_everything_else) {
    SafetyFilterConfig config;
    config.allowed_keys.insert(nimrtc::rd_input::keys::kA);
    SafetyFilter filter{config};

    EXPECT_TRUE(filter.check(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kDown)).allowed());
    EXPECT_FALSE(filter.check(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kB, KeyAction::kDown)).allowed());
}

TEST(input_safety_filter, an_empty_allow_list_denies_nothing_by_itself) {
    SafetyFilterConfig config;
    SafetyFilter filter{config};

    EXPECT_TRUE(filter.check(nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kF12, KeyAction::kDown)).allowed());
    EXPECT_TRUE(filter.check(nimrtc::rd_input::make_mouse_move(100, 100)).allowed());
}

TEST(input_safety_filter, an_axis_limit_clamps_the_motion) {
    SafetyFilterConfig config;
    config.dx_range = AxisRange{/*enabled=*/true, /*min=*/-50, /*max=*/50};
    config.dy_range = AxisRange{/*enabled=*/true, /*min=*/-50, /*max=*/50};
    SafetyFilter filter{config};

    InputEvent event = nimrtc::rd_input::make_mouse_move(500, -500);
    FilterResult result;
    EXPECT_TRUE(filter.apply(event, result));
    EXPECT_TRUE(result.clamped);
    EXPECT_EQ(event.dx, 50);
    EXPECT_EQ(event.dy, -50);
}

TEST(input_safety_filter, an_absolute_position_limit_clamps_the_position) {
    SafetyFilterConfig config;
    config.x_range = AxisRange{/*enabled=*/true, /*min=*/0, /*max=*/1920};
    config.y_range = AxisRange{/*enabled=*/true, /*min=*/0, /*max=*/1080};
    SafetyFilter filter{config};

    InputEvent event = nimrtc::rd_input::make_mouse_button(
        MouseButton::kLeft, ButtonAction::kDown, 5000, -20);
    FilterResult result;
    EXPECT_TRUE(filter.apply(event, result));
    EXPECT_EQ(event.x, 1920);
    EXPECT_EQ(event.y, 0);
}

TEST(input_safety_filter, a_denied_button_is_dropped) {
    SafetyFilterConfig config;
    config.denied_buttons.insert(MouseButton::kBack);
    SafetyFilter filter{config};

    EXPECT_FALSE(filter.check(nimrtc::rd_input::make_mouse_button(
        MouseButton::kBack, ButtonAction::kDown)).allowed());
    EXPECT_TRUE(filter.check(nimrtc::rd_input::make_mouse_button(
        MouseButton::kLeft, ButtonAction::kDown)).allowed());
}

TEST(input_safety_filter, the_deny_observer_is_notified) {
    int notifications = 0;
    SafetyFilterConfig config;
    config.denied_keys.insert(nimrtc::rd_input::keys::kMeta);
    config.on_deny = [&notifications](const InputEvent&, const std::string& reason) {
        ++notifications;
        EXPECT_FALSE(reason.empty());
    };
    SafetyFilter filter{config};

    (void)filter.check(nimrtc::rd_input::make_key(nimrtc::rd_input::keys::kMeta, KeyAction::kDown));
    EXPECT_EQ(notifications, 1);

    (void)filter.check(nimrtc::rd_input::make_key(nimrtc::rd_input::keys::kA, KeyAction::kDown));
    EXPECT_EQ(notifications, 1);
}
