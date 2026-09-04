#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/core/time.hpp>

#include <nimrtc/rtp/packet.hpp>

// =============================================================================
// nimrtc::jb
// -----------------------------------------------------------------------------
// Adaptive jitter buffer (RFC 3550-style) with NimRTC's ref_frame hook
// (see docs/zh/NimRTC-V2-技术文档.md §8.4 differentiation #3).
//
// P0 contract: data structures + push/pop API + Stats.
// Implementation lands in P2 (Agent 3).
// =============================================================================
namespace nimrtc::jb {

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------
struct Config {
    core::Milliseconds initial_delay = core::Milliseconds{50};
    core::Milliseconds min_delay     = core::Milliseconds{20};
    core::Milliseconds max_delay     = core::Milliseconds{400};

    // Hard cap on buffered packets (FIFO trim from oldest when exceeded).
    std::size_t        max_packets   = 512;

    // Adaptive mode: target_delay tracks recent jitter estimate between
    // min_delay and max_delay. If false, target_delay == initial_delay.
    bool               adaptive      = true;
};

// -----------------------------------------------------------------------------
// Frame — output unit.
// A frame may consist of one or more RTP packets sharing an RTP timestamp.
// -----------------------------------------------------------------------------
struct Frame {
    std::vector<rtp::PacketView> packets;
    std::uint32_t    frame_timestamp    = 0;       // RTP timestamp of first packet
    core::Nanoseconds capture_latency   = {};      // now - first arrival (sanity check)
    std::optional<core::TimePoint> first_arrival;  // raw monotonic arrival of first packet

    // §8.4 — caller pre-assigns a ref_frame id. jb copies this through so
    // downstream components (decode, mux, decision layer) can correlate a
    // network frame with a decision frame.
    std::optional<std::uint64_t> ref_frame;
};

// -----------------------------------------------------------------------------
// Stats — observability for /stats endpoint and tests.
// -----------------------------------------------------------------------------
struct Stats {
    std::size_t          buffered_packets = 0;
    std::size_t          frames_emitted   = 0;
    std::size_t          packets_dropped  = 0;
    core::Milliseconds   current_delay    = {};
    core::Milliseconds   current_jitter   = {};
};

// -----------------------------------------------------------------------------
// JitterBuffer
// -----------------------------------------------------------------------------
class JitterBuffer {
public:
    JitterBuffer();                              // uses default Config
    explicit JitterBuffer(Config config);
    ~JitterBuffer();

    JitterBuffer(const JitterBuffer&)            = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;
    JitterBuffer(JitterBuffer&&)                 noexcept;
    JitterBuffer& operator=(JitterBuffer&&)      noexcept;

    // Insert a parsed RTP packet. `arrival` is the monotonic time the packet
    // was *received* off the wire (network thread), not the time it was
    // dequeued onto the worker. Setting ref_frame here lets the jitter
    // buffer correlate network ↔ decision frames (see §8.4).
    void push(rtp::PacketView packet,
              core::TimePoint arrival,
              std::optional<std::uint64_t> ref_frame = std::nullopt);

    // Pull the next frame whose playout deadline is <= `now`. Returns
    // std::nullopt if no frame is ready yet.
    std::optional<Frame> pop(core::TimePoint now);

    // Force-emit every buffered frame in arrival order. Use on stream
    // end / ICE disconnect / reset().
    std::vector<Frame> flush();

    // Discard all buffered state.
    void reset() noexcept;

    Stats stats() const noexcept;

    // Live reconfigure. Thread-safety: caller ensures no concurrent push/pop.
    void update_config(Config config);

    Config config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::jb