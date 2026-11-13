/**
 * @file src/engine/src/engine_ref_frame_table.hpp
 * @brief PRIVATE engine helper: the ref-frame → capture-timestamp table.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): this table
 * used to be a block of members *and* methods inside
 * `NimRTCEngine::Impl`, which made the engine's internal header a
 * catch-all.  The table is self-contained — it owns its storage and
 * knows nothing about the engine — so it is a collaborator class now.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What the table is for
 *
 * The engine stamps every outbound video frame with a "ref frame id"
 * (the `VideoSourceFrame::frame_seq`) and the frame's capture instant.
 * When the peer reports a loss, the engine has to turn that id back into
 * the frame it named, so it can retransmit or force an IDR.
 *
 * ## Why it is a fixed-size open-addressed table
 *
 * The registry used to be a `std::map` + a `std::deque`, so every
 * recorded frame allocated a map node *and* a deque block — two heap
 * allocations per sent frame, forever.  A fixed-size open-addressed
 * table plus a fixed-size insertion ring replaces both: they are
 * allocated once and never grow, so recording a frame performs no
 * allocation at all.
 *
 * ## Thread safety
 *
 * None — single-threaded by engine contract (the same contract the
 * sender-report tracker follows).  The engine's send path is the only
 * writer and the feedback path the only reader.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nimrtc::engine::detail {

/** A bounded registry from ref-frame id to capture timestamp.
 *
 *  The table is sized once (at construction) and never resized, so
 *  `record()` performs no allocation.  When the registry is full the
 *  oldest entry is evicted — the registry cannot grow without bound. */
class RefFrameTable {
public:
    /// The registry holds this many ids at most.
    static constexpr std::size_t kCap = 1024;

    /// Sentinel for a free slot.  `UINT64_MAX` cannot be a real
    ///  frame_seq in practice, and the table is only 1024 entries so the
    ///  odds of a collision are negligible — and a collision would only
    ///  affect the *lookup* of that one id, not the correctness of the
    ///  ring.
    static constexpr std::uint64_t kEmptyId = UINT64_MAX;

    /// One slot of the open-addressed table.
    ///
    ///  `id == kEmptyId` marks a free slot.  Linear probing is used (the
    ///  table is at most half full, so the probe length is short).
    struct Slot {
        std::uint64_t id = kEmptyId;
        std::int64_t  capture_ts_us = 0;
    };

    RefFrameTable() { init(); }

    /// Size the table for `kCap` entries at a load factor of at most 0.5.
    void init() {
        std::size_t n = 1;
        while (n < kCap * 2) n <<= 1;
        table_.assign(n, Slot{});
        order_.assign(kCap, kEmptyId);
    }

    /// Record `id` with its capture timestamp, evicting the oldest entry
    ///  when the registry is full.
    void record(std::uint64_t id, std::int64_t capture_ts_us) {
        bool found = false;
        Slot* slot = find(id, found);
        if (!slot) return;          // the table is full and unprobeable
        if (!found) {
            // A new id: it may have to evict the oldest one.
            if (count_ >= order_.size()) {
                const std::uint64_t oldest = oldest_id();
                if (oldest != kEmptyId) erase(oldest);
            }
            slot->id = id;
            push(id);
        }
        slot->capture_ts_us = capture_ts_us;
    }

    /// The capture timestamp recorded for `id`, if it is still in the
    ///  registry.
    [[nodiscard]] bool lookup(std::uint64_t id,
                              std::int64_t& capture_ts_us) const {
        bool found = false;
        const Slot* slot = find(id, found);
        if (!found || !slot) return false;
        capture_ts_us = slot->capture_ts_us;
        return true;
    }

    /// Whether `id` is still in the registry.
    [[nodiscard]] bool contains(std::uint64_t id) const {
        bool found = false;
        (void)find(id, found);
        return found;
    }

    /// The smallest id whose recorded capture timestamp is
    ///  `capture_ts_us`.
    ///
    ///  The registry used to be a `std::map`, so a timestamp shared by
    ///  several ids resolved to the *smallest* id.  The table is
    ///  unordered, so the smallest match is selected explicitly to keep
    ///  the observable behaviour identical.
    [[nodiscard]] std::optional<std::uint64_t> find_smallest_with_timestamp(
        std::int64_t capture_ts_us) const {
        std::uint64_t best = 0;
        bool found = false;
        for (const Slot& slot : table_) {
            if (slot.id == kEmptyId) continue;
            if (slot.capture_ts_us != capture_ts_us) continue;
            if (!found || slot.id < best) {
                best = slot.id;
                found = true;
            }
        }
        if (!found) return std::nullopt;
        return best;
    }

private:
    /// The oldest id still in the insertion-order ring.
    [[nodiscard]] std::uint64_t oldest_id() const noexcept {
        if (count_ == 0) return kEmptyId;
        const std::size_t tail = (head_ + order_.size() - count_) % order_.size();
        return order_[tail];
    }

    /// Append an id to the insertion-order ring.
    void push(std::uint64_t id) noexcept {
        if (order_.empty()) return;
        order_[head_] = id;
        head_ = (head_ + 1) % order_.size();
        if (count_ < order_.size()) {
            count_++;
        }
    }

    /// Remove an id from the hash table (used when it is evicted).
    ///
    /// Linear probing needs *backward-shift* deletion: simply marking the
    /// slot free would break the probe chain for every key that probed
    /// past it, and those keys would become unreachable.  Instead the
    /// entries that follow the hole are pulled back until the chain is
    /// contiguous again.
    void erase(std::uint64_t id) noexcept {
        bool found = false;
        Slot* slot = find(id, found);
        if (!slot || !found) return;

        const std::size_t mask = table_.size() - 1;
        std::size_t i = static_cast<std::size_t>(slot - table_.data());
        table_[i].id = kEmptyId;

        // Pull back every entry whose home slot lies on the probe path
        // that the hole just broke.
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask;
            const std::uint64_t moved_id = table_[j].id;
            if (moved_id == kEmptyId) break;
            const std::size_t home = hash(moved_id) & mask;
            // "home" is cyclically in (i, j] → the entry must stay at j.
            const bool must_stay = (i < j) ? (home > i && home <= j)
                                           : (home > i || home <= j);
            if (!must_stay) {
                table_[i] = table_[j];
                table_[j].id = kEmptyId;
                i = j;
            }
        }
    }

    /// The hash of a frame id.  A 64-bit mix (splitmix64 finaliser) so
    ///  sequential frame ids spread evenly over the table.
    [[nodiscard]] static std::size_t hash(std::uint64_t id) noexcept {
        std::uint64_t z = id + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return static_cast<std::size_t>(z);
    }

    /// Find the slot holding `id`, or the first free slot on the probe
    ///  path (which is where the id would be inserted).  `found` reports
    ///  whether the id was actually present.
    [[nodiscard]] Slot* find(std::uint64_t id, bool& found) noexcept {
        found = false;
        if (table_.empty()) return nullptr;
        const std::size_t mask = table_.size() - 1;
        std::size_t i = hash(id) & mask;
        for (std::size_t probe = 0; probe <= mask; ++probe) {
            Slot& slot = table_[i];
            if (slot.id == id) {
                found = true;
                return &slot;
            }
            if (slot.id == kEmptyId) return &slot;
            i = (i + 1) & mask;
        }
        return nullptr;
    }

    /// The const overload, for the read-only accessors.
    [[nodiscard]] const Slot* find(std::uint64_t id, bool& found) const noexcept {
        found = false;
        if (table_.empty()) return nullptr;
        const std::size_t mask = table_.size() - 1;
        std::size_t i = hash(id) & mask;
        for (std::size_t probe = 0; probe <= mask; ++probe) {
            const Slot& slot = table_[i];
            if (slot.id == id) {
                found = true;
                return &slot;
            }
            if (slot.id == kEmptyId) return nullptr;
            i = (i + 1) & mask;
        }
        return nullptr;
    }

    /// The open-addressed table.  Sized to a power of two >= 2x the cap so
    ///  the load factor never exceeds 0.5.
    std::vector<Slot> table_;

    /// The ids in insertion order (oldest first), used for eviction.
    ///
    /// A fixed-size circular buffer, not a `std::deque`: the deque grew a
    /// new block every so often, which is a heap allocation on the send
    /// path.  The ring is allocated once, together with the table.
    std::vector<std::uint64_t> order_;

    /// Write cursor into `order_` (the next slot to write).
    std::size_t head_ = 0;

    /// How many ids are currently in `order_`.
    std::size_t count_ = 0;
};

}  // namespace nimrtc::engine::detail
