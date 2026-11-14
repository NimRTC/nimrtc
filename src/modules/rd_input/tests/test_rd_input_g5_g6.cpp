// src/modules/rd_input/tests/test_rd_input_g5_g6.cpp
// ----------------------------------------------------------------------------
// The input <-> frame binding (G5) and the dead-man switch / heartbeat (G6).
//
// These are the two pieces of Track G that are *policy* rather than
// transport:
//
//   - G5: an outgoing event carries the id of the frame the operator was
//     looking at, and the wire format carries that id when — and only
//     when — the sender asks for it.
//   - G6: the remote side notices when the operator stops (the heartbeat
//     times out) or lets go (the dead-man switch is released), and enters
//     the configured safe state exactly once per transition.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>

#include "nimrtc/rd_input/deadman_watchdog.hpp"
#include "nimrtc/rd_input/event_codec.hpp"
#include "nimrtc/rd_input/input_event.hpp"
#include "nimrtc/rd_input/ref_frame_binder.hpp"

namespace nimrtc::rd_input {
namespace {

// Decode a span from an encoded buffer.
core::ByteSpan span_of(const std::vector<std::uint8_t>& bytes) {
    return core::ByteSpan{bytes.data(), bytes.size()};
}

// ---------------------------------------------------------------------------
// G5 — the ref-frame id on the wire
// ---------------------------------------------------------------------------

TEST(RdInputG5, RefFrameSurvivesTheRoundTrip) {
    InputEvent event = make_mouse_button(MouseButton::kLeft, ButtonAction::kDown, 100, 200);
    event.ref_frame = 4242u;

    EncodeOptions options;
    options.include_ref_frame = true;

    const auto encoded = encode_event(event, options);
    ASSERT_TRUE(encoded.ok()) << encoded.error().message();

    const auto decoded = decode_event(span_of(encoded.value()));
    ASSERT_TRUE(decoded.ok()) << decoded.error().message();
    EXPECT_TRUE(decoded.value().ref_frame.has_value());
    EXPECT_EQ(*decoded.value().ref_frame, 4242u);
}

TEST(RdInputG5, RefFrameIsAbsentByDefault) {
    InputEvent event = make_mouse_move(3, -4);
    event.ref_frame = 7u;

    // The default options do not ask for the ref-frame id: the event
    // carries one, but the wire format must not carry it.  This is the
    // zero-cost property that keeps the latency-critical path small.
    const auto encoded = encode_event(event);
    ASSERT_TRUE(encoded.ok());

    const auto decoded = decode_event(span_of(encoded.value()));
    ASSERT_TRUE(decoded.ok());
    EXPECT_FALSE(decoded.value().ref_frame.has_value());
}

TEST(RdInputG5, RefFrameCostsNothingWhenThereIsNoId) {
    InputEvent event = make_mouse_move(3, -4);
    // No id on the event.
    EncodeOptions without;
    EncodeOptions with;
    with.include_ref_frame = true;

    const auto a = encode_event(event, without);
    const auto b = encode_event(event, with);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().size(), b.value().size());
}

TEST(RdInputG5, RefFrameAndTimestampCoexist) {
    InputEvent event = make_key(keys::kA, KeyAction::kDown);
    event.timestamp_us = 123456789u;
    event.ref_frame = 99u;

    EncodeOptions options;
    options.include_timestamp = true;
    options.include_ref_frame = true;

    const auto encoded = encode_event(event, options);
    ASSERT_TRUE(encoded.ok());

    const auto decoded = decode_event(span_of(encoded.value()));
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().timestamp_us, 123456789u);
    ASSERT_TRUE(decoded.value().ref_frame.has_value());
    EXPECT_EQ(*decoded.value().ref_frame, 99u);
}

TEST(RdInputG5, RefFrameSurvivesTheBatchRoundTrip) {
    std::vector<InputEvent> events;
    InputEvent move = make_mouse_move(1, 2);
    move.ref_frame = 11u;
    InputEvent key = make_key(keys::kEnter, KeyAction::kDown);
    key.ref_frame = 12u;
    events.push_back(move);
    events.push_back(key);

    EncodeOptions options;
    options.include_ref_frame = true;

    const auto encoded = encode_batch(events, options);
    ASSERT_TRUE(encoded.ok());

    const auto decoded = decode_batch(span_of(encoded.value()));
    ASSERT_TRUE(decoded.ok());
    ASSERT_EQ(decoded.value().size(), 2u);
    ASSERT_TRUE(decoded.value()[0].ref_frame.has_value());
    EXPECT_EQ(*decoded.value()[0].ref_frame, 11u);
    ASSERT_TRUE(decoded.value()[1].ref_frame.has_value());
    EXPECT_EQ(*decoded.value()[1].ref_frame, 12u);
}

TEST(RdInputG5, EncodedSizeAccountsForTheRefFrame) {
    InputEvent event = make_mouse_move(1, 2);
    event.ref_frame = 1000u;

    EncodeOptions options;
    options.include_ref_frame = true;

    const auto encoded = encode_event(event, options);
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(encoded.value().size(), encoded_size(event, options));
}

// ---------------------------------------------------------------------------
// G5 — the version / flag extension's compatibility contract
// ---------------------------------------------------------------------------

TEST(RdInputG5, AnUnknownFlagBitIsIgnored) {
    // The flags byte is the extension point.  A decoder that does not
    // know a bit **ignores** it (the append-only-friendly answer the
    // header documents) — the event still decodes.
    //
    // The header also documents the honest limit: a decoder that does
    // not know a bit whose *field* is present cannot skip that field,
    // so the version byte remains the strong guard.  What this test
    // pins is the "ignore the unknown bit" half.
    InputEvent event = make_mouse_move(3, -4);
    const auto encoded = encode_event(event);
    ASSERT_TRUE(encoded.ok());
    ASSERT_GE(encoded.value().size(), 3u);

    // Set an unknown bit (bit 7) in the flags byte.
    std::vector<std::uint8_t> mutated = encoded.value();
    mutated[2] |= 0x80u;

    const auto decoded = decode_event(span_of(mutated));
    ASSERT_TRUE(decoded.ok()) << decoded.error().message();
    EXPECT_EQ(decoded.value().dx, 3);
    EXPECT_EQ(decoded.value().dy, -4);
    EXPECT_FALSE(decoded.value().ref_frame.has_value());
}

TEST(RdInputG5, ANewerVersionIsRejectedUnlessTheDecoderOptsIn) {
    // The version byte is the strong guard: an event from a future
    // format is refused, not guessed at.
    InputEvent event = make_mouse_move(1, 1);

    EncodeOptions options;
    options.version = 99;   // a version this build does not know

    const auto encoded = encode_event(event, options);
    ASSERT_TRUE(encoded.ok());

    const auto strict = decode_event(span_of(encoded.value()));
    EXPECT_FALSE(strict.ok());
    EXPECT_EQ(strict.error().code(), nimrtc::core::ErrorCode::ProtocolError);

    // A decoder that explicitly opts in parses it under the current
    // rules.
    DecodeOptions permissive;
    permissive.allow_newer_version = true;
    const auto lenient = decode_event(span_of(encoded.value()), permissive);
    ASSERT_TRUE(lenient.ok()) << lenient.error().message();
    EXPECT_EQ(lenient.value().dx, 1);
}

// ---------------------------------------------------------------------------
// G5 — the binder
// ---------------------------------------------------------------------------

TEST(RdInputG5, BinderStampsTheCurrentId) {
    std::uint64_t current = 5u;
    RefFrameBinder binder{[&current]() -> std::optional<std::uint64_t> { return current; }};

    InputEvent event = make_mouse_move(1, 1);
    EXPECT_TRUE(binder.stamp(event));
    ASSERT_TRUE(event.ref_frame.has_value());
    EXPECT_EQ(*event.ref_frame, 5u);

    current = 6u;
    const InputEvent stamped = binder.stamped(make_mouse_move(2, 2));
    ASSERT_TRUE(stamped.ref_frame.has_value());
    EXPECT_EQ(*stamped.ref_frame, 6u);
}

TEST(RdInputG5, BinderClearsAStaleIdWhenThereIsNoFrame) {
    bool have_frame = true;
    RefFrameBinder binder{[&have_frame]() -> std::optional<std::uint64_t> {
        if (have_frame) return std::uint64_t{77u};
        return std::nullopt;
    }};

    InputEvent event = make_mouse_move(1, 1);
    EXPECT_TRUE(binder.stamp(event));
    ASSERT_TRUE(event.ref_frame.has_value());

    have_frame = false;
    EXPECT_FALSE(binder.stamp(event));
    EXPECT_FALSE(event.ref_frame.has_value());
}

TEST(RdInputG5, BinderWithoutAProviderIsHarmless) {
    RefFrameBinder binder{nullptr};
    EXPECT_FALSE(binder.has_provider());
    InputEvent event = make_mouse_move(1, 1);
    event.ref_frame = 3u;
    EXPECT_FALSE(binder.stamp(event));
    EXPECT_FALSE(event.ref_frame.has_value());
}

// ---------------------------------------------------------------------------
// G6 — the dead-man switch and the heartbeat
// ---------------------------------------------------------------------------

TEST(RdInputG6, HeartbeatTimeoutEntersTheSafeState) {
    DeadManConfig config;
    config.heartbeat_timeout_ms = 1000;

    int safe_calls = 0;
    DeadManState reported = DeadManState::kAlive;
    DeadManWatchdog watchdog{config,
                             [&](DeadManState s) { ++safe_calls; reported = s; }};

    watchdog.note_heartbeat(0);
    EXPECT_TRUE(watchdog.alive());

    // Just before the timeout: still alive.
    EXPECT_FALSE(watchdog.check_timeout(999'000));
    EXPECT_TRUE(watchdog.alive());
    EXPECT_EQ(safe_calls, 0);

    // Past the timeout: the safe state is entered, once.
    EXPECT_TRUE(watchdog.check_timeout(1'000'001));
    EXPECT_FALSE(watchdog.alive());
    EXPECT_EQ(safe_calls, 1);
    EXPECT_EQ(reported, DeadManState::kTimedOut);
    EXPECT_EQ(watchdog.state(), DeadManState::kTimedOut);

    // A second check does not fire the callback again.
    EXPECT_FALSE(watchdog.check_timeout(2'000'000));
    EXPECT_EQ(safe_calls, 1);
    EXPECT_EQ(watchdog.safe_state_entries(), 1u);
}

TEST(RdInputG6, AHeartbeatRecoversTheSession) {
    DeadManConfig config;
    config.heartbeat_timeout_ms = 1000;

    int safe_calls = 0;
    int recovery_calls = 0;
    DeadManWatchdog watchdog{config,
                             [&](DeadManState) { ++safe_calls; },
                             [&]() { ++recovery_calls; }};

    watchdog.note_heartbeat(0);
    EXPECT_TRUE(watchdog.check_timeout(2'000'000));
    EXPECT_EQ(safe_calls, 1);

    watchdog.note_heartbeat(2'000'000);
    EXPECT_TRUE(watchdog.alive());
    EXPECT_EQ(recovery_calls, 1);
    EXPECT_EQ(watchdog.recoveries(), 1u);
}

TEST(RdInputG6, TheDeadManReleaseEntersTheSafeStateImmediately) {
    DeadManConfig config;
    config.heartbeat_timeout_ms = 1000;
    config.deadman_enabled = true;
    config.deadman_button = MouseButton::kLeft;

    int safe_calls = 0;
    DeadManState reported = DeadManState::kAlive;
    DeadManWatchdog watchdog{config,
                             [&](DeadManState s) { ++safe_calls; reported = s; }};

    watchdog.note_heartbeat(0);
    watchdog.observe(make_mouse_button(MouseButton::kLeft, ButtonAction::kUp, 0, 0), 100);

    EXPECT_FALSE(watchdog.alive());
    EXPECT_EQ(safe_calls, 1);
    EXPECT_EQ(reported, DeadManState::kReleased);
    EXPECT_EQ(watchdog.state(), DeadManState::kReleased);

    // A release is deliberate: a heartbeat does NOT clear it.
    watchdog.note_heartbeat(200);
    EXPECT_FALSE(watchdog.alive());
    EXPECT_EQ(safe_calls, 1);

    // An arbitrary move does NOT clear it either — the safe state is
    // sticky, so a stray mouse move cannot silently resume the session.
    watchdog.note_activity(250);
    EXPECT_FALSE(watchdog.alive());
    EXPECT_EQ(safe_calls, 1);

    // Pressing the switch again re-arms it.
    watchdog.observe(make_mouse_button(MouseButton::kLeft, ButtonAction::kDown, 0, 0), 300);
    EXPECT_TRUE(watchdog.alive());
}

TEST(RdInputG6, ADeadManReleaseOnAnotherButtonIsJustActivity) {
    DeadManConfig config;
    config.deadman_enabled = true;
    config.deadman_button = MouseButton::kLeft;

    int safe_calls = 0;
    DeadManWatchdog watchdog{config, [&](DeadManState) { ++safe_calls; }};

    watchdog.note_heartbeat(0);
    watchdog.observe(make_mouse_button(MouseButton::kRight, ButtonAction::kUp, 0, 0), 100);
    EXPECT_TRUE(watchdog.alive());
    EXPECT_EQ(safe_calls, 0);
}

TEST(RdInputG6, ActivityKeepsTheSessionAlive) {
    DeadManConfig config;
    config.heartbeat_timeout_ms = 1000;

    DeadManWatchdog watchdog{config};
    watchdog.note_heartbeat(0);

    // Typing is activity: the heartbeat timeout must not fire.
    for (std::uint64_t t = 100'000; t < 900'000; t += 100'000) {
        watchdog.observe(make_key(keys::kA, KeyAction::kDown), t);
        EXPECT_FALSE(watchdog.check_timeout(t));
    }
    EXPECT_TRUE(watchdog.alive());
}

TEST(RdInputG6, TheTimeoutCanBeDisabled) {
    DeadManConfig config;
    config.heartbeat_timeout_ms = 0;

    DeadManWatchdog watchdog{config};
    watchdog.note_heartbeat(0);
    EXPECT_FALSE(watchdog.check_timeout(10'000'000));
    EXPECT_TRUE(watchdog.alive());
}

TEST(RdInputG6, SilenceIsReported) {
    DeadManConfig config;
    config.heartbeat_timeout_ms = 1000;

    DeadManWatchdog watchdog{config};
    watchdog.note_heartbeat(1'000);
    EXPECT_EQ(watchdog.silence_us(1'500), 500u);
    EXPECT_EQ(watchdog.silence_us(1'000), 0u);
}

TEST(RdInputG6, ResetClearsTheState) {
    DeadManConfig config;
    config.heartbeat_timeout_ms = 1000;

    DeadManWatchdog watchdog{config};
    watchdog.note_heartbeat(0);
    EXPECT_TRUE(watchdog.check_timeout(2'000'000));
    ASSERT_FALSE(watchdog.alive());

    watchdog.reset();
    EXPECT_TRUE(watchdog.alive());
    EXPECT_EQ(watchdog.safe_state_entries(), 0u);
    EXPECT_EQ(watchdog.recoveries(), 0u);
}

// ---------------------------------------------------------------------------
// G6 — the heartbeat emitter
// ---------------------------------------------------------------------------

TEST(RdInputG6, TheEmitterBeatsOnItsInterval) {
    HeartbeatEmitter emitter{250};
    EXPECT_FALSE(emitter.disabled());

    // The first beat is always due.
    EXPECT_TRUE(emitter.due(0));
    emitter.on_sent(0);
    EXPECT_EQ(emitter.beats_sent(), 1u);

    EXPECT_FALSE(emitter.due(100'000));
    EXPECT_TRUE(emitter.due(250'000));
    emitter.on_sent(250'000);
    EXPECT_EQ(emitter.beats_sent(), 2u);
}

TEST(RdInputG6, TheEmitterCanBeDisabled) {
    HeartbeatEmitter emitter{0};
    EXPECT_TRUE(emitter.disabled());
    EXPECT_FALSE(emitter.due(0));
    EXPECT_FALSE(emitter.due(1'000'000));
}

TEST(RdInputG6, TheEmitterResets) {
    HeartbeatEmitter emitter{250};
    EXPECT_TRUE(emitter.due(0));
    emitter.on_sent(0);
    emitter.reset();
    EXPECT_EQ(emitter.beats_sent(), 0u);
    EXPECT_TRUE(emitter.due(0));
}

} // namespace
} // namespace nimrtc::rd_input
