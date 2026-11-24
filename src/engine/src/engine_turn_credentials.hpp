/**
 * @file src/engine/src/engine_turn_credentials.hpp
 * @brief PRIVATE engine collaborator (v1.4 N7): TURN credentials as a provider seam.
 *
 * ADR-032 (proposed): TURN credentials are the host's business — they are
 * issued by the host's signalling service and carry a TTL, so the engine
 * cannot hold them once and be done.  The host hands the engine a
 * `TurnCredentialsProvider` in `EngineConfig::turn_credentials`, and the
 * engine *pulls* a fresh credential set at the two moments a TURN server
 * is (re)configured:
 *
 *   1. before candidate gathering (`open()`), when there are no static
 *      TURN servers;
 *   2. after every N3 reconnection restart, because the old credentials
 *      may have expired while the session was down.
 *
 * ## What lives here
 *
 * `TurnCredentialsState` — the bookkeeping for those pulls: what the most
 * recent pull produced, whether it failed (consecutively), whether the
 * transport reported the applied credentials were rejected (401/403), and
 * whether the freshly pulled credentials differ from the ones last applied
 * to the transport.  It owns no clock and no transport: `refresh()`
 * receives `now_us` explicitly (the same clock discipline as
 * `engine_ice_recovery.hpp` / N3), and the engine executes the actual
 * `plugins::IICETransport::add_turn_server_with_transport()` calls.
 *
 * ## Privacy
 *
 * This header is PRIVATE: it is not installed and no public header includes
 * it.  It includes the engine's own public header for `TurnCredentials` /
 * `TurnCredentialsProvider` (the seam the slice adds there) and never a
 * concrete `nimrtc::ice/` header (Layout Invariant 4 / ADR-025).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <nimrtc/engine/engine.hpp>

namespace nimrtc::engine::detail {

/** The bookkeeping for the turn-credentials provider seam (v1.4 N7).
 *
 *  Owned by `SessionState`; driven by `NimRTCEngine::refresh_turn_credentials()`
 *  from `open()` and from the N3 reconnection restart.  The machine itself
 *  never touches a transport — it only records what a pull produced and
 *  how the transport answered.
 */
class TurnCredentialsState {
public:
    /** The stats-facing state (spec §四). */
    enum class State : std::uint8_t {
        kIdle = 0,        ///< no pull attempted yet
        kOk = 1,          ///< the last pull returned a usable credential
        kUnavailable = 2, ///< the last pull failed (provider false / empty host)
        kRejected = 3,    ///< the transport reported 401/403 (sticky latch)
    };

    /** Pull one credential set from the host's provider.
     *
     *  Returns true and fills `*out` when a usable set was obtained — the
     *  provider returned true with a non-empty `host` (spec §四 16: an
     *  empty host is treated exactly like "cannot get one").  A `port` of
     *  0 is normalized to 3478 (TURN/UDP default, spec §设计 一) before the
     *  value is recorded, so change detection compares effective values.
     *
     *  Returns false — and the caller keeps the session going without TURN —
     *  when the provider is null (spec §四 15: never call it, never crash),
     *  returned false, or returned an empty host.  A failed pull counts as a
     *  consecutive failure and puts the machine in `kUnavailable`; a
     *  successful pull clears the streak (spec §二 5 / §二 10).
     */
    bool refresh(std::int64_t now_us, TurnCredentialsProvider provider,
                 void* ctx, TurnCredentials* out) noexcept;

    /** True when the most recent pull(s) failed consecutively. */
    bool consecutive_failures() const noexcept { return consecutive_failures_ > 0; }

    /** The consecutive-failure count. */
    std::uint32_t consecutive_failures_count() const noexcept {
        return consecutive_failures_;
    }

    /** True once the transport reported the applied credentials were
     *  rejected (401/403).  The latch is sticky for the engine's lifetime
     *  (spec §测试策略 calls it 被拒闩锁); `reset()` clears it at open(). */
    bool rejected() const noexcept { return state_ == State::kRejected; }

    /** Fold the transport's rejection observation into the machine. */
    void note_rejected() noexcept { state_ = State::kRejected; }

    State state() const noexcept { return state_; }

    /** The stats spelling of `state()`: "idle" / "ok" / "unavailable" /
     *  "rejected".  The projection decides the "" value (no provider
     *  configured) from the engine config, not here. */
    std::string_view state_string() const noexcept;

    /** The credentials the most recent pull produced, or nullptr when no
     *  pull has produced one yet. */
    const TurnCredentials* last() const noexcept { return has_last_ ? &last_ : nullptr; }

    /** True when the freshly pulled credentials differ from the ones last
     *  applied to the transport — or none were applied yet (the first
     *  successful pull always has to be applied). */
    bool credentials_changed() const noexcept;

    /** Mark the freshly pulled credentials as applied to the transport. */
    void note_applied() noexcept;

    /** Forget every observation.  Called at `open()` — the ICE module's own
     *  process-wide rejection latch clears there too, so the machine starts
     *  each session clean. */
    void reset() noexcept;

private:
    State state_ = State::kIdle;
    std::uint32_t consecutive_failures_ = 0;
    TurnCredentials last_;
    bool has_last_ = false;
    TurnCredentials applied_;
    bool has_applied_ = false;
};

}  // namespace nimrtc::engine::detail
