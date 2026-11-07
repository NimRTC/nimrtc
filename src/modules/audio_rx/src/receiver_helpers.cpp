/**
 * @file src/modules/audio_rx/src/receiver_helpers.cpp
 * @brief Helpers of the audio receive pipeline (v1.2.0, audio pipeline WP-3).
 *
 * Two helpers live here:
 *
 * * `downmix_to()` — the channel mix of spec B-9.  It is rate-preserving:
 *   the sample count per channel never changes, only the number of
 *   channels does.  Chrome negotiates Opus as stereo while NimRTC's own
 *   capture side is mono, so the mix is the normal case, not an
 *   exception.
 * * `rtp_units_per_frame()` — the step of the media timeline in RTP clock
 *   units (960 at 48 kHz / 20 ms).
 */

#include "receiver_internal.hpp"

namespace nimrtc::audio_rx {

std::uint32_t rtp_units_per_frame(std::uint32_t clock_rate_hz,
                                   std::uint16_t frame_duration_ms) noexcept {
    if (clock_rate_hz == 0) return 0;
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(clock_rate_hz) * frame_duration_ms) / 1000u);
}

void downmix_to(const float* in, std::size_t samples_per_channel,
                std::uint8_t in_channels, std::uint8_t out_channels,
                std::vector<float>& out) {
    if (!in || samples_per_channel == 0 || in_channels == 0) {
        out.clear();
        return;
    }

    const std::uint8_t target = out_channels ? out_channels : in_channels;

    if (target == in_channels) {
        // Nothing to mix — keep the signal as it is.
        out.assign(in, in + samples_per_channel * in_channels);
        return;
    }

    out.assign(samples_per_channel * target, 0.0f);
    for (std::size_t s = 0; s < samples_per_channel; ++s) {
        for (std::uint8_t o = 0; o < target; ++o) {
            // The source channels that belong to this output channel.
            std::size_t first = (static_cast<std::size_t>(o) * in_channels) / target;
            std::size_t last =
                (static_cast<std::size_t>(o + 1) * in_channels) / target;
            if (last <= first) {
                // Fewer source channels than output channels: the output
                // channel repeats the nearest source channel.
                first = (first < in_channels) ? first : (in_channels - 1);
                last = first + 1;
            }
            float sum = 0.0f;
            for (std::size_t c = first; c < last; ++c) {
                sum += in[s * in_channels + c];
            }
            out[s * target + o] = sum / static_cast<float>(last - first);
        }
    }
}

} // namespace nimrtc::audio_rx
