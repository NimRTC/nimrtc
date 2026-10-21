#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <chrono>
#include <cstdint>
#include <nimrtc/core/api_version.hpp>

namespace nimrtc::core {

// -----------------------------------------------------------------------------
// Duration aliases
// -----------------------------------------------------------------------------
using NIMRTC_API_STABLE Nanoseconds  = std::chrono::nanoseconds;
using NIMRTC_API_STABLE Microseconds = std::chrono::microseconds;
using NIMRTC_API_STABLE Milliseconds = std::chrono::milliseconds;
using NIMRTC_API_STABLE Seconds      = std::chrono::seconds;

// -----------------------------------------------------------------------------
// Time points
// -----------------------------------------------------------------------------
// Steady / monotonic clock — for jitter measurement, scheduling, internal timing.
using NIMRTC_API_STABLE SteadyClock  = std::chrono::steady_clock;
using NIMRTC_API_STABLE TimePoint    = SteadyClock::time_point;

// Wall clock — for logging, NTP-aligned timestamps.
using NIMRTC_API_STABLE SystemClock  = std::chrono::system_clock;
using NIMRTC_API_STABLE SystemTimePoint = SystemClock::time_point;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
NIMRTC_API_STABLE constexpr TimePoint epoch() noexcept { return TimePoint{}; }

constexpr Nanoseconds to_nanoseconds(std::int64_t ns) noexcept { return Nanoseconds{ns}; }
constexpr Microseconds to_microseconds(std::int64_t us) noexcept { return Microseconds{us}; }
constexpr Milliseconds to_milliseconds(std::int64_t ms) noexcept { return Milliseconds{ms}; }

// Duration cast helper — used heavily in RTP/RTCP and jitter buffers.
template<class From, class To>
constexpr To duration_cast_to(From from) noexcept {
    return std::chrono::duration_cast<To>(from);
}

} // namespace nimrtc::core