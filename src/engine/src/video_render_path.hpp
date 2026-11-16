/**
 * @file src/engine/src/video_render_path.hpp
 * @brief PRIVATE engine collaborator: the decoded-frame delivery-path rule.
 *
 * Architecture remediation plan §4.3 (D1) put the video plane's *state*
 * into named collaborators.  This file is the video plane's *policy* for
 * the receive side: **which delivery path a decoded frame takes**.
 *
 * ## The rule
 *
 * A decoded frame reaches the sink by one of two routes:
 *
 *   - the **GPU path**: the decoder hands back a native surface
 *     (`video_frame::GpuBuffer`) and the sink presents it — the picture
 *     never enters host memory (remote-desktop plan E1 / RD-4);
 *   - the **CPU path**: the decoder writes host pixels and the sink
 *     consumes them — the universal fallback.
 *
 * The choice is made **once**, at `open()` time, from three facts:
 *
 *   1. the decoder's `supports_zero_copy_decode()` — can it produce a
 *      surface at all?
 *   2. the sink's `supports_gpu_render()` — can it consume one?
 *   3. the sink's `supports_cpu_render()` — can it consume host pixels?
 *
 * The GPU path is taken only when **both** (1) and (2) hold.  Otherwise
 * the CPU path runs, provided (3) holds.  When neither path is possible
 * the combination is *incompatible* — and the engine reports that at
 * `open()` time rather than dropping every frame silently.
 *
 * ## Why this is a pure function
 *
 * The rule is the kind of thing that is easy to get subtly wrong and
 * hard to see: a wrong decision does not crash, it renders garbage or
 * renders nothing.  So it lives in a header-only, dependency-free
 * function that a unit test can drive through every combination
 * (2 × 2 × 2 = 8) without an engine, a transport, a clock or a plugin.
 *
 * Same shape as `bandwidth_allocation.hpp` (the per-stream share rule):
 * engine-private, header-only, unit-tested directly.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).  It includes only `nimrtc/plugins/`
 * seam headers, which the engine is allowed to use.
 *
 * @see docs/plan/remote-desktop-plan.md §Track E (E1 — renderer seam).
 */
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_sink.hpp>

namespace nimrtc::engine::detail {

/** Which route a decoded frame takes from the decoder to the sink. */
enum class VideoRenderPath : std::uint8_t {
    /** The GPU surface path: decoder → `GpuBuffer` → sink, no host copy. */
    kGpuSurface = 0,
    /** The CPU staging path: decoder → host pixels → sink. */
    kCpuStaging = 1,
    /** Neither path is possible — the codec/sink pair is incompatible. */
    kIncompatible = 2,
};

/** Human-readable name of a delivery path (for logs / diagnostics). */
inline constexpr std::string_view video_render_path_name(VideoRenderPath p) noexcept {
    switch (p) {
        case VideoRenderPath::kGpuSurface: return "gpu-surface";
        case VideoRenderPath::kCpuStaging: return "cpu-staging";
        case VideoRenderPath::kIncompatible: return "incompatible";
    }
    return "unknown";
}

/** The three facts the delivery-path rule is a function of. */
struct VideoRenderPathInputs {
    /** The decoder can produce a GPU surface
     *  (`IVideoCodec::supports_zero_copy_decode()`). */
    bool decoder_zero_copy = false;
    /** The sink can consume a GPU surface
     *  (`IVideoSink::supports_gpu_render()`). */
    bool sink_gpu_render = false;
    /** The sink can consume host-memory frames
     *  (`IVideoSink::supports_cpu_render()`). */
    bool sink_cpu_render = true;
};

/**
 * @brief Decide the delivery path for decoded frames.
 *
 * Pure: no engine state, no plugin calls, no allocation.  All eight
 * combinations of the three inputs are well-defined:
 *
 *   | decoder zero-copy | sink GPU | sink CPU | path        |
 *   |-------------------|----------|----------|-------------|
 *   | yes               | yes      | any      | kGpuSurface |
 *   | yes               | no       | yes      | kCpuStaging |
 *   | yes               | no       | no       | kIncompatible |
 *   | no                | any      | yes      | kCpuStaging |
 *   | no                | any      | no       | kIncompatible |
 *
 * The GPU path wins whenever it is available: RD-4 makes zero-copy the
 * default and CPU staging the fallback.
 */
[[nodiscard]] inline VideoRenderPath
decide_video_render_path(const VideoRenderPathInputs& in) noexcept {
    if (in.decoder_zero_copy && in.sink_gpu_render) {
        return VideoRenderPath::kGpuSurface;
    }
    if (in.sink_cpu_render) {
        return VideoRenderPath::kCpuStaging;
    }
    return VideoRenderPath::kIncompatible;
}

// ---------------------------------------------------------------------------
// Capture capability freeze
// ---------------------------------------------------------------------------

/** Why a requested capture configuration is not supported by a device. */
enum class CaptureViolation : std::uint8_t {
    /** The request is supported. */
    kNone = 0,
    /** The request asked for a zero-copy (GPU) capture the device cannot do. */
    kZeroCopyUnsupported = 1,
    /** The request asked for dirty-rect metadata the device cannot produce. */
    kDirtyRectsUnsupported = 2,
    /** The request asked for a sub-region the device cannot select. */
    kRegionSelectionUnsupported = 3,
    /** The request asked for more pixels than the device's maximum. */
    kMaxPixelsExceeded = 4,
    /** The request asked for a frame rate above the device's maximum. */
    kMaxFpsExceeded = 5,
    /** The request asked for a size the device's format list does not offer. */
    kFormatNotOffered = 6,
};

/** Human-readable name of a capture violation (for logs / diagnostics). */
inline constexpr std::string_view capture_violation_name(CaptureViolation v) noexcept {
    switch (v) {
        case CaptureViolation::kNone:                      return "none";
        case CaptureViolation::kZeroCopyUnsupported:        return "zero-copy unsupported";
        case CaptureViolation::kDirtyRectsUnsupported:      return "dirty-rects unsupported";
        case CaptureViolation::kRegionSelectionUnsupported: return "region selection unsupported";
        case CaptureViolation::kMaxPixelsExceeded:          return "max pixels exceeded";
        case CaptureViolation::kMaxFpsExceeded:             return "max fps exceeded";
        case CaptureViolation::kFormatNotOffered:           return "format not offered";
    }
    return "unknown";
}

/** The result of checking a capture request against a device's capabilities. */
struct CaptureCapabilityCheck {
    /** The violation, or `kNone` when the request is supported. */
    CaptureViolation violation = CaptureViolation::kNone;

    /** True iff the request is supported by the device. */
    [[nodiscard]] bool ok() const noexcept {
        return violation == CaptureViolation::kNone;
    }
};

/**
 * @brief Check a capture request against a device's capabilities.
 *
 * This is the **freeze** of the `CaptureCapabilities` / `CaptureFormat`
 * semantics (remote-desktop plan A1): the meaning of every flag is
 * fixed, and an unsupported *combination* is detected here — at
 * `open()` time — instead of failing on the first captured frame.
 *
 * The frozen semantics:
 *
 *   - `CaptureFormat::zero_copy == true` means "deliver this format
 *     without a CPU readback of the captured surface".  It therefore
 *     requires `CaptureCapabilities::zero_copy == true`.  The device
 *     statement is *device-wide* while the format statement is
 *     *per-format*, so the stricter of the two wins.
 *   - `CaptureFormat::has_dirty_rects == true` means "this format
 *     carries per-frame dirty-rectangle metadata".  It requires
 *     `CaptureCapabilities::dirty_rects == true`.
 *   - A non-zero `CaptureConfig::region` means "capture a sub-region",
 *     which requires `CaptureCapabilities::region_selection == true`.
 *   - `CaptureCapabilities::max_pixels` bounds the requested geometry:
 *     `width * height` must not exceed it (0 = unbounded).
 *   - `CaptureCapabilities::max_fps` bounds the requested frame rate
 *     (0 = unbounded).
 *
 * `CaptureConfig::want_cursor` is deliberately **not** a violation: the
 * cursor is a *preference*, honoured "only if the device can do it"
 * (see `CaptureConfig::want_cursor`).  A device that cannot composite
 * the pointer simply delivers frames without it.
 *
 * @param caps     The device's advertised capabilities.
 * @param cfg      The configuration the caller requested.  A zero
 *                 width/height means "the device's best", in which case
 *                 the size checks do not apply.
 * @param offered  The formats the device says it can deliver.  An empty
 *                 list means "the device does not publish a format
 *                 list" — the size check then rests on `max_pixels`
 *                 alone.
 */
[[nodiscard]] inline CaptureCapabilityCheck
check_capture_capabilities(const plugins::CaptureCapabilities& caps,
                            const plugins::CaptureConfig& cfg,
                            const std::vector<plugins::CaptureFormat>& offered) noexcept {
    CaptureCapabilityCheck out;

    // --- zero-copy -------------------------------------------------------
    // The *per-format* flag is the stricter of the two: a device that
    // advertises zero-copy in general but lists this format without it
    // cannot deliver this format without a readback.
    if (cfg.format.zero_copy && !caps.zero_copy) {
        out.violation = CaptureViolation::kZeroCopyUnsupported;
        return out;
    }
    if (cfg.format.zero_copy && !offered.empty()) {
        bool found = false;
        for (const auto& f : offered) {
            if (f.width == cfg.format.width &&
                f.height == cfg.format.height &&
                f.zero_copy) {
                found = true;
                break;
            }
        }
        if (!found) {
            out.violation = CaptureViolation::kZeroCopyUnsupported;
            return out;
        }
    }

    // --- dirty rectangles ------------------------------------------------
    if (cfg.format.has_dirty_rects && !caps.dirty_rects) {
        out.violation = CaptureViolation::kDirtyRectsUnsupported;
        return out;
    }

    // --- region selection ------------------------------------------------
    const bool wants_region =
        cfg.region.width != 0 && cfg.region.height != 0;
    if (wants_region && !caps.region_selection) {
        out.violation = CaptureViolation::kRegionSelectionUnsupported;
        return out;
    }

    // --- geometry --------------------------------------------------------
    // A zero width/height means "the device's best", so there is
    // nothing to check against the maximum.
    if (cfg.format.width != 0 && cfg.format.height != 0) {
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(cfg.format.width) *
            static_cast<std::uint64_t>(cfg.format.height);
        if (caps.max_pixels != 0 && pixels > caps.max_pixels) {
            out.violation = CaptureViolation::kMaxPixelsExceeded;
            return out;
        }
    }
    if (caps.max_fps != 0 && cfg.format.fps != 0 &&
        cfg.format.fps > caps.max_fps) {
        out.violation = CaptureViolation::kMaxFpsExceeded;
        return out;
    }

    // --- format list -----------------------------------------------------
    // The format list is the device's own statement of what it can
    // deliver.  When it publishes one, the requested geometry must be
    // in it (the `tag` is opaque to the engine, so it is not compared).
    if (!offered.empty() && cfg.format.width != 0 && cfg.format.height != 0) {
        bool found = false;
        for (const auto& f : offered) {
            if (f.width == cfg.format.width && f.height == cfg.format.height) {
                found = true;
                break;
            }
        }
        if (!found) {
            out.violation = CaptureViolation::kFormatNotOffered;
            return out;
        }
    }

    return out;
}

} // namespace nimrtc::engine::detail
