/**
 * @file src/engine/src/engine_turn_credentials.cpp
 * @brief PRIVATE engine collaborator (v1.4 N7): the implementation half of
 * `engine_turn_credentials.hpp`.
 *
 * The machine owns no clock and no transport: `refresh()` receives `now_us`
 * from the caller (the engine's tick / open path), and the actual
 * `plugins::IICETransport::add_turn_server_with_transport()` calls are
 * executed by the engine — this TU only books what a pull produced, how it
 * failed, and whether the transport rejected it.
 */
#include "engine_turn_credentials.hpp"

namespace nimrtc::engine::detail {

namespace {

/** Whether two credential sets are the same, field for field. */
bool same_credentials(const TurnCredentials& a, const TurnCredentials& b) noexcept {
    return a.host == b.host && a.port == b.port && a.username == b.username &&
           a.password == b.password && a.transport == b.transport &&
           a.tls_server_name == b.tls_server_name;
}

}  // namespace

bool TurnCredentialsState::refresh(std::int64_t now_us,
                                   TurnCredentialsProvider provider,
                                   void* ctx, TurnCredentials* out) noexcept {
    (void)now_us;
    // A null provider means "not configured" — the caller should not have
    // consulted the machine at all, but it must not crash on the attempt
    // either (spec §四 15).  The state is left untouched.
    if (provider == nullptr) return false;

    TurnCredentials c;
    const bool obtained = provider(ctx, &c) && !c.host.empty();
    if (obtained) {
        // Port 0 means "the default" (TURN/UDP 3478, spec §设计 一).
        // Normalize it here so `credentials_changed()` compares effective
        // values — a provider that returns port 0 twice means "default"
        // both times, which is the same server, not a change.
        if (c.port == 0) c.port = 3478;
        last_ = c;
        has_last_ = true;
        // A successful pull breaks the consecutive-failure streak.  The
        // rejection latch is sticky: the fact that the server once refused
        // our credentials outlives the pull that replaced them.
        consecutive_failures_ = 0;
        if (state_ != State::kRejected) state_ = State::kOk;
        if (out != nullptr) *out = c;
        return true;
    }

    // The provider returned false — or true with an empty host, which the
    // spec treats identically (spec §四 16: "当作拿不到").  Count the
    // consecutive failure and report "unavailable"; the caller keeps the
    // session going without TURN.
    ++consecutive_failures_;
    if (state_ != State::kRejected) state_ = State::kUnavailable;
    return false;
}

std::string_view TurnCredentialsState::state_string() const noexcept {
    switch (state_) {
        case State::kIdle:        return "idle";
        case State::kOk:          return "ok";
        case State::kUnavailable: return "unavailable";
        case State::kRejected:    return "rejected";
    }
    return "idle";
}

bool TurnCredentialsState::credentials_changed() const noexcept {
    if (!has_last_) return false;
    // Nothing has been applied yet: the freshly pulled credentials are by
    // definition "not the applied ones" — they must be applied.
    if (!has_applied_) return true;
    return !same_credentials(last_, applied_);
}

void TurnCredentialsState::note_applied() noexcept {
    if (!has_last_) return;
    applied_ = last_;
    has_applied_ = true;
}

void TurnCredentialsState::reset() noexcept {
    state_ = State::kIdle;
    consecutive_failures_ = 0;
    has_last_ = false;
    has_applied_ = false;
    last_ = TurnCredentials{};
    applied_ = TurnCredentials{};
}

}  // namespace nimrtc::engine::detail
