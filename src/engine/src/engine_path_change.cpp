/**
 * @file src/engine/src/engine_path_change.cpp
 * @brief PRIVATE engine collaborator (v1.4 N8): the path-change ledger.
 *
 * The implementation half of `engine_path_change.hpp`.  The tracker owns no
 * clock and no transport: the "before" pair is passed in by the engine at
 * each restart point, and the reason is a pure decision over the two pairs.
 */
#include "engine_path_change.hpp"

namespace nimrtc::engine::detail {

void PathChangeTracker::note_before(
    std::optional<IceCandidatePairInfo> pair) noexcept {
    before_ = std::move(pair);
}

const std::optional<IceCandidatePairInfo>&
PathChangeTracker::before() const noexcept {
    return before_;
}

void PathChangeTracker::reset() noexcept {
    before_ = std::nullopt;
}

std::string_view PathChangeTracker::classify(
    const std::optional<IceCandidatePairInfo>& old_pair,
    const std::optional<IceCandidatePairInfo>& new_pair) noexcept {
    if (!old_pair && !new_pair) return "";
    if (!old_pair || !new_pair) return "local-address-changed";

    // Both ends present: the only pair-shape difference the reason
    // vocabulary names is the relay bit.  Anything else is a local address
    // change (the same judgment `TransportStats::relayed` makes: either end
    // with type "relay" makes the path relayed).
    if (is_relayed(*old_pair) != is_relayed(*new_pair)) {
        return "relay-fallback";
    }
    return "local-address-changed";
}

bool PathChangeTracker::same_path(const IceCandidatePairInfo& a,
                                  const IceCandidatePairInfo& b) noexcept {
    return a.local.address == b.local.address &&
           a.local.port == b.local.port &&
           a.local.protocol == b.local.protocol &&
           a.local.type == b.local.type &&
           a.remote.address == b.remote.address &&
           a.remote.port == b.remote.port &&
           a.remote.protocol == b.remote.protocol &&
           a.remote.type == b.remote.type;
}

}  // namespace nimrtc::engine::detail
