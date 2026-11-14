// audio_device/src/ring_buffer.cpp
// The bounded ring buffer — see include/nimrtc/audio_device/ring_buffer.hpp.

#include <nimrtc/audio_device/ring_buffer.hpp>

#include <algorithm>
#include <cstring>

namespace nimrtc::audio_device {

AudioRingBuffer::AudioRingBuffer(std::size_t capacity_frames,
                                 std::uint16_t channels) noexcept
    : capacity_frames_(capacity_frames == 0 ? 1 : capacity_frames),
      channels_(channels == 0 ? 1 : channels) {
    storage_.assign(capacity_frames_ * frame_stride(), 0.0f);
}

std::size_t AudioRingBuffer::write(const float* samples,
                                   std::size_t num_frames) noexcept {
    if (samples == nullptr || num_frames == 0) {
        return 0;
    }

    const std::size_t stride = frame_stride();

    // A block larger than the whole buffer can never fit: only the last
    // `capacity_frames_` frames of the block survive.  Keeping the
    // *tail* is what matters for audio: the tail is the most recent
    // data, and the head is what the consumer would have wanted first
    // (and already missed).
    const std::size_t to_write = std::min(num_frames, capacity_frames_);

    std::lock_guard<std::mutex> lock(mutex_);

    const std::size_t used = write_pos_ - read_pos_;

    // Make room by evicting the oldest frames.
    if (used + to_write > capacity_frames_) {
        const std::size_t evicted = used + to_write - capacity_frames_;
        read_pos_ += evicted;
        dropped_ += evicted;
    }

    // The surviving part of the block starts `num_frames - to_write`
    // frames into the caller's buffer.
    const float* src = samples + (num_frames - to_write) * stride;

    // Copy the block, wrapping around the end of the storage.
    const std::size_t start = write_pos_ % capacity_frames_;
    const std::size_t first = std::min(to_write, capacity_frames_ - start);
    std::memcpy(storage_.data() + start * stride,
                src,
                first * stride * sizeof(float));
    if (to_write > first) {
        std::memcpy(storage_.data(),
                    src + first * stride,
                    (to_write - first) * stride * sizeof(float));
    }

    write_pos_ += to_write;
    written_ += to_write;
    return to_write;
}

std::size_t AudioRingBuffer::read(float* dst, std::size_t max_frames) noexcept {
    if (dst == nullptr || max_frames == 0) {
        return 0;
    }

    const std::size_t stride = frame_stride();

    std::lock_guard<std::mutex> lock(mutex_);

    const std::size_t used = write_pos_ - read_pos_;
    if (used == 0) {
        return 0;
    }

    const std::size_t to_read = std::min(max_frames, used);

    const std::size_t start = read_pos_ % capacity_frames_;
    const std::size_t first = std::min(to_read, capacity_frames_ - start);
    std::memcpy(dst,
                storage_.data() + start * stride,
                first * stride * sizeof(float));
    if (to_read > first) {
        std::memcpy(dst + first * stride,
                    storage_.data(),
                    (to_read - first) * stride * sizeof(float));
    }

    read_pos_ += to_read;
    read_count_ += to_read;
    return to_read;
}

void AudioRingBuffer::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    read_pos_ = write_pos_;
}

std::size_t AudioRingBuffer::available() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return write_pos_ - read_pos_;
}

std::uint64_t AudioRingBuffer::dropped_frames() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

std::uint64_t AudioRingBuffer::written_frames() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return written_;
}

std::uint64_t AudioRingBuffer::read_frames() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return read_count_;
}

} // namespace nimrtc::audio_device
