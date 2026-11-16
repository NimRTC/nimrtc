// src/modules/rd_session/tests/test_session_guard.cpp
// ----------------------------------------------------------------------------
// The session-guard module's unit tests.
//
// The suites cover the parts that can be pinned down on any CI leg:
//
//   guard_state_name_*   the three states are named, and named
//                        *distinctly* (a log that says "unknown" for
//                        two different problems is useless)
//   guard_hint_*         every state has a non-empty hint, and the
//                        locked hint names the *remedy*
//   guard_probe_*        the probe answers with one of the three states
//                        and does not crash
//   guard_recover_*      the recovery reports a reason when it does
//                        not succeed (never a silent no-op)
//
// The Windows backend's *behaviour* against a locked session cannot be
// unit-tested (it needs a locked workstation, and locking one
// interrupts the test run), so it is verified by the example's own
// diagnostics and by the LAN runbook instead.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <nimrtc/rd_session/session_guard.hpp>

#include <string>

namespace {

using nimrtc::rd_session::SessionGuardState;
using nimrtc::rd_session::query_session_guard_state;
using nimrtc::rd_session::recover_console_session;
using nimrtc::rd_session::session_guard_state_hint;
using nimrtc::rd_session::session_guard_state_name;

// ---------------------------------------------------------------------------
// The state names
// ---------------------------------------------------------------------------

TEST(SessionGuard, StateNamesAreDistinct) {
    // Three states, three names.  If two of them ever collapse into
    // the same string, the log stops being able to tell them apart --
    // which is the whole reason the state is an enum and not a bool.
    const std::string usable = std::string(session_guard_state_name(
        SessionGuardState::kUsable));
    const std::string locked = std::string(session_guard_state_name(
        SessionGuardState::kLocked));
    const std::string unknown = std::string(session_guard_state_name(
        SessionGuardState::kUnknown));

    EXPECT_NE(usable, locked);
    EXPECT_NE(usable, unknown);
    EXPECT_NE(locked, unknown);
}

TEST(SessionGuard, StateNamesAreNotEmpty) {
    EXPECT_FALSE(session_guard_state_name(SessionGuardState::kUsable).empty());
    EXPECT_FALSE(session_guard_state_name(SessionGuardState::kLocked).empty());
    EXPECT_FALSE(session_guard_state_name(SessionGuardState::kUnknown).empty());
}

// ---------------------------------------------------------------------------
// The hints
// ---------------------------------------------------------------------------

TEST(SessionGuard, EveryStateHasANonEmptyHint) {
    // An empty hint is worse than no hint: the log line reads as if
    // the module had nothing to say.
    EXPECT_FALSE(session_guard_state_hint(SessionGuardState::kUsable).empty());
    EXPECT_FALSE(session_guard_state_hint(SessionGuardState::kLocked).empty());
    EXPECT_FALSE(session_guard_state_hint(SessionGuardState::kUnknown).empty());
}

TEST(SessionGuard, TheLockedHintNamesTheRemedy) {
    // The hint is what an operator reads at 2 a.m. on someone else's
    // machine.  It has to say what to *do*, not just what happened.
    const std::string hint =
        session_guard_state_hint(SessionGuardState::kLocked);
    EXPECT_NE(hint.find("LOCKED"), std::string::npos);
    EXPECT_NE(hint.find("unlock"), std::string::npos);
}

TEST(SessionGuard, TheUnknownHintDoesNotClaimALock) {
    // "Unknown" must not read like "locked": the two need different
    // responses (unlock the machine vs. look at the picture).
    const std::string hint =
        session_guard_state_hint(SessionGuardState::kUnknown);
    EXPECT_EQ(hint.find("LOCKED"), std::string::npos);
    EXPECT_NE(hint.find("could not be determined"), std::string::npos);
}

// ---------------------------------------------------------------------------
// The probe
// ---------------------------------------------------------------------------

TEST(SessionGuard, TheProbeAnswersWithAKnownState) {
    // The probe cannot be *asserted* to a particular state (the answer
    // depends on the machine the test runs on), but it must always
    // answer with one of the three -- a garbage value would mean the
    // dispatch was wired wrong.
    const SessionGuardState state = query_session_guard_state();
    EXPECT_TRUE(state == SessionGuardState::kUsable ||
                state == SessionGuardState::kLocked ||
                state == SessionGuardState::kUnknown);
}

TEST(SessionGuard, TheProbeIsRepeatable) {
    // Two consecutive probes on a machine that is not being locked
    // underneath the test must agree -- an unstable answer would make
    // the periodic check in the device flap.
    const SessionGuardState first = query_session_guard_state();
    const SessionGuardState second = query_session_guard_state();
    EXPECT_EQ(first, second);
}

// ---------------------------------------------------------------------------
// The recovery
// ---------------------------------------------------------------------------

TEST(SessionGuard, TheRecoveryNeverFailsSilently) {
    // When the recovery does not succeed it must say why.  A silent
    // `false` would leave the operator staring at a locked machine
    // with no idea what happened.
    std::string error;
    const bool ok = recover_console_session(&error);
    if (!ok) {
        EXPECT_FALSE(error.empty())
            << "the recovery failed without saying why";
    }
}

TEST(SessionGuard, TheRecoveryToleratesANullErrorPointer) {
    // The error pointer is optional; passing null must not crash.
    (void)recover_console_session(nullptr);
}

} // namespace
