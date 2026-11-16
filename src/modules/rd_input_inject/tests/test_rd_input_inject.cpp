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

// The base class is not exported through the public header, so the
// test reaches it by a relative path.  That is deliberate: the base
// class is an implementation detail, and a test that only used the
// public seam could not prove the state machine's behaviour.
#include "../src/injector_base.hpp"

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
