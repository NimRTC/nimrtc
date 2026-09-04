#pragma once

#include <chrono>

namespace nimrtc::core {

// -----------------------------------------------------------------------------
// Duration aliases
// -----------------------------------------------------------------------------
using Nanoseconds  = std::chrono::nanoseconds;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;
using Seconds      = std::chrono::seconds;

// -----------------------------------------------------------------------------
// Time points
// -----------------------------------------------------------------------------
// Steady / monotonic clock — for jitter measurement, scheduling, internal timing.
using SteadyClock  = std::chrono::steady_clock;
using TimePoint    = SteadyClock::time_point;

// Wall clock — for logging, NTP-aligned timestamps.
using SystemClock  = std::chrono::system_clock;
using SystemTimePoint = SystemClock::time_point;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
constexpr TimePoint epoch() noexcept { return TimePoint{}; }

constexpr Nanoseconds to_nanoseconds(std::int64_t ns) noexcept { return Nanoseconds{ns}; }
constexpr Microseconds to_microseconds(std::int64_t us) noexcept { return Microseconds{us}; }
constexpr Milliseconds to_milliseconds(std::int64_t ms) noexcept { return Milliseconds{ms}; }

// Duration cast helper — used heavily in RTP/RTCP and jitter buffers.
template<class From, class To>
constexpr To duration_cast_to(From from) noexcept {
    return std::chrono::duration_cast<To>(from);
}

} // namespace nimrtc::core