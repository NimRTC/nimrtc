/**
 * @file nimrtc/core/stats_registry.hpp
 * @brief The statistics registry: a named, snapshot-able view of a
 *        session's counters (plan Track S / S1, design plan DS-1).
 *
 * ## Why this exists
 *
 * Before this header the engine exposed exactly one statistics shape:
 * `NimRTCEngine::Stats`, a flat struct of ~130 cumulative counters.
 * That shape answers "how much has happened since `open()`", and
 * nothing else:
 *
 *   * a host cannot tell *when* a number was measured, so it cannot
 *     compute an interval rate (bytes per second, packets per second)
 *     without taking two readings and guessing how far apart they
 *     were;
 *   * a host cannot diff two readings in a *typed* way, because it
 *     does not know which fields are cumulative counters (diffable)
 *     and which are instantaneous gauges (not diffable);
 *   * a host cannot ask "give me the per-object entries" (one row per
 *     stream, per candidate pair, per data channel) because there is
 *     no place to put them.
 *
 * This header is the missing piece.  It is deliberately a **core**
 * type (Layout Invariant 1: `src/core/` is the only INTERFACE library
 * and only exposes stable types) so that every layer can speak it:
 * the engine publishes into it, a future module (ICE, DataChannel) can
 * publish into it through a plugin seam, and a host reads it.
 *
 * ## The three types
 *
 *   - `StatSemantics` — how a number behaves over time.  `kCumulative`
 *     means "monotonically increasing since the session started", so
 *     the difference of two readings is the increment that happened in
 *     between.  `kGauge` means "an instantaneous value" (a bitrate, a
 *     jitter, a queue depth), so the difference of two readings is
 *     *meaningless* — the value at the later reading is the answer.
 *     Getting this wrong is the classic statistics bug (a host
 *     subtracting two EMA bitrates and reporting a negative rate), so
 *     the semantics travel with the number and the delta API refuses
 *     to diff a gauge.
 *
 *   - `StatSnapshot` — an immutable, self-contained reading of the
 *     registry at one instant.  It carries the **monotonic timestamp**
 *     of the reading and a **monotonically increasing sequence
 *     number**, which is what makes "the interval between these two
 *     readings" a well-defined thing.  A snapshot owns its data, so a
 *     host may keep it after the engine is destroyed.
 *
 *   - `StatsDelta` — the difference between two readings: the per
 *     counter increments, plus the interval's duration.  This is the
 *     "interval semantics" the design plan asks for: a host can read
 *     a rate straight out of it.
 *
 * ## The registry
 *
 * `StatsRegistry` is the mutable side: entries are declared once
 * (name + semantics) and their values are updated as the session
 * runs.  Taking a snapshot does **not** mutate the registry's values;
 * it only advances the sequence number.
 *
 * The registry is an **instance**, not a singleton: Layout Invariant 6
 * forbids a module from exporting global mutable state, and a
 * registry of session statistics is state.  Two engines in one
 * process get two registries.
 *
 * ## Threading
 *
 * `StatsRegistry` is **not** internally synchronised.  Its owner
 * serialises access (the engine holds a mutex around the projection).
 * The alternative — a mutex inside every core type — would tax the
 * hot path for a surface that is polled, not pushed.
 *
 * ## Naming
 *
 * Entry names are plain field names (`"video_frames_sent"`), grouped
 * by an optional group string.  The empty group is the engine-global
 * scope; a future per-object surface uses a group such as
 * `"inbound-rtp"` or `"candidate-pair"`.
 *
 * @see docs/plan/v1.2-next-plan.md §3 Track S (S1)
 * @see docs/plan/stats-surface-plan.md §3 DS-1
 */

#ifndef NIMRTC_CORE_STATS_REGISTRY_HPP
#define NIMRTC_CORE_STATS_REGISTRY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nimrtc/core/api_version.hpp>

namespace nimrtc::core {

// -----------------------------------------------------------------------------
// StatSemantics
// -----------------------------------------------------------------------------

/**
 * @brief How a statistic behaves over time.
 *
 * The distinction is the whole point of the registry: it tells a host
 * whether the difference between two readings is meaningful.
 */
enum class NIMRTC_API_STABLE StatSemantics : std::uint8_t {
    /**
     * A monotonically increasing counter, counted from the start of
     * the session.  `to - from` is the increment that happened between
     * the two readings.
     */
    kCumulative = 0,
    /**
     * An instantaneous value (a bitrate, a jitter, a queue depth).
     * The difference of two readings is **meaningless**; the value at
     * the later reading is the answer.
     */
    kGauge = 1,
};

/// True when @p semantics is `kCumulative` (i.e. the value is diffable).
NIMRTC_API_STABLE inline bool stat_is_cumulative(StatSemantics semantics) noexcept {
    return semantics == StatSemantics::kCumulative;
}

// -----------------------------------------------------------------------------
// StatEntry
// -----------------------------------------------------------------------------

/**
 * @brief One named statistic, as it appears in a snapshot.
 */
struct NIMRTC_API_STABLE StatEntry {
    /// The group the entry belongs to ("" = the engine-global scope).
    std::string group;
    /// The entry's name (e.g. "video_frames_sent").
    std::string name;
    /// The value at the snapshot's instant.
    std::int64_t value = 0;
    /// How the value behaves over time.
    StatSemantics semantics = StatSemantics::kCumulative;

    /// The fully-qualified key ("group/name", or "name" when the
    /// group is empty).
    std::string key() const {
        if (group.empty()) return name;
        return group + "/" + name;
    }
};

// -----------------------------------------------------------------------------
// StatSnapshot
// -----------------------------------------------------------------------------

/**
 * @brief An immutable reading of a statistics registry at one instant.
 *
 * A snapshot carries:
 *
 *   * the **timestamp** of the reading, in microseconds on whatever
 *     monotonic clock the owner used (the engine uses its steady
 *     clock, so it is directly comparable with the engine's other
 *     timing surfaces);
 *   * the **sequence number** of the reading, which increases by one
 *     per reading and therefore orders two readings even when their
 *     timestamps are identical (a coarse clock, or two readings taken
 *     within the same microsecond);
 *   * the **values** themselves.
 *
 * The snapshot owns its data: it stays valid after the registry (and
 * the engine) is destroyed.
 */
class NIMRTC_API_STABLE StatSnapshot {
public:
    StatSnapshot() = default;

    /// The sequence number of this reading (0 = "no reading").
    NIMRTC_API_STABLE std::uint64_t seq() const noexcept { return seq_; }

    /// The timestamp of this reading, in microseconds.
    NIMRTC_API_STABLE std::int64_t time_us() const noexcept { return time_us_; }

    /// The entries, in declaration order.
    NIMRTC_API_STABLE const std::vector<StatEntry>& entries() const noexcept {
        return *entries_;
    }

    /// The number of entries.
    NIMRTC_API_STABLE std::size_t size() const noexcept {
        return entries_ ? entries_->size() : 0u;
    }

    /// True when this snapshot holds no entries (a default-constructed
    /// snapshot, or a registry with nothing declared).
    NIMRTC_API_STABLE bool empty() const noexcept { return size() == 0u; }

    /// The entry named @p name in @p group.
    ///
    /// @return false when there is no such entry.
    NIMRTC_API_STABLE bool find(std::string_view group,
                                std::string_view name,
                                StatEntry& out) const noexcept;

    /// The value of the entry named @p name in @p group.
    ///
    /// @return false when there is no such entry (the output is then
    ///         left untouched).
    NIMRTC_API_STABLE bool value_of(std::string_view group,
                                     std::string_view name,
                                     std::int64_t& out) const noexcept;

private:
    friend class StatsRegistry;

    std::uint64_t seq_ = 0;
    std::int64_t time_us_ = 0;
    // Shared and immutable: copying a snapshot is cheap, and the data
    // outlives the registry that produced it.
    std::shared_ptr<const std::vector<StatEntry>> entries_ =
        std::make_shared<const std::vector<StatEntry>>();
};

// -----------------------------------------------------------------------------
// StatDeltaEntry
// -----------------------------------------------------------------------------

/**
 * @brief One counter's increment over an interval.
 */
struct NIMRTC_API_STABLE StatDeltaEntry {
    /// The group the entry belongs to ("" = the engine-global scope).
    std::string group;
    /// The entry's name.
    std::string name;
    /// The value at the beginning of the interval.
    std::int64_t from = 0;
    /// The value at the end of the interval.
    std::int64_t to = 0;
    /// `to - from` for a cumulative counter; `to` for a gauge (a
    /// gauge has no meaningful increment).
    std::int64_t delta = 0;
    /// How the value behaves over time.
    StatSemantics semantics = StatSemantics::kCumulative;
};

// -----------------------------------------------------------------------------
// StatsDelta
// -----------------------------------------------------------------------------

/**
 * @brief The difference between two readings.
 *
 * The delta is the "interval semantics" the statistics surface needs:
 * a host that keeps the previous reading can ask the engine for the
 * increment over the interval instead of keeping its own copy of
 * every counter.
 *
 * A gauge is **not** diffed.  `counter_delta()` refuses to answer for
 * a gauge and returns false, because the answer would be a
 * meaningless number (the difference of two instantaneous bitrates is
 * not a bitrate).  `gauge_value()` answers for a gauge instead: it
 * returns the value at the end of the interval.
 */
class NIMRTC_API_STABLE StatsDelta {
public:
    StatsDelta() = default;

    /// The sequence number of the interval's beginning.
    NIMRTC_API_STABLE std::uint64_t from_seq() const noexcept { return from_seq_; }
    /// The sequence number of the interval's end.
    NIMRTC_API_STABLE std::uint64_t to_seq() const noexcept { return to_seq_; }
    /// The timestamp of the interval's beginning, in microseconds.
    NIMRTC_API_STABLE std::int64_t from_time_us() const noexcept { return from_time_us_; }
    /// The timestamp of the interval's end, in microseconds.
    NIMRTC_API_STABLE std::int64_t to_time_us() const noexcept { return to_time_us_; }

    /// The length of the interval, in microseconds (never negative).
    NIMRTC_API_STABLE std::int64_t duration_us() const noexcept;

    /// True when the delta has a usable baseline: both readings
    /// carry entries and the end follows the beginning.
    NIMRTC_API_STABLE bool valid() const noexcept { return valid_; }

    /// The increment of the cumulative counter named @p name in
    /// @p group.
    ///
    /// @return false when there is no such entry, when the entry is a
    ///         gauge (a gauge has no increment), or when the delta has
    ///         no usable baseline.
    NIMRTC_API_STABLE bool counter_delta(std::string_view group,
                                         std::string_view name,
                                         std::int64_t& out) const noexcept;

    /// The value of the gauge named @p name in @p group at the end of
    /// the interval.
    ///
    /// @return false when there is no such entry, when the entry is a
    ///         cumulative counter, or when the delta has no usable
    ///         baseline.
    NIMRTC_API_STABLE bool gauge_value(std::string_view group,
                                       std::string_view name,
                                       std::int64_t& out) const noexcept;

    /// The per-counter increments of the interval.
    NIMRTC_API_STABLE const std::vector<StatDeltaEntry>& entries() const noexcept {
        return entries_;
    }

private:
    friend class StatsRegistry;

    std::uint64_t from_seq_ = 0;
    std::uint64_t to_seq_ = 0;
    std::int64_t from_time_us_ = 0;
    std::int64_t to_time_us_ = 0;
    bool valid_ = false;
    std::vector<StatDeltaEntry> entries_;
};

// -----------------------------------------------------------------------------
// StatLookup
// -----------------------------------------------------------------------------

/**
 * @brief A source of statistic values, addressed by group and name.
 *
 * The delta machinery needs to read a baseline that may live in a
 * snapshot (a host's own copy) rather than in the registry.  A lookup
 * is the abstraction over both.
 *
 * The callback returns false when it has no value for the entry.
 */
using NIMRTC_API_STABLE StatLookup =
    std::function<bool(std::string_view group, std::string_view name, std::int64_t& out)>;

// -----------------------------------------------------------------------------
// StatsRegistry
// -----------------------------------------------------------------------------

/**
 * @brief The mutable side of the statistics surface: a set of named
 *        entries whose values are updated as a session runs.
 *
 * An entry is **declared** once (its name, its group and its
 * semantics) and its value is then **set** as often as the owner
 * likes.  Declaring an entry twice is a programming error:
 * `declare()` returns false and keeps the first declaration.
 *
 * The registry is **not** internally synchronised; its owner
 * serialises access.
 */
class NIMRTC_API_STABLE StatsRegistry {
public:
    StatsRegistry() = default;

    /**
     * @brief Declare an entry.
     *
     * @param group      The group the entry belongs to ("" for the
     *                   engine-global scope).
     * @param name       The entry's name.
     * @param semantics  How the value behaves over time.
     *
     * @return true when the entry was declared; false when it already
     *         existed (its semantics are then left as they were).
     */
    NIMRTC_API_STABLE bool declare(std::string_view group,
                                   std::string_view name,
                                   StatSemantics semantics);

    /**
     * @brief Set an entry's value.
     *
     * @return false when the entry was never declared.
     */
    NIMRTC_API_STABLE bool set(std::string_view group,
                               std::string_view name,
                               std::int64_t value);

    /// The value of an entry (0 when it does not exist).
    NIMRTC_API_STABLE std::int64_t value_of(std::string_view group,
                                            std::string_view name) const noexcept;

    /// The number of declared entries.
    NIMRTC_API_STABLE std::size_t entry_count() const noexcept { return entries_.size(); }

    /// The sequence number of the most recent reading (0 = none yet).
    NIMRTC_API_STABLE std::uint64_t snapshot_seq() const noexcept { return seq_; }

    /**
     * @brief The declaration index of an entry.
     *
     * The index is stable for the lifetime of the registry, so an
     * owner that updates the same set of entries on every tick can
     * resolve the indices once and then use `set_at()`.
     *
     * @return The index, or `npos` when the entry does not exist.
     */
    NIMRTC_API_STABLE std::size_t index_of(std::string_view group,
                                            std::string_view name) const noexcept;

    /// The value of the entry at @p index (0 when the index is out of
    /// range).
    NIMRTC_API_STABLE std::int64_t value_at(std::size_t index) const noexcept;

    /**
     * @brief Set the entry at @p index.
     *
     * @return false when the index is out of range.
     */
    NIMRTC_API_STABLE bool set_at(std::size_t index, std::int64_t value);

    /**
     * @brief Advance the sequence number without taking a reading.
     *
     * A caller that only needs to *order* two observations (rather
     * than read the values) can advance the counter instead of paying
     * for a snapshot copy.  The returned number is the new sequence
     * number.
     */
    NIMRTC_API_STABLE std::uint64_t advance(std::int64_t time_us);

    /**
     * @brief Take a reading of the registry.
     *
     * Advances the sequence number by one and copies the current
     * values into the returned snapshot.
     *
     * @param time_us  The timestamp of the reading, in microseconds
     *                 on the owner's monotonic clock.  The registry
     *                 does not read a clock itself: the owner knows
     *                 which timebase its own timings are expressed
     *                 in, and the two must be comparable.
     */
    NIMRTC_API_STABLE StatSnapshot snapshot(std::int64_t time_us);

    /**
     * @brief A reading of the registry that does not advance the
     *        sequence number.
     *
     * The caller owns the sequence number (it may have assigned one
     * through `advance()` already), so this is the way to take a
     * snapshot whose sequence number matches another view of the same
     * instant.
     */
    NIMRTC_API_STABLE StatSnapshot snapshot_at(std::uint64_t seq,
                                               std::int64_t time_us) const;

    /**
     * @brief The difference between a previous reading and the
     *        registry's current state.
     *
     * Takes a fresh reading (so the sequence number advances) and
     * diffs it against @p previous.
     *
     * @param previous  The earlier reading.  A default-constructed
     *                  snapshot means "no baseline": the returned
     *                  delta is then invalid (see `valid()`).
     */
    NIMRTC_API_STABLE StatsDelta delta_since(const StatSnapshot& previous,
                                             std::int64_t time_us);

    /**
     * @brief The difference between two arbitrary sources of values.
     *
     * The general form of `delta_since()`: the baseline does not have
     * to be a snapshot, so an owner can diff a host's *own* copy of
     * the flat statistics against the registry's current values.
     *
     * The registry's declared entries define the shape of the delta:
     * every declared entry produces one `StatDeltaEntry`.
     */
    NIMRTC_API_STABLE StatsDelta diff(std::uint64_t from_seq,
                                      std::int64_t from_time_us,
                                      const StatLookup& from,
                                      std::uint64_t to_seq,
                                      std::int64_t to_time_us,
                                      const StatLookup& to) const;

private:
    struct Entry {
        std::string group;
        std::string name;
        StatSemantics semantics = StatSemantics::kCumulative;
        std::int64_t value = 0;
    };

    // The lookup key is "group\0name"; the group is empty for the
    // engine-global scope.
    static std::string make_key(std::string_view group, std::string_view name);

    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
    std::uint64_t seq_ = 0;
};

// -----------------------------------------------------------------------------
// Out-of-line definitions
// -----------------------------------------------------------------------------

inline NIMRTC_API_STABLE bool StatSnapshot::find(std::string_view group,
                                                std::string_view name,
                                                StatEntry& out) const noexcept {
    if (!entries_) return false;
    for (const auto& e : *entries_) {
        if (e.name == name && e.group == group) {
            out = e;
            return true;
        }
    }
    return false;
}

inline bool StatSnapshot::value_of(std::string_view group,
                                   std::string_view name,
                                   std::int64_t& out) const noexcept {
    if (!entries_) return false;
    for (const auto& e : *entries_) {
        if (e.name == name && e.group == group) {
            out = e.value;
            return true;
        }
    }
    return false;
}

inline std::int64_t StatsDelta::duration_us() const noexcept {
    if (to_time_us_ <= from_time_us_) return 0;
    return to_time_us_ - from_time_us_;
}

inline bool StatsDelta::counter_delta(std::string_view group,
                                      std::string_view name,
                                      std::int64_t& out) const noexcept {
    if (!valid_) return false;
    for (const auto& e : entries_) {
        if (e.name == name && e.group == group) {
            // A gauge has no increment: the difference of two
            // instantaneous values is not a rate.
            if (e.semantics != StatSemantics::kCumulative) return false;
            out = e.delta;
            return true;
        }
    }
    return false;
}

inline bool StatsDelta::gauge_value(std::string_view group,
                                    std::string_view name,
                                    std::int64_t& out) const noexcept {
    if (!valid_) return false;
    for (const auto& e : entries_) {
        if (e.name == name && e.group == group) {
            if (e.semantics != StatSemantics::kGauge) return false;
            out = e.to;
            return true;
        }
    }
    return false;
}

inline std::string StatsRegistry::make_key(std::string_view group,
                                           std::string_view name) {
    std::string key;
    key.reserve(group.size() + name.size() + 1);
    key.append(group);
    key.push_back('\0');
    key.append(name);
    return key;
}

inline bool StatsRegistry::declare(std::string_view group,
                                   std::string_view name,
                                   StatSemantics semantics) {
    if (name.empty()) return false;
    const std::string key = make_key(group, name);
    auto it = index_.find(key);
    if (it != index_.end()) return false;  // already declared
    index_.emplace(key, entries_.size());
    Entry e;
    e.group.assign(group);
    e.name.assign(name);
    e.semantics = semantics;
    entries_.push_back(std::move(e));
    return true;
}

inline bool StatsRegistry::set(std::string_view group,
                               std::string_view name,
                               std::int64_t value) {
    const std::string key = make_key(group, name);
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    entries_[it->second].value = value;
    return true;
}

inline std::int64_t StatsRegistry::value_of(std::string_view group,
                                            std::string_view name) const noexcept {
    const std::string key = make_key(group, name);
    auto it = index_.find(key);
    if (it == index_.end()) return 0;
    return entries_[it->second].value;
}

inline std::size_t StatsRegistry::index_of(std::string_view group,
                                            std::string_view name) const noexcept {
    const std::string key = make_key(group, name);
    auto it = index_.find(key);
    if (it == index_.end()) return static_cast<std::size_t>(-1);
    return it->second;
}

inline std::int64_t StatsRegistry::value_at(std::size_t index) const noexcept {
    if (index >= entries_.size()) return 0;
    return entries_[index].value;
}

inline bool StatsRegistry::set_at(std::size_t index, std::int64_t value) {
    if (index >= entries_.size()) return false;
    entries_[index].value = value;
    return true;
}

inline std::uint64_t StatsRegistry::advance(std::int64_t time_us) {
    (void)time_us;
    return ++seq_;
}

inline StatSnapshot StatsRegistry::snapshot(std::int64_t time_us) {
    ++seq_;
    return snapshot_at(seq_, time_us);
}

inline StatSnapshot StatsRegistry::snapshot_at(std::uint64_t seq,
                                               std::int64_t time_us) const {
    auto data = std::make_shared<std::vector<StatEntry>>();
    data->reserve(entries_.size());
    for (const auto& e : entries_) {
        StatEntry out;
        out.group = e.group;
        out.name = e.name;
        out.value = e.value;
        out.semantics = e.semantics;
        data->push_back(std::move(out));
    }

    StatSnapshot snap;
    snap.seq_ = seq;
    snap.time_us_ = time_us;
    snap.entries_ = std::move(data);
    return snap;
}

inline StatsDelta StatsRegistry::delta_since(const StatSnapshot& previous,
                                             std::int64_t time_us) {
    const std::uint64_t to_seq = advance(time_us);

    StatLookup from = [&previous](std::string_view group, std::string_view name,
                                  std::int64_t& out) {
        return previous.value_of(group, name, out);
    };
    StatLookup to = [this](std::string_view group, std::string_view name,
                           std::int64_t& out) {
        out = value_of(group, name);
        return true;
    };
    return diff(previous.seq(), previous.time_us(), from, to_seq, time_us, to);
}

inline StatsDelta StatsRegistry::diff(std::uint64_t from_seq,
                                      std::int64_t from_time_us,
                                      const StatLookup& from,
                                      std::uint64_t to_seq,
                                      std::int64_t to_time_us,
                                      const StatLookup& to) const {
    StatsDelta d;
    d.from_seq_ = from_seq;
    d.to_seq_ = to_seq;
    d.from_time_us_ = from_time_us;
    d.to_time_us_ = to_time_us;

    // A usable baseline needs entries on both sides, and the end must
    // not precede the beginning.
    if (entries_.empty() || to_seq < from_seq) {
        d.valid_ = false;
        return d;
    }

    bool complete = true;
    d.entries_.reserve(entries_.size());
    for (const auto& def : entries_) {
        std::int64_t to_value = 0;
        if (!to(def.group, def.name, to_value)) continue;

        StatDeltaEntry e;
        e.group = def.group;
        e.name = def.name;
        e.semantics = def.semantics;
        e.to = to_value;

        std::int64_t from_value = 0;
        if (!from(def.group, def.name, from_value)) {
            // No baseline for this entry: the delta would be
            // measured against a made-up zero.
            complete = false;
            from_value = 0;
        }
        e.from = from_value;

        if (def.semantics == StatSemantics::kCumulative) {
            // A counter never goes backwards.  If it somehow did, the
            // session was restarted under the host, and reporting a
            // negative increment would be a lie; report zero instead.
            e.delta = (e.to >= e.from) ? (e.to - e.from) : 0;
        } else {
            // A gauge has no increment.
            e.delta = e.to;
        }
        d.entries_.push_back(std::move(e));
    }
    d.valid_ = complete;
    return d;
}

}  // namespace nimrtc::core

#endif  // NIMRTC_CORE_STATS_REGISTRY_HPP
