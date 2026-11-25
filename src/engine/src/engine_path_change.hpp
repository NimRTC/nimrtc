/**
 * @file src/engine/src/engine_path_change.hpp
 * @brief PRIVATE engine collaborator (v1.4 N8): the path-change bookkeeping.
 *
 * The engine already restarts ICE on its own (N3, ADR-031) and on demand
 * (`restart_ice()`), but a host that learns about the switch afterwards only
 * gets a reason — it cannot tell "which path were we on, and which one are
 * we on now".  N8 closes that gap: at each report point the engine records
 * the pair it was using **before** the restart, reads the pair it is on
 * **after** it, and hands the host both — plus the reason, classified from
 * the two pairs by a pure function.
 *
 * ## What lives here
 *
 *   - `PathChangeTracker` — the "which pair was the previous one" ledger.
 *     The engine calls `note_before(current_pair())` at each restart point
 *     before the transport restarts, reads `before()` afterwards, and lets
 *     `classify(old, new)` decide the reason.
 *
 *   - `classify` / `same_path` — the two pure decisions, shared by every
 *     report point so the reason vocabulary cannot drift between them.
 *
 * ## Clock discipline
 *
 * The tracker holds no clock and touches no transport: the pair is taken
 * by the caller, the reason is decided from the pairs alone (C3 — a
 * decision must be deterministic).  `reset()` is called only at
 * `open()` / `close()`, so each session's ledger starts empty.
 *
 * ## Privacy
 *
 * This header is PRIVATE: it is not installed and no public header includes
 * it.  It reaches `IceCandidatePairInfo` through the engine's own public
 * header — never through a concrete `nimrtc::ice/` header (Layout Invariant
 * 4 / ADR-025: the engine reads ICE state through `plugins::IICETransport`
 * and projects it onto its own public shape).
 */
#pragma once

#include <optional>
#include <string_view>

#include <nimrtc/engine/engine.hpp>

namespace nimrtc::engine::detail {

/** The path-change ledger (v1.4 N8).
 *
 *  The engine records the current pair **before** an ICE restart, then reads
 *  the new pair **after** it; the two, compared, say "from where to where".
 *  Without this ledger the "previous path" would be scattered across the
 *  three call sites (N3 restart, manual restart, peer restart) and would
 *  drift.
 *
 *  It holds no transport handle and no clock: the pair comes in through
 *  `note_before()`, the reason comes out of `classify()`.
 */
class PathChangeTracker {
public:
    /** Record the "before the switch" pair (call before the restart). */
    void note_before(std::optional<IceCandidatePairInfo> pair) noexcept;

    /** The recorded "before" pair (nothing recorded yet = nullopt). */
    const std::optional<IceCandidatePairInfo>& before() const noexcept;

    /** Clear the ledger (called at open() / close() — every session starts
     *  from zero). */
    void reset() noexcept;

    /** Classify the change between two pairs into the closed reason
     *  vocabulary.
     *
     *  - both empty / old empty + new empty → "" (nothing to report)
     *  - old empty, new present → "local-address-changed" (first selection)
     *  - old present, new empty → "local-address-changed" (pair dropped)
     *  - both present, **relay bit differs** → "relay-fallback"
     *  - both present, relay bit the same → "local-address-changed"
     *
     *  The relay bit is judged the same way `TransportStats::relayed` is:
     *  either end with `type == "relay"` makes the path relayed. */
    static std::string_view classify(
        const std::optional<IceCandidatePairInfo>& old_pair,
        const std::optional<IceCandidatePairInfo>& new_pair) noexcept;

    /** Whether two pairs are the same path (field-by-field compare). */
    static bool same_path(const IceCandidatePairInfo& a,
                          const IceCandidatePairInfo& b) noexcept;

private:
    static bool is_relayed(const IceCandidatePairInfo& p) noexcept {
        return p.local.type == "relay" || p.remote.type == "relay";
    }

    std::optional<IceCandidatePairInfo> before_;
};

}  // namespace nimrtc::engine::detail
