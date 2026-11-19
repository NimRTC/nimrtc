/**
 * @file nimrtc/net_sim/nat_relay.hpp
 * @brief The **simulated NAT** — an address-translating UDP relay.
 *
 * ## What it is
 *
 * A relay with **two** UDP sockets.  Two peers each advertise one of
 * the relay's addresses in their SDP, and every packet that arrives at
 * one socket is forwarded to the *other* peer **through the other
 * socket**.  The consequence is the whole point of the thing:
 *
 * > Neither peer ever sees the other's real address.  Each sees the
 * > relay's address, exactly as a peer behind a NAT sees the NAT's
 * > server-reflexive address.
 *
 * This is the v1.3 plan's Q5 "simulated NAT" — an
 * address-translating relay that is **not** a TURN server.  The plan's
 * §5 (Q5-1) records the decision: the real TURN leg is Track C's C5
 * and is deferred to v1.4.0, and the simulated NAT keeps Q5
 * independent of it.
 *
 * ## Why two sockets and not one
 *
 * With a single socket the relay would have to decide, for every
 * packet, which peer it came from — and it could only do that by
 * already knowing the peers' real addresses.  With two sockets the
 * decision is structural: peer A sends to the relay's *second*
 * address and peer B sends to the relay's *first*, so each socket
 * sees exactly one peer.  The relay **learns** each peer's real
 * address from the first packet that arrives on its socket, and the
 * two learned addresses are what it forwards to.
 *
 * The relay rewrites the **source** address of every forwarded packet
 * to the address of the socket it forwards through, so the packet
 * arrives at the peer looking as though it came from the address that
 * peer was told to expect.  That is what keeps ICE happy: the
 * connectivity check, the binding response, the DTLS handshake and the
 * media all travel over the same translated path, and each side's
 * selected candidate pair stays consistent.
 *
 * ## The impairments
 *
 * The relay applies an `ImpairmentConfig` to every packet it
 * forwards, so the impairment hits the **real RTP path** — the same
 * bytes the kernel would have dropped, delayed, reordered or
 * rate-limited under `tc qdisc netem`.  The relay is the portable
 * half of Q5; `tools/benchmark_weaknet_netem.sh` is the kernel-side
 * half, and the two are driven from the same profile.
 *
 * ## Threading
 *
 * One thread owns the sockets.  `start()` spawns it, `stop()` joins
 * it, and every socket call happens on that thread.  The public
 * accessors (`stats()`, `endpoint_a()`, `endpoint_b()`) are safe to
 * call from any thread.
 *
 * @see nimrtc/net_sim/impairment.hpp — the profile the relay applies
 * @see docs/plan/v1.3-plan.md §Q5 — the soak this relay exists for
 */

#ifndef NIMRTC_NET_SIM_NAT_RELAY_HPP
#define NIMRTC_NET_SIM_NAT_RELAY_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <string>

#include "nimrtc/net_sim/impairment.hpp"

namespace nimrtc::net_sim {

// ---------------------------------------------------------------------------
// The relay
// ---------------------------------------------------------------------------

/**
 * @brief An address-translating UDP relay: the v1.3 plan's simulated NAT.
 */
class NIMRTC_API_STABLE NatRelay {
public:
    /** How the relay is configured. */
    struct NIMRTC_API_STABLE Config {
        /** The address the relay binds to.  The default (loopback) is
         *  what the soak uses: the relay sits between two peers on the
         *  same machine. */
        std::string bind_host = "127.0.0.1";

        /** The impairment profile applied to every forwarded packet. */
        ImpairmentConfig impairment;

        /** When false, the relay forwards every packet unchanged — the
         *  translation without the impairments.  Useful for measuring
         *  the relay's own overhead separately from the link's. */
        bool impaired = true;
    };

    /** What the relay has seen. */
    struct NIMRTC_API_STABLE Stats {
        /** The packets the relay received on either socket. */
        std::uint64_t packets_received = 0;
        /** The packets the relay forwarded to a peer. */
        std::uint64_t packets_forwarded = 0;
        /** The bytes the relay forwarded. */
        std::uint64_t bytes_forwarded = 0;
        /** The packets the impairment profile dropped. */
        std::uint64_t packets_dropped = 0;
        /** The packets the impairment profile held back to reorder. */
        std::uint64_t packets_reordered = 0;
        /** The packets that arrived from a source the relay had not
         *  learned yet and were therefore buffered until it had. */
        std::uint64_t packets_buffered = 0;
        /** The packets that arrived from an address the relay did not
         *  expect (a stray packet, or a peer that moved). */
        std::uint64_t packets_unknown_source = 0;
        /** The packets the relay had to drop because its own buffer was
         *  full. */
        std::uint64_t packets_dropped_buffer_full = 0;
    };

    /** Build a relay.  The sockets are not bound until `start()`. */
    explicit NatRelay(const Config& config);

    /** Stop the relay (if it is running) and release the sockets. */
    ~NatRelay();

    NatRelay(const NatRelay&) = delete;
    NatRelay& operator=(const NatRelay&) = delete;

    /**
     * @brief Bind the sockets and start the relay thread.
     *
     * @param[out] error  the reason the relay could not start (empty on
     *                    success).
     * @return `true` when the relay is running.
     */
    bool start(std::string* error);

    /** Stop the relay.  Idempotent. */
    void stop() noexcept;

    /** True while the relay thread is running. */
    bool running() const noexcept;

    /**
     * @brief The address **peer A** advertises in its SDP.
     *
     *  The host part is the relay's `bind_host`; the port is the relay's
     *  first socket's.  Peer B sends its traffic here, and the relay
     *  forwards it to peer A.
     *
     *  Empty before `start()`.
     */
    std::string endpoint_a() const;

    /**
     * @brief The address **peer B** advertises in its SDP.
     *
     *  The mirror image of `endpoint_a()`: peer A sends its traffic
     *  here, and the relay forwards it to peer B.
     */
    std::string endpoint_b() const;

    /** The relay's counters. */
    Stats stats() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace nimrtc::net_sim

#endif // NIMRTC_NET_SIM_NAT_RELAY_HPP
