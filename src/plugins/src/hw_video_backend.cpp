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
template <typename B>
const B* first_match(const std::vector<B>& all,
                     VideoCodecKind kind,
                     std::string_view preferred_id) noexcept {
    // 1. Explicit preferred id (must be available)
    if (!preferred_id.empty()) {
        for (const auto& b : all) {
            if (b.id != preferred_id) continue;
            if (b.codec_kind != kind) continue;
            if (b.available && !b.available(VideoCodecConfig{})) continue;
            return &b;
        }
    }
    // 2. Highest-priority available backend for this kind
    std::vector<const B*> sorted;
    sorted.reserve(all.size());
    for (const auto& b : all) {
        if (b.codec_kind == kind && b.available && b.available(VideoCodecConfig{})) {
            sorted.push_back(&b);
        }
    }
    std::sort(sorted.begin(), sorted.end(), by_priority_desc<B>);
    if (!sorted.empty()) return sorted.front();
    // 3. ANY backend for this kind (even unavailable) — last-resort so caller
    //    sees why selection failed (returns a backend whose `create` will
    //    fail loudly rather than returning nullptr).
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
                                       std::string_view preferred_id) const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return first_match(encoders_, kind, preferred_id);
}

const VideoDecoderBackend*
HwVideoBackendRegistry::select_decoder(VideoCodecKind kind,
                                       std::string_view preferred_id) const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return first_match(decoders_, kind, preferred_id);
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

} // namespace nimrtc::plugins
