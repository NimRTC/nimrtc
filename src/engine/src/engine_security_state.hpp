/**
 * @file src/engine/src/engine_security_state.hpp
 * @brief PRIVATE engine collaborator: the DTLS / SRTP transport-security state.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): this state
 * used to live as loose members inside `NimRTCEngine::Impl`, which made
 * the engine's internal header a catch-all and scattered one feature's
 * state across the file.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What the state is
 *
 * The engine's *transport security* — the DTLS handshake that produces
 * the keying material, and the SRTP context that consumes it:
 *
 *   1. the DTLS session's state transition trace (the last state the
 *      engine observed, so a transition is logged once);
 *   2. the SRTP installation flag — whether the keying material has
 *      been installed and the outbound path is encrypted;
 *   3. the SRTP drop counter — how many packets the outbound path
 *      refused to encrypt.
 *
 * Grouping them is not cosmetic: (2) is the *authority* on whether
 * the outbound path may send at all, and (3) only means something
 * once (2) is true.  A reader who has to look at three places to
 * answer "is this session encrypted?" will eventually get it wrong.
 *
 * ## Why the DTLS handle itself is NOT here
 *
 * `Impl::dtls` (the `weak_ptr<IDtlsSession>`) and `Impl::srtp` (the
 * `unique_ptr<SrtpContext>`) are the engine's *composition* — the
 * objects the engine owns — and they stay in `Impl` per §4.3.  This
 * collaborator holds only the *state* about them.
 */
#pragma once

#include <cstdint>

#include <nimrtc/dtls/dtls_types.hpp>

namespace nimrtc::engine::detail {

/** The DTLS / SRTP transport-security state.
 *
 *  Not thread-safe: the engine's tick loop is the only writer and the
 *  media path the only reader, on the same thread. */
struct SecurityState {
    // ---- The DTLS handshake trace ---------------------------------------

    /** The last DTLS state the engine observed.
     *
     *  The engine logs a *transition* rather than a state, so it needs
     *  the previous value to compare against.  It is a per-engine
     *  member (not a static local) so two engines in the same process
     *  — e.g. the loopback test — log their transitions
     *  independently. */
    nimrtc::dtls::DtlsState last_dtls_state = nimrtc::dtls::DtlsState::Closed;

    // ---- The SRTP installation ------------------------------------------

    /** Whether the SRTP keying material has been installed.
     *
     *  This is the *authority* on whether the outbound path is
     *  encrypted: every send path checks it before encrypting, and
     *  `NimRTCEngine::srtp_installed()` exposes it.  It becomes true
     *  exactly once per DTLS connection — at the moment the handshake
     *  reaches `Connected` and the keying material becomes available
     *  (RFC 5764 §4.2) — and false again when the transport tears the
     *  session down. */
    bool srtp_installed = false;

    /** How many outbound packets the SRTP layer refused to encrypt.
     *
     *  A drop here means the engine had a packet to send but no usable
     *  key, so the packet was discarded rather than sent in the clear.
     *  The count is surfaced through the engine's statistics as
     *  `srtp_drops`. */
    int srtp_stats_drop = 0;
};

}  // namespace nimrtc::engine::detail
