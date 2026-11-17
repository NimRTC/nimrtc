/**
 * @file src/modules/rd_input_inject/tests/test_rd_input_inject.cpp
 * @brief The input-injection module's unit tests.
 *
 * ## What is tested here, and what is not
 *
 * The **portable** half of the module is tested here:
 *
 *   - the key layout: which vocabulary codes it knows, and which it
 *     refuses;
 *   - the injection state machine: which keys / buttons the injector
 *     believes are down, and how the auto-repeat depth is tracked;
 *   - the release-everything path: the dead-man switch's safe state.
 *
 * The **platform** half is not tested here, because it needs a real
 * desktop session, a real focus and a real input stack — a unit test
 * that injects a keystroke into a CI runner's session 0 proves
 * nothing and would be flaky.  The platform backends are exercised by
 * the example application (`examples/remote-desktop/`).
 *
 * The platform half is tested *indirectly* here, though: the test uses
 * a **recording injector** (a subclass of the module's own base class)
 * so the dispatch, the state machine and the modifier bookkeeping are
 * all exercised without touching an OS.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/rd_input_inject/input_injector.hpp>
#include <nimrtc/rd_input_inject/key_map.hpp>
#include <nimrtc/rd_input/input_event.hpp>

// The macOS keycode table is portable data (see ../src/macos_keycodes.hpp),
// so it is reachable -- and testable -- on every platform.
#include "../src/macos_keycodes.hpp"

// The Linux input-layer codes and the X11 keysyms are portable data
// too (see ../src/linux_input_codes.hpp), for the same reason: the
// Linux backend's two injection vocabularies must be verifiable on
// every CI leg, not only on a Linux one.
#include "../src/linux_input_codes.hpp"

// The base class is not exported through the public header, so the
// test reaches it by a relative path.  That is deliberate: the base
// class is an implementation detail, and a test that only used the
// public seam could not prove the state machine's behaviour.
#include "../src/injector_base.hpp"

// The macOS virtual-keycode table is portable data (see
// ../src/macos_keycodes.hpp), so the test reaches it by a relative
// path -- the same way it reaches the base class.  The table is
// verified on EVERY CI leg, including the ones where the macOS
// backend itself is never compiled.
#include "../src/macos_keycodes.hpp"

namespace {

using nimrtc::rd_input::InputEvent;
using nimrtc::rd_input::KeyAction;
using nimrtc::rd_input::ButtonAction;
using nimrtc::rd_input::MouseButton;
namespace inj = nimrtc::rd_input_inject;
namespace rdi = nimrtc::rd_input_inject::detail;

// ---------------------------------------------------------------------------
// A recording injector
//
// It derives from the module's *internal* base class, so the test can
// observe exactly what the base class would have asked the platform to
// do — without an OS.
// ---------------------------------------------------------------------------

/** One recorded OS call. */
struct RecordedCall {
    enum class Kind {
        kMouseMove,
        kButton,
        kWheel,
        kKey,
        kText,
    };

    Kind kind = Kind::kMouseMove;
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    inj::NeutralKey key = inj::NeutralKey::kNone;
    bool down = false;
    MouseButton button = MouseButton::kUnknown;
    std::string text;
};

/** The injector that records instead of injecting. */
class RecordingInjector final : public rdi::InjectorBase {
public:
    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "recording";
    }

    [[nodiscard]] bool available() const noexcept override { return true; }

    [[nodiscard]] std::string unavailable_reason() const noexcept override {
        return {};
    }

    const std::vector<RecordedCall>& calls() const noexcept { return calls_; }

    void clear() { calls_.clear(); }

protected:
    bool do_inject_mouse_move(std::int32_t dx, std::int32_t dy) override {
        // The aggregate is built field by field rather than with a brace
        // list: a brace list would leave `text` uninitialized, which
        // GCC's `-Wmissing-field-initializers` (fatal under `-Werror`)
        // rejects.
        RecordedCall call;
        call.kind = RecordedCall::Kind::kMouseMove;
        call.dx = dx;
        call.dy = dy;
        calls_.push_back(std::move(call));
        return true;
    }

    bool do_inject_button(MouseButton button, ButtonAction action) override {
        RecordedCall call;
        call.kind = RecordedCall::Kind::kButton;
        call.button = button;
        call.down = (action == ButtonAction::kDown);
        calls_.push_back(std::move(call));
        return true;
    }

    bool do_inject_wheel(std::int32_t dx, std::int32_t dy) override {
        RecordedCall call;
        call.kind = RecordedCall::Kind::kWheel;
        call.dx = dx;
        call.dy = dy;
        calls_.push_back(std::move(call));
        return true;
    }

    bool do_inject_key(inj::NeutralKey key, KeyAction action) override {
        RecordedCall call;
        call.kind = RecordedCall::Kind::kKey;
        call.key = key;
        call.down = (action == KeyAction::kDown);
        calls_.push_back(std::move(call));
        return true;
    }

    bool do_inject_text(const std::string& text) override {
        RecordedCall call;
        call.kind = RecordedCall::Kind::kText;
        call.text = text;
        calls_.push_back(std::move(call));
        return true;
    }

private:
    std::vector<RecordedCall> calls_;
};

/** Count the recorded calls of one kind. */
std::size_t count_kind(const std::vector<RecordedCall>& calls,
                       RecordedCall::Kind kind) {
    std::size_t n = 0;
    for (const RecordedCall& call : calls) {
        if (call.kind == kind) ++n;
    }
    return n;
}

} // namespace

// ===========================================================================
// 1. The key layout
// ===========================================================================

TEST(RdInputInject, KeyLayoutKnowsTheVocabularyCodes) {
    // Every code the vocabulary's `keys::` namespace names must be in
    // the layout -- otherwise the two halves would disagree about what
    // "the A key" is.
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kEscape));
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kTab));
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kEnter));
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kShift));
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kControl));
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kAlt));
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kMeta));
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kF1));
    EXPECT_TRUE(inj::is_neutral_key_known(nimrtc::rd_input::keys::kF12));

    for (char c = 'A'; c <= 'Z'; ++c) {
        const std::uint32_t code = nimrtc::rd_input::keys::kA +
                                    static_cast<std::uint32_t>(c - 'A');
        EXPECT_TRUE(inj::is_neutral_key_known(code))
            << "the letter '" << c << "' is not in the layout";
    }
}

TEST(RdInputInject, KeyLayoutRefusesAnUnknownCode) {
    // A code the layout does not know must be refused, never guessed.
    EXPECT_FALSE(inj::is_neutral_key_known(0));
    EXPECT_FALSE(inj::is_neutral_key_known(999));
    EXPECT_FALSE(inj::is_neutral_key_known(0xFFFFFFFFu));

    EXPECT_EQ(inj::neutral_key_of(999), std::nullopt);
    EXPECT_EQ(inj::neutral_key_of(nimrtc::rd_input::keys::kA),
              inj::NeutralKey::kA);
}

TEST(RdInputInject, KeyLayoutNamesEveryKey) {
    // Every key must have a name -- a name is what a host prints in a
    // diagnostic, and a nameless key would print as an empty string.
    for (std::uint32_t code = 1; code < 200; ++code) {
        const auto key = inj::neutral_key_of(code);
        if (!key.has_value()) continue;
        EXPECT_FALSE(inj::neutral_key_name(*key).empty())
            << "the key with code " << code << " has no name";
    }
}

// ===========================================================================
// 2. The injection state machine
// ===========================================================================

TEST(RdInputInject, KeyPressAndReleaseAreTracked) {
    RecordingInjector injector;

    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_key(
                  nimrtc::rd_input::keys::kA, KeyAction::kDown)),
              inj::InjectionResult::kInjected);
    EXPECT_EQ(injector.keys_down(), 1u);

    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_key(
                  nimrtc::rd_input::keys::kA, KeyAction::kUp)),
              inj::InjectionResult::kInjected);
    EXPECT_EQ(injector.keys_down(), 0u);

    const auto stats = injector.stats();
    EXPECT_EQ(stats.injected, 2u);
    EXPECT_EQ(stats.refused, 0u);
    EXPECT_EQ(stats.failed, 0u);
}

TEST(RdInputInject, AnUnknownKeyIsRefusedAndChangesNoState) {
    RecordingInjector injector;

    // A code the layout does not know.
    const auto result =
        injector.inject(nimrtc::rd_input::make_key(4242, KeyAction::kDown));

    EXPECT_EQ(result, inj::InjectionResult::kRefused);
    EXPECT_EQ(injector.keys_down(), 0u);
    EXPECT_TRUE(injector.calls().empty());
    EXPECT_EQ(injector.stats().refused, 1u);
}

TEST(RdInputInject, AButtonPressAndReleaseAreTracked) {
    RecordingInjector injector;

    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_mouse_button(
                  MouseButton::kLeft, ButtonAction::kDown, 10, 20)),
              inj::InjectionResult::kInjected);
    EXPECT_EQ(injector.buttons_down(), 1u);

    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_mouse_button(
                  MouseButton::kLeft, ButtonAction::kUp, 10, 20)),
              inj::InjectionResult::kInjected);
    EXPECT_EQ(injector.buttons_down(), 0u);
}

TEST(RdInputInject, AnUnknownButtonIsRefused) {
    RecordingInjector injector;

    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_mouse_button(
                  MouseButton::kUnknown, ButtonAction::kDown)),
              inj::InjectionResult::kRefused);
    EXPECT_EQ(injector.buttons_down(), 0u);
}

// ===========================================================================
// 3. The modifier bookkeeping
// ===========================================================================

TEST(RdInputInject, AModifierIsPressedAroundTheKey) {
    RecordingInjector injector;

    // A key event that carries the Shift modifier.
    InputEvent event = nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kDown,
        static_cast<std::uint32_t>(nimrtc::rd_input::Modifier::kShift));

    EXPECT_EQ(injector.inject(event), inj::InjectionResult::kInjected);

    // The sequence is: Shift down, the key down, Shift up.
    ASSERT_EQ(injector.calls().size(), 3u);
    EXPECT_EQ(injector.calls()[0].key, inj::NeutralKey::kShift);
    EXPECT_TRUE(injector.calls()[0].down);
    EXPECT_EQ(injector.calls()[1].key, inj::NeutralKey::kA);
    EXPECT_TRUE(injector.calls()[1].down);
    EXPECT_EQ(injector.calls()[2].key, inj::NeutralKey::kShift);
    EXPECT_FALSE(injector.calls()[2].down);
}

TEST(RdInputInject, AModifierThatIsAlreadyHeldIsNotReleased) {
    RecordingInjector injector;

    // The operator holds Shift down (its own key event).
    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_key(
                  nimrtc::rd_input::keys::kShift, KeyAction::kDown)),
              inj::InjectionResult::kInjected);
    EXPECT_EQ(injector.keys_down(), 1u);

    injector.clear();

    // Now a key event that *mentions* Shift.  The injector must not
    // press it again (and then release it), or the operator's Shift
    // would be released by a keystroke that merely mentioned it.
    InputEvent event = nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kDown,
        static_cast<std::uint32_t>(nimrtc::rd_input::Modifier::kShift));

    EXPECT_EQ(injector.inject(event), inj::InjectionResult::kInjected);

    // Only the key itself was injected.
    ASSERT_EQ(injector.calls().size(), 1u);
    EXPECT_EQ(injector.calls()[0].key, inj::NeutralKey::kA);
    EXPECT_TRUE(injector.calls()[0].down);

    // And the Shift is still held.
    EXPECT_EQ(injector.keys_down(), 2u);
}

TEST(RdInputInject, TheMetaModifierIsNotSynthesised) {
    RecordingInjector injector;

    // The Meta (Windows / Command / Super) key opens the shell.  A
    // remote-desktop agent that can press it can lock the operator out
    // of the machine they are driving, so it is never synthesised.
    InputEvent event = nimrtc::rd_input::make_key(
        nimrtc::rd_input::keys::kA, KeyAction::kDown,
        static_cast<std::uint32_t>(nimrtc::rd_input::Modifier::kMeta));

    EXPECT_EQ(injector.inject(event), inj::InjectionResult::kInjected);

    // Only the key itself -- no Meta press and no Meta release.
    ASSERT_EQ(injector.calls().size(), 1u);
    EXPECT_EQ(injector.calls()[0].key, inj::NeutralKey::kA);
}

// ===========================================================================
// 4. The safe state (the dead-man switch's release-everything path)
// ===========================================================================

TEST(RdInputInject, ReleaseAllReleasesEveryKeyAndButton) {
    RecordingInjector injector;

    injector.inject(nimrtc::rd_input::make_key(nimrtc::rd_input::keys::kA,
                                                 KeyAction::kDown));
    injector.inject(nimrtc::rd_input::make_key(nimrtc::rd_input::keys::kShift,
                                                 KeyAction::kDown));
    injector.inject(nimrtc::rd_input::make_mouse_button(
        MouseButton::kLeft, ButtonAction::kDown));

    ASSERT_EQ(injector.keys_down(), 2u);
    ASSERT_EQ(injector.buttons_down(), 1u);

    injector.clear();
    const std::size_t released = injector.release_all();

    // Two keys and one button.
    EXPECT_EQ(released, 3u);
    EXPECT_EQ(count_kind(injector.calls(), RecordedCall::Kind::kKey), 2u);
    EXPECT_EQ(count_kind(injector.calls(), RecordedCall::Kind::kButton), 1u);

    // And the state is empty afterwards.
    EXPECT_EQ(injector.keys_down(), 0u);
    EXPECT_EQ(injector.buttons_down(), 0u);
}

TEST(RdInputInject, ReleaseAllIsIdempotent) {
    RecordingInjector injector;

    injector.inject(nimrtc::rd_input::make_key(nimrtc::rd_input::keys::kA,
                                                 KeyAction::kDown));

    EXPECT_EQ(injector.release_all(), 1u);
    // The second call has nothing to release.
    EXPECT_EQ(injector.release_all(), 0u);
}

// ===========================================================================
// 5. The pointer
// ===========================================================================

TEST(RdInputInject, ThePointerFollowsTheMotion) {
    RecordingInjector injector;

    injector.inject(nimrtc::rd_input::make_mouse_move(10, -5));
    EXPECT_EQ(injector.pointer_x(), 10);
    EXPECT_EQ(injector.pointer_y(), -5);

    injector.inject(nimrtc::rd_input::make_mouse_move(3, 4));
    EXPECT_EQ(injector.pointer_x(), 13);
    EXPECT_EQ(injector.pointer_y(), -1);
}

TEST(RdInputInject, AZeroMotionIsANoOp) {
    RecordingInjector injector;

    // The coalescer may legitimately produce a zero motion (the mouse
    // did not move between two polls), so it is not a refusal.
    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_mouse_move(0, 0)),
              inj::InjectionResult::kInjected);
    EXPECT_TRUE(injector.calls().empty());
    EXPECT_EQ(injector.stats().refused, 0u);
}

// ===========================================================================
// 6. The text path
// ===========================================================================

TEST(RdInputInject, TextIsForwardedAsOneCall) {
    RecordingInjector injector;

    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_text("hello")),
              inj::InjectionResult::kInjected);
    ASSERT_EQ(injector.calls().size(), 1u);
    EXPECT_EQ(injector.calls()[0].kind, RecordedCall::Kind::kText);
    EXPECT_EQ(injector.calls()[0].text, "hello");
}

TEST(RdInputInject, EmptyTextIsANoOp) {
    RecordingInjector injector;

    EXPECT_EQ(injector.inject(nimrtc::rd_input::make_text("")),
              inj::InjectionResult::kInjected);
    EXPECT_TRUE(injector.calls().empty());
}

// ===========================================================================
// 7. The factory
// ===========================================================================

TEST(RdInputInject, TheFactoryAlwaysReturnsAnInjector) {
    // A host that asks for an injector must never get a null pointer --
    // it gets a backend whose `available()` tells the truth.
    std::unique_ptr<inj::InputInjector> injector(inj::create_input_injector());
    ASSERT_NE(injector, nullptr);

    EXPECT_FALSE(injector->backend_name().empty());
    EXPECT_FALSE(inj::default_injector_backend_name().empty());

    // The availability and the reason must agree: an available backend
    // has no reason, and an unavailable one has one.
    if (injector->available()) {
        EXPECT_TRUE(injector->unavailable_reason().empty());
    } else {
        EXPECT_FALSE(injector->unavailable_reason().empty());
    }
}

TEST(RdInputInject, TheStubRefusesEveryEvent) {
    // The stub never reaches an OS, so it must never claim to have
    // injected anything.
    std::unique_ptr<inj::InputInjector> injector(inj::create_input_injector());
    ASSERT_NE(injector, nullptr);

    if (injector->available()) {
        // This host has a real backend -- the stub's behaviour is not
        // observable here, and injecting into the CI runner's session
        // would be wrong anyway.
        GTEST_SKIP() << "this host has a real injection backend";
    }

    EXPECT_EQ(injector->inject(nimrtc::rd_input::make_mouse_move(1, 1)),
              inj::InjectionResult::kRefused);
    EXPECT_EQ(injector->stats().refused, 1u);
    EXPECT_EQ(injector->release_all(), 0u);
}

// ===========================================================================
// 8. The macOS virtual-keycode table
// ===========================================================================
//
// The table is *portable data* (see ../../macos_keycodes.hpp), so it is
// verified on EVERY CI leg -- including the Windows and Linux ones,
// where the macOS backend itself is never compiled.  That is the point:
// a keycode table that could only be checked on an Apple machine would
// rot silently between two Apple machines.
//
// The expected values are the platform's frozen ANSI-layout hardware
// key positions.  They are spelled out literally here (rather than
// re-derived from the table) so that a wrong *entry* in the table is
// caught, not merely a change.

TEST(RdInputInject, TheMacosKeycodeTableIsCorrect) {
    using namespace nimrtc::rd_input_inject::detail;

    // The letters.
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kA), 0x00);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kS), 0x01);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kD), 0x02);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF), 0x03);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kH), 0x04);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kG), 0x05);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kZ), 0x06);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kX), 0x07);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kC), 0x08);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kV), 0x09);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kB), 0x0B);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kQ), 0x0C);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kW), 0x0D);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kE), 0x0E);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kR), 0x0F);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kY), 0x10);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kT), 0x11);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kO), 0x1F);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kU), 0x20);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kI), 0x22);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kP), 0x23);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kL), 0x25);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kJ), 0x26);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kK), 0x28);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kN), 0x2D);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kM), 0x2E);

    // The editing / navigation keys.
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kEnter), 0x24);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kTab), 0x30);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kSpace), 0x31);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kBackspace), 0x33);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kDelete), 0x75);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kEscape), 0x35);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kHome), 0x73);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kPageUp), 0x74);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kEnd), 0x77);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kPageDown), 0x79);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kLeft), 0x7B);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kRight), 0x7C);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kDown), 0x7D);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kUp), 0x7E);

    // The modifiers.  On macOS the "Alt" key is the Option key and the
    // "Meta" key is the Command key.
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kMeta), 0x37);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kShift), 0x38);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kAlt), 0x3A);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kControl), 0x3B);

    // The function keys.
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF5), 0x60);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF6), 0x61);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF7), 0x62);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF3), 0x63);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF8), 0x64);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF9), 0x65);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF11), 0x67);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF10), 0x6D);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF12), 0x6F);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF4), 0x76);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF2), 0x78);
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kF1), 0x7A);
}

TEST(RdInputInject, TheMacosKeycodeTableRefusesTheUnknownKey) {
    using namespace nimrtc::rd_input_inject::detail;

    // The sentinel is 0xFFFF, NOT 0 -- 0x00 is the real code for 'A'.
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kNone), 0xFFFF);
    EXPECT_EQ(kMacVKUnknown, 0xFFFF);

    // And 'A' -- the key whose code is 0 -- must NOT be mistaken for
    // the "unknown" sentinel.  This is the one case where a table that
    // used 0 as its sentinel would silently refuse a valid key.
    EXPECT_EQ(macos_keycode_for(inj::NeutralKey::kA), 0x00);
    EXPECT_NE(macos_keycode_for(inj::NeutralKey::kA), kMacVKUnknown);
}

// ===========================================================================
// 9. The Linux input codes and the X11 keysyms
// ===========================================================================
//
// The Linux backend has two injection vocabularies, and they are
// unrelated:
//
//   - the uinput path speaks the *kernel's* codes (`KEY_A` is 30,
//     `BTN_LEFT` is 0x110);
//   - the XTest path speaks the *X server's* (`XK_a` is 0x61, and the
//     left button is the number `1`).
//
// Passing one to the other does not fail loudly: the X server accepts
// a key it does not recognise and injects *nothing*, so the device
// reports success while the operator's keystrokes go nowhere.  The
// tables are therefore checked here, on every CI leg, with the
// expected values spelled out literally -- so a wrong *entry* is
// caught, not merely a change.

TEST(RdInputInject, TheLinuxKeycodeTableIsCorrect) {
    using namespace nimrtc::rd_input_inject::detail;

    // The letters.  These are the US layout's hardware positions: the
    // kernel numbers the keys in the order they sit on the keyboard.
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kA), 30);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kS), 31);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kD), 32);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF), 33);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kG), 34);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kH), 35);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kJ), 36);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kK), 37);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kL), 38);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kQ), 16);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kW), 17);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kE), 18);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kR), 19);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kT), 20);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kY), 21);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kU), 22);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kI), 23);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kO), 24);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kP), 25);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kZ), 44);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kX), 45);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kC), 46);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kV), 47);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kB), 48);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kN), 49);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kM), 50);

    // The editing / navigation keys.
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kEnter), 28);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kTab), 15);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kSpace), 57);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kBackspace), 14);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kDelete), 111);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kEscape), 1);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kHome), 102);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kPageUp), 104);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kEnd), 107);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kPageDown), 109);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kLeft), 105);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kRight), 106);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kDown), 108);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kUp), 103);

    // The modifiers.
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kShift), 42);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kControl), 29);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kAlt), 56);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kMeta), 125);

    // The function keys.
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF1), 59);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF2), 60);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF3), 61);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF4), 62);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF5), 63);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF6), 64);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF7), 65);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF8), 66);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF9), 67);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF10), 68);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF11), 87);
    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kF12), 88);
}

TEST(RdInputInject, TheLinuxButtonCodeTableIsCorrect) {
    using namespace nimrtc::rd_input_inject::detail;

    EXPECT_EQ(linux_button_code_for(nimrtc::rd_input::MouseButton::kLeft),
              0x110);  // BTN_LEFT
    EXPECT_EQ(linux_button_code_for(nimrtc::rd_input::MouseButton::kRight),
              0x111);  // BTN_RIGHT
    EXPECT_EQ(linux_button_code_for(nimrtc::rd_input::MouseButton::kMiddle),
              0x112);  // BTN_MIDDLE
    EXPECT_EQ(linux_button_code_for(nimrtc::rd_input::MouseButton::kBack),
              0x113);  // BTN_SIDE
    EXPECT_EQ(linux_button_code_for(nimrtc::rd_input::MouseButton::kForward),
              0x114);  // BTN_EXTRA
}

TEST(RdInputInject, TheX11KeysymTableIsCorrect) {
    using namespace nimrtc::rd_input_inject::detail;

    // The letters.  For Latin-1 the X11 KeySym *is* the character
    // code, so 'a' is 0x61.
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kA), 0x0061);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kB), 0x0062);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kC), 0x0063);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kD), 0x0064);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kE), 0x0065);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF), 0x0066);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kG), 0x0067);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kH), 0x0068);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kI), 0x0069);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kJ), 0x006A);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kK), 0x006B);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kL), 0x006C);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kM), 0x006D);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kN), 0x006E);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kO), 0x006F);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kP), 0x0070);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kQ), 0x0071);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kR), 0x0072);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kS), 0x0073);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kT), 0x0074);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kU), 0x0075);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kV), 0x0076);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kW), 0x0077);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kX), 0x0078);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kY), 0x0079);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kZ), 0x007A);

    // The editing / navigation keys.  X11 spells "page up" and "page
    // down" as "prior" and "next".
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kEscape), 0xFF1B);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kTab), 0xFF09);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kEnter), 0xFF0D);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kSpace), 0x0020);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kBackspace), 0xFF08);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kDelete), 0xFFFF);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kHome), 0xFF50);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kEnd), 0xFF57);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kPageUp), 0xFF55);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kPageDown), 0xFF56);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kLeft), 0xFF51);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kUp), 0xFF52);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kRight), 0xFF53);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kDown), 0xFF54);

    // The modifiers.  The X server names the *left* variant of each.
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kShift), 0xFFE1);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kControl), 0xFFE3);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kAlt), 0xFFE9);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kMeta), 0xFFEB);

    // The function keys.
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF1), 0xFFBE);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF2), 0xFFBF);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF3), 0xFFC0);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF4), 0xFFC1);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF5), 0xFFC2);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF6), 0xFFC3);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF7), 0xFFC4);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF8), 0xFFC5);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF9), 0xFFC6);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF10), 0xFFC7);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF11), 0xFFC8);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kF12), 0xFFC9);
}

TEST(RdInputInject, TheX11ButtonNumberTableIsCorrect) {
    using namespace nimrtc::rd_input_inject::detail;

    // The X server's conventional numbering.  These are *not* the
    // kernel's BTN_* codes, which is the whole point: the XTest path
    // used to pass BTN_LEFT (0x110) where the server expects 1, and
    // the server accepted it silently while injecting nothing.
    EXPECT_EQ(x11_button_number_for(nimrtc::rd_input::MouseButton::kLeft), 1u);
    EXPECT_EQ(x11_button_number_for(nimrtc::rd_input::MouseButton::kMiddle), 2u);
    EXPECT_EQ(x11_button_number_for(nimrtc::rd_input::MouseButton::kRight), 3u);
    EXPECT_EQ(x11_button_number_for(nimrtc::rd_input::MouseButton::kBack), 8u);
    EXPECT_EQ(x11_button_number_for(nimrtc::rd_input::MouseButton::kForward), 9u);
}
TEST(RdInputInject, TheTwoLinuxVocabulariesDoNotCollide) {
    using namespace nimrtc::rd_input_inject::detail;

    // The regression this table exists to prevent: the XTest path used
    // the *kernel's* codes, so a key press asked the X server for a
    // key it does not have.  The X server accepts it and injects
    // nothing -- the operator's keystroke goes nowhere while the
    // device reports success.
    //
    // So for every key the table knows, the two vocabularies must
    // disagree.  If they ever agreed, one of the tables would have
    // been copied from the other by mistake.
    const inj::NeutralKey kKeys[] = {
        inj::NeutralKey::kA,  inj::NeutralKey::kZ,
        inj::NeutralKey::kF1, inj::NeutralKey::kF12,
        inj::NeutralKey::kEnter, inj::NeutralKey::kEscape,
        inj::NeutralKey::kSpace, inj::NeutralKey::kTab,
        inj::NeutralKey::kLeft, inj::NeutralKey::kRight,
        inj::NeutralKey::kUp,   inj::NeutralKey::kDown,
        inj::NeutralKey::kHome, inj::NeutralKey::kEnd,
        inj::NeutralKey::kPageUp, inj::NeutralKey::kPageDown,
        inj::NeutralKey::kShift, inj::NeutralKey::kControl,
        inj::NeutralKey::kAlt,  inj::NeutralKey::kMeta,
    };
    for (const inj::NeutralKey key : kKeys) {
        EXPECT_NE(linux_keycode_for(key), kLinuxKeyUnknown)
            << "the kernel table does not know the key";
        EXPECT_NE(x11_keysym_for(key), kX11KeySymUnknown)
            << "the X11 table does not know the key";
        EXPECT_NE(linux_keycode_for(key), x11_keysym_for(key))
            << "the kernel code and the X11 KeySym must not be the same "
               "value -- one of the tables was copied from the other";
    }
}

TEST(RdInputInject, TheAsciiTablesAreCorrect) {
    using namespace nimrtc::rd_input_inject::detail;

    // The uinput side: the US layout's key for the character.
    EXPECT_EQ(linux_keycode_for_ascii('a'), 30);   // KEY_A
    EXPECT_EQ(linux_keycode_for_ascii('z'), 44);   // KEY_Z
    EXPECT_EQ(linux_keycode_for_ascii('0'), 11);   // KEY_0
    EXPECT_EQ(linux_keycode_for_ascii('9'), 10);   // KEY_9
    EXPECT_EQ(linux_keycode_for_ascii(' '), 57);   // KEY_SPACE
    EXPECT_EQ(linux_keycode_for_ascii('\n'), 28);  // KEY_ENTER
    EXPECT_EQ(linux_keycode_for_ascii('\t'), 15);  // KEY_TAB
    EXPECT_EQ(linux_keycode_for_ascii('/'), 53);   // KEY_SLASH
    EXPECT_EQ(linux_keycode_for_ascii('\\'), 43);  // KEY_BACKSLASH
    EXPECT_EQ(linux_keycode_for_ascii(','), 51);   // KEY_COMMA
    EXPECT_EQ(linux_keycode_for_ascii('.'), 52);   // KEY_DOT
    EXPECT_EQ(linux_keycode_for_ascii('-'), 12);   // KEY_MINUS
    EXPECT_EQ(linux_keycode_for_ascii('='), 13);   // KEY_EQUAL
    EXPECT_EQ(linux_keycode_for_ascii('['), 26);   // KEY_LEFTBRACE
    EXPECT_EQ(linux_keycode_for_ascii(']'), 27);   // KEY_RIGHTBRACE
    EXPECT_EQ(linux_keycode_for_ascii(';'), 39);   // KEY_SEMICOLON
    EXPECT_EQ(linux_keycode_for_ascii('\''), 40);  // KEY_APOSTROPHE
    EXPECT_EQ(linux_keycode_for_ascii('`'), 41);   // KEY_GRAVE

    // A character with no key on the US layout is refused, not guessed.
    EXPECT_EQ(linux_keycode_for_ascii('\x01'), kLinuxKeyUnknown);
    EXPECT_EQ(linux_keycode_for_ascii('\x7F'), kLinuxKeyUnknown);

    // The X11 side: for Latin-1 the KeySym is the character code, with
    // the two control characters spelled out.
    EXPECT_EQ(x11_keysym_for_ascii('a'), 0x0061);
    EXPECT_EQ(x11_keysym_for_ascii('Z'), 0x005A);
    EXPECT_EQ(x11_keysym_for_ascii(' '), 0x0020);
    EXPECT_EQ(x11_keysym_for_ascii('/'), 0x002F);
    EXPECT_EQ(x11_keysym_for_ascii('\n'), 0xFF0D);  // XK_Return
    EXPECT_EQ(x11_keysym_for_ascii('\r'), 0xFF0D);  // XK_Return
    EXPECT_EQ(x11_keysym_for_ascii('\t'), 0xFF09);  // XK_Tab

    // A control character with no KeySym on this path is refused.
    EXPECT_EQ(x11_keysym_for_ascii('\x01'), kX11KeySymUnknown);
    EXPECT_EQ(x11_keysym_for_ascii('\x7F'), kX11KeySymUnknown);
}

TEST(RdInputInject, TheLinuxTablesRefuseTheUnknownKey) {
    using namespace nimrtc::rd_input_inject::detail;

    EXPECT_EQ(linux_keycode_for(inj::NeutralKey::kNone), kLinuxKeyUnknown);
    EXPECT_EQ(x11_keysym_for(inj::NeutralKey::kNone), kX11KeySymUnknown);
    EXPECT_EQ(linux_button_code_for(nimrtc::rd_input::MouseButton::kUnknown),
              kLinuxButtonUnknown);
    EXPECT_EQ(x11_button_number_for(nimrtc::rd_input::MouseButton::kUnknown),
              kX11ButtonUnknown);

    // The sentinel must not collide with a real entry.
    EXPECT_NE(linux_keycode_for(inj::NeutralKey::kA), kLinuxKeyUnknown);
    EXPECT_NE(x11_keysym_for(inj::NeutralKey::kA), kX11KeySymUnknown);
}

