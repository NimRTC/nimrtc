/**
 * @file src/plugins/src/hw_video_backend.cpp
 * @brief HwVideoBackendRegistry implementation + auto-load entry point.
 */

#include <nimrtc/plugins/hw_video_backend.hpp>

#include <algorithm>
#include <mutex>
#include <string_view>

namespace nimrtc::plugins {

namespace {

// Comparator: higher priority first, stable.
template <typename B>
bool by_priority_desc(const B* a, const B* b) noexcept {
    return a->priority > b->priority;
}

// Null-check helper.
//
// The `preference` filter is the plan's B2 knob:
//   - `kAuto`     — every backend is eligible (hardware first, because the
//                   priority order puts hardware above software).
//   - `kHardware` — only `is_hw` backends are eligible.  When none is
//                   available the selection *fails* (returns nullptr),
//                   because silently handing back a software backend
//                   would defeat the point of asking for hardware.
//   - `kSoftware` — only the non-hardware backends are eligible.
//
// The filter is applied to the *explicit* preferred id as well: naming
// a software backend while asking for hardware is a contradiction, and
// the honest answer to that is "no match", not a silent override.
template <typename B>
bool eligible_for_preference(const B& b,
                             VideoCodecBackendPreference preference) noexcept {
    switch (preference) {
        case VideoCodecBackendPreference::kHardware:
            return b.is_hw;
        case VideoCodecBackendPreference::kSoftware:
            return !b.is_hw;
        case VideoCodecBackendPreference::kAuto:
            break;
    }
    return true;
}

template <typename B>
const B* first_match(const std::vector<B>& all,
                     VideoCodecKind kind,
                     std::string_view preferred_id,
                     VideoCodecBackendPreference preference) noexcept {
    // 1. Explicit preferred id (must be available and eligible)
    if (!preferred_id.empty()) {
        for (const auto& b : all) {
            if (b.id != preferred_id) continue;
            if (b.codec_kind != kind) continue;
            if (!eligible_for_preference(b, preference)) continue;
            if (b.available && !b.available(VideoCodecConfig{})) continue;
            return &b;
        }
    }
    // 2. Highest-priority available backend for this kind
    std::vector<const B*> sorted;
    sorted.reserve(all.size());
    for (const auto& b : all) {
        if (b.codec_kind != kind) continue;
        if (!eligible_for_preference(b, preference)) continue;
        if (b.available && !b.available(VideoCodecConfig{})) {
            continue;
        }
        sorted.push_back(&b);
    }
    std::sort(sorted.begin(), sorted.end(), by_priority_desc<B>);
    if (!sorted.empty()) return sorted.front();
    // 3. ANY backend for this kind (even unavailable) — last-resort so caller
    //    sees why selection failed (returns a backend whose `create` will
    //    fail loudly rather than returning nullptr).
    //
    //    This fallback is only reached in `kAuto` mode: in `kHardware` /
    //    `kSoftware` mode an ineligible backend must NOT be handed back,
    //    because that is exactly the silent fallback the mode forbids.
    if (preference != VideoCodecBackendPreference::kAuto) {
        return nullptr;
    }
    for (const auto& b : all) {
        if (b.codec_kind == kind) return &b;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

HwVideoBackendRegistry& HwVideoBackendRegistry::instance() noexcept {
    static HwVideoBackendRegistry inst;
    static std::once_flag once;
    std::call_once(once, [&]{ inst.install_stub_backends(); });
    return inst;
}

void HwVideoBackendRegistry::install_stub_backends() noexcept {
    // Stub encoders + decoders are always present (priority 0). These come
    // from the h264 module — wired in via register_default_video_backends().
    // Nothing to do here by default.
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void HwVideoBackendRegistry::register_encoder(VideoEncoderBackend backend) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    encoders_.push_back(std::move(backend));
}

void HwVideoBackendRegistry::register_decoder(VideoDecoderBackend backend) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    decoders_.push_back(std::move(backend));
}

void HwVideoBackendRegistry::register_pair(VideoEncoderBackend enc,
                                          VideoDecoderBackend dec) noexcept {
    register_encoder(std::move(enc));
    register_decoder(std::move(dec));
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

const VideoEncoderBackend*
HwVideoBackendRegistry::select_encoder(VideoCodecKind kind,
                                       std::string_view preferred_id,
                                       VideoCodecBackendPreference preference) const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return first_match(encoders_, kind, preferred_id, preference);
}

const VideoDecoderBackend*
HwVideoBackendRegistry::select_decoder(VideoCodecKind kind,
                                       std::string_view preferred_id,
                                       VideoCodecBackendPreference preference) const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return first_match(decoders_, kind, preferred_id, preference);
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

std::vector<const VideoEncoderBackend*>
HwVideoBackendRegistry::list_encoders(
        std::optional<VideoCodecKind> kind_filter) const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<const VideoEncoderBackend*> out;
    out.reserve(encoders_.size());
    for (const auto& b : encoders_) {
        if (kind_filter && b.codec_kind != *kind_filter) continue;
        out.push_back(&b);
    }
    std::sort(out.begin(), out.end(), by_priority_desc<VideoEncoderBackend>);
    return out;
}

std::vector<const VideoDecoderBackend*>
HwVideoBackendRegistry::list_decoders(
        std::optional<VideoCodecKind> kind_filter) const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<const VideoDecoderBackend*> out;
    out.reserve(decoders_.size());
    for (const auto& b : decoders_) {
        if (kind_filter && b.codec_kind != *kind_filter) continue;
        out.push_back(&b);
    }
    std::sort(out.begin(), out.end(), by_priority_desc<VideoDecoderBackend>);
    return out;
}

// ---------------------------------------------------------------------------
// Test-only
// ---------------------------------------------------------------------------

void HwVideoBackendRegistry::clear_for_tests() noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    encoders_.clear();
    decoders_.clear();
    install_stub_backends();
}

void HwVideoBackendRegistry::clear_for_kind_for_tests(VideoCodecKind kind) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    encoders_.erase(std::remove_if(encoders_.begin(), encoders_.end(),
                                   [kind](const VideoEncoderBackend& b) {
                                       return b.codec_kind == kind;
                                   }),
                    encoders_.end());
    decoders_.erase(std::remove_if(decoders_.begin(), decoders_.end(),
                                   [kind](const VideoDecoderBackend& b) {
                                       return b.codec_kind == kind;
                                   }),
                    decoders_.end());
}

} // namespace nimrtc::plugins
