/**
 * @file nimrtc/audio_device/ring_buffer.hpp
 * @brief The shared bounded ring buffer between the device thread and the host.
 *
 * Every audio device backend (PortAudio, the synthetic device, and any
 * platform backend a host writes later) has the same shape:
 *
 *   - the **device thread** produces or consumes blocks of PCM at the
 *     device's own pace (a 10 ms quantum, typically),
 *   - the **host thread** produces (capture) or consumes (playback)
 *     blocks at the *engine's* pace.
 *
 * Between the two there has to be a buffer, and it has to be bounded:
 * an unbounded one turns a slow consumer into unbounded latency instead
 * of a dropped block.
 *
 * ## Why a mutex and not a lock-free ring
 *
 * The obvious lock-free design is a single-producer / single-consumer
 * ring with two atomic indices.  It is the wrong choice *here*, and the
 * reason is the overflow policy.
 *
 * The policy is "when the buffer is full, drop the **oldest** block".
 * In a lock-free SPSC ring the producer owns the write index and the
 * consumer owns the read index: to make room, the producer has to
 * advance the *consumer's* index, which means overwriting a slot the
 * consumer may be copying out of at that very moment.  That is a data
 * race — undefined behaviour, and in practice an audible tear.
 *
 * A mutex makes the overflow atomic with respect to both sides: the
 * producer evicts the oldest frames and appends the new ones inside one
 * critical section, and the consumer copies out inside another.  The
 * lock is held for a memcpy of one quantum (a few hundred floats),
 * which is orders of magnitude below the audio deadline.
 *
 * (The alternative — refusing the *new* frames — would be lock-free but
 * wrong for audio: a playback queue that refuses new samples plays
 * stale audio, and a capture queue that refuses new samples drops the
 * most recent speech.)
 *
 * ## Storage
 *
 * The buffer owns a flat `std::vector<float>` and works in *frames*
 * (a frame = one sample per channel).  The channel count is fixed at
 * construction, so a frame is `channels` contiguous floats.
 *
 * @note Remote-desktop plan F2. The portable half of the device layer.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace nimrtc::audio_device {

/**
 * @brief A bounded, thread-safe ring buffer of interleaved float PCM.
 *
 * The producer calls `write()`; the consumer calls `read()`.  The two
 * may be different threads (the normal case: the device thread and the
 * host thread) or the same one (a purely synchronous device).
 *
 * All the query methods are safe to call from any thread.
 */
class NIMRTC_API_STABLE AudioRingBuffer {
public:
    /**
     * Construct a buffer.
     *
     * @param capacity_frames The buffer holds at most this many frames.
     *                         A frame is one sample *per channel*.
     * @param channels        The channel count (1 = mono, 2 = stereo).
     */
    AudioRingBuffer(std::size_t capacity_frames, std::uint16_t channels) noexcept;

    AudioRingBuffer(const AudioRingBuffer&)            = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    // ---- Producer side ----------------------------------------------------

    /**
     * Write frames into the buffer.
     *
     * When the buffer does not have room for the whole block, the
     * **oldest** frames are evicted to make space and the drop is
     * counted.  The call never blocks and never fails.
     *
     * @param samples     The interleaved float PCM.
     * @param num_frames  The number of frames (samples per channel).
     * @return The number of frames actually written.  It is less than
     *         @p num_frames only when @p num_frames exceeds the
     *         buffer's total capacity (the excess is discarded).
     */
    std::size_t write(const float* samples, std::size_t num_frames) noexcept;

    // ---- Consumer side ----------------------------------------------------

    /**
     * Read frames out of the buffer.
     *
     * @param dst        Receives the interleaved float PCM.
     * @param max_frames The number of frames @p dst can hold.
     * @return The number of frames actually read (0 when the buffer is
     *         empty).
     */
    std::size_t read(float* dst, std::size_t max_frames) noexcept;

    /** Discard every buffered frame. */
    void clear() noexcept;

    // ---- Queries ----------------------------------------------------------

    /** The number of frames currently buffered. */
    [[nodiscard]] std::size_t available() const noexcept;

    /** The number of frames the buffer can hold. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_frames_; }

    /** The channel count. */
    [[nodiscard]] std::uint16_t channels() const noexcept { return channels_; }

    /** The number of frames dropped (overwritten) since construction. */
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept;

    /** The number of frames written since construction. */
    [[nodiscard]] std::uint64_t written_frames() const noexcept;

    /** The number of frames read since construction. */
    [[nodiscard]] std::uint64_t read_frames() const noexcept;

private:
    /** The number of floats one frame occupies. */
    [[nodiscard]] std::size_t frame_stride() const noexcept { return channels_; }

    mutable std::mutex mutex_;
    std::vector<float> storage_;
    std::size_t capacity_frames_ = 0;
    std::uint16_t channels_ = 1;

    // The ring's read and write positions, in frames.  `write_pos_` is
    // always >= `read_pos_`, and `write_pos_ - read_pos_` is the number
    // of frames currently buffered.
    std::size_t read_pos_  = 0;
    std::size_t write_pos_ = 0;

    std::uint64_t dropped_     = 0;
    std::uint64_t written_    = 0;
    std::uint64_t read_count_  = 0;
};

} // namespace nimrtc::audio_device
