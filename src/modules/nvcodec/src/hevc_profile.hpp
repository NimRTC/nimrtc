/**
 * @file src/modules/nvcodec/src/hevc_profile.hpp
 * @brief The H.265 profile an NVENC session is initialised for
 *        (v1.3 plan item **Q1b**).
 *
 * ## Why the encoder needs a profile knob
 *
 * Q1b asserts that a **mid-stream H.265 parameter-set change** decodes
 * on the *same* NVDEC session, with the new picture's geometry, and
 * without a key-frame request round trip.  The H.265 module has no
 * software encoder (the stub produces no pixels), so the only producer
 * of a conforming H.265 bitstream in this tree is NVENC — and the
 * profile the session is initialised for decides the
 * `general_profile_idc` that the VPS and the SPS carry.
 *
 * Without the knob, every NVENC H.265 session produced profile MAIN,
 * and the only way to make the *parameter sets* differ between two
 * access units was a geometry change.  That covers half of Q1b.  The
 * other half — the parameter sets themselves changing while the
 * geometry stays the same — needs a second stream initialised for a
 * different profile, and this enum is how a test (or a caller) asks
 * for one.
 *
 * ## The default does not move
 *
 * `kMain` is what this backend has always produced; a caller that says
 * nothing gets exactly the bitstream it got before.  `kMain10` is
 * opt-in, and it is the only other profile this backend offers: the
 * 10-bit *input* path does not exist here (the staging path converts
 * I420 to 8-bit NV12), so a MAIN10 session is requested for its
 * *parameter sets*, not for ten-bit pixels.
 *
 * This header is private to the module (ADR-015): the enum is passed
 * between `nvenc_encoder.cpp` and the test that needs it through the
 * test-only accessor in `nv_backends.hpp`, never through the public
 * surface.
 */

#pragma once

#include <cstdint>

namespace nimrtc::nvcodec {

/** The H.265 profile an NVENC session asks for. */
enum class H265Profile : std::uint8_t {
    /** `general_profile_idc` 1 — 8-bit 4:2:0.  The backend's default. */
    kMain   = 1,
    /** `general_profile_idc` 2 — the driver decides what the 8-bit NV12
     *  input becomes; the VPS / SPS then say MAIN10.  Available only on
     *  silicon whose NVENC offers it — the session's own
     *  `nvEncInitializeEncoder` is what reports that, and a host without
     *  it fails `open()` rather than silently producing MAIN. */
    kMain10 = 2,
};

} // namespace nimrtc::nvcodec
