/**
 * @file nimrtc/core/src/pal_default_registrars.cpp
 * @brief PAL Slice 2: explicit self-registration table (PAL architecture §4 Slice 2).
 *
 * ## Purpose
 *
 * `kDefaultRegistrars[]` is the single authoritative list of every built-in
 * plugin's `register_default_plugins()` entry point.  It replaces the inline
 * statement-list body of `core::register_all_default_plugins()` with an
 * iterable array, enabling:
 *
 *   (a) compile-time completeness audits (a test can iterate the array and
 *       assert no two entries have the same factory id),
 *   (b) mechanical grep discoverability: `git grep 'kDefaultRegistrars'`
 *       surfaces the table in one hit,
 *   (c) future extension without editing `registry.hpp` — adding a new module
 *       requires only one new `&module::register_default_plugins` entry here.
 *
 * ## Layout constraint
 *
 * `src/core/` is normally an INTERFACE (header-only) library per Layout
 * Invariant 1 of `docs/architecture.md`.  This source file is the sole
 * exception: `pal_default_registrars.cpp` holds a static-initialised array
 * whose construction must run at program startup, which requires a compiled
 * translation unit.  `src/core/CMakeLists.txt` adds this file to a
 * non-INTERFACE target so it participates in the build.
 *
 * ## Adding a new plugin
 *
 * When a new built-in module `foo` is added:
 *
 *   1. Ensure `foo` declares `namespace nimrtc::foo { void
 *      register_default_plugins() noexcept; }` in its public header.
 *   2. Add `&nimrtc::foo::register_default_plugins` to `kDefaultRegistrars[]`
 *      below (keep the list alphabetical by namespace).
 *   3. If the module is conditional (behind a CMake `option`), guard the
 *      array entry with the same `#ifdef` that guards the existing call
 *      in `registry.hpp`.
 *
 * ## What is NOT registered here
 *
 * None of the transport-layer seams (DTLS / SCTP / raw_udp /
 * transport-stack) were registered in v0.10.x pre-Slice-8 because
 * the typed `core::PluginRegistry` slots did not exist. Slice 8
 * (v0.10.2) added the four slots and the matching entries in this
 * table — DTLS / SCTP / raw_udp / transport are now registered
 * alongside the existing audio / video modules, completing the
 * transport-layer PAL surface per `docs/plan/transport-selection.md`
 * §6.5.
 */

#include <nimrtc/core/registry.hpp>

namespace nimrtc::core::detail {

// ---------------------------------------------------------------------------
// Type alias — all registration entry points share this signature.
// ---------------------------------------------------------------------------
using RegistrarFn = void(*)() noexcept;

// ---------------------------------------------------------------------------
// kDefaultRegistrars — iterable table of all built-in plugin registrars.
// ---------------------------------------------------------------------------
//
// Entries are listed in the same order as they appeared in the original
// inline body of `register_all_default_plugins()` in `registry.hpp`.
// Guarded entries mirror the `#ifdef` guards that protected the original calls.
//
// Alphabetical order by namespace is preferred for new additions.
constexpr RegistrarFn kDefaultRegistrars[] = {
    &nimrtc::audio3a::register_default_plugins,
    // v1.2.0 (audio pipeline, WP-7): the audio receive-pipeline module.
    // Its entry point populates the registry's audio-receiver slot
    // (register_audio_receiver) under the id "nimrtc_audio_rx", so the
    // engine can resolve the pipeline through the seam without naming
    // a concrete module (Layout Invariant 4 / spec B-15).
    //
    // The declaration lives in <nimrtc/core/registry.hpp> (namespace
    // nimrtc::audio_rx) so this table can take its address without
    // including the module header — the same pattern the Slice 8
    // transport entries below use.
#ifdef NIMRTC_HAS_AUDIO_RX
    &nimrtc::audio_rx::register_default_plugins,
#endif
    &nimrtc::bwe::register_default_plugins,
    &nimrtc::datachannel::register_default_plugins,
#ifdef NIMRTC_HAS_H264
    &nimrtc::h264::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_H265
    // v1.3 (video plan C4 / C8): the H.265 codec module.  Its registrar
    // entry materialises only when the module is built; the engine then
    // resolves it through the registry's video-codec slot.
    &nimrtc::h265::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_AV1
    // v1.3 (video plan C6 / C8): the AV1 codec module.  Same shape as
    // the H.265 entry above.
    &nimrtc::av1::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_VP9
    // Remote-desktop plan B11: the VP9 codec module.  It registers the
    // "vp9" codec factory and populates the VP9 backend table (the
    // optional vendored libvpx row plus the stub).
    &nimrtc::vp9::register_default_plugins,
#endif
    &nimrtc::ice::register_default_plugins,
    &nimrtc::jb::register_default_plugins,
#ifdef NIMRTC_HAS_NET_FEEDBACK
    // v1.2.0 M1 (F2): the receiver-side network-feedback collector.  Its
    // entry point populates the registry's net-feedback slot under the id
    // "nimrtc_net_feedback", so the engine can resolve the collector
    // through the seam without naming a concrete module.
    &nimrtc::net_feedback::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_OPUS
    &nimrtc::opus::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_SESSION
    // v1.2.0 (session capability plan, N1 / T1): the session model.
    // Its entry point populates the registry's session slot under the
    // id "session", so the engine can resolve the session model
    // through the seam without naming a concrete module.
    &nimrtc::session::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_DTMF
    // v1.2.0 (session capability plan, D1 / D2): the RFC 4733
    // telephone-event codec.  Its entry point populates the registry's
    // DTMF slot under the id "dtmf", so the engine can resolve the
    // codec through the seam without naming a concrete module.
    &nimrtc::dtmf::register_default_plugins,
#endif
    &nimrtc::rtp::register_default_plugins,
    &nimrtc::sched::register_default_plugins,
    &nimrtc::sdp::register_default_plugins,
#ifdef NIMRTC_HAS_SRTP
    // v1.2.0 (remediation plan B7 / Q3): the SRTP crypto backend.
    // Its entry point populates the registry's SRTP-backend slot
    // (register_srtp_backend) under the id "libsrtp", so the engine can
    // resolve the crypto backend through the seam without naming a
    // concrete module (Layout Invariant 4).
    &nimrtc::srtp::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_VIDEO_PIPELINE
    &nimrtc::video_pipeline::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_RD_INPUT
    // Remote-desktop plan Track G (G1 / G2): the remote-desktop input
    // module.  Its entry point populates the registry's control-channel
    // slot (register_control_channel) under the id "control-channel", so
    // the engine can resolve the control channel through the seam
    // without naming a concrete module (Layout Invariant 4).
    &nimrtc::rd_input::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_RD_QOE
    // Remote-desktop plan Track J (J3 / J6): the QoE-metrics module.  Its
    // entry point populates the registry's qoe-collector slot
    // (register_qoe_collector) under the id "rd-qoe", so the engine can
    // feed the collector through the seam without naming a concrete
    // module (Layout Invariant 4).
    &nimrtc::rd_qoe::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_VIDEO_SINK
    &nimrtc::video_sink::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_VIDEO_SOURCE
    &nimrtc::video_source::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_VIDEO_CAPTURE
    // Remote-desktop plan A1: the capture-device module.  Its entry
    // populates the registry's video-capture slot (register_video_capture)
    // under the id "synthetic", so the engine can resolve a capture
    // backend through the seam without naming a concrete module
    // (Layout Invariant 4).
    &nimrtc::video_capture::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_VIDEO_BITRATE_CONTROL
    // Remote-desktop plan D1: the video rate controller.  Its entry
    // populates the registry's video-bitrate-controller slot
    // (register_video_bitrate_controller) under the id
    // "video-bitrate-control", so the engine can resolve the ladder
    // through the seam without naming a concrete module
    // (Layout Invariant 4).
    &nimrtc::video_bitrate_control::register_default_plugins,
#endif
    // Transport PAL Slice 8 (v0.10.2): DTLS / SCTP / raw_udp /
    // transport-stack default-plugins entries. These mirror the
    // unconditional entries above — there are no `NIMRTC_HAS_*` guards
    // because every transport-layer backend is now a real Seam:
    //   - DTLS:  WolfsslDtlsFactory    (id="wolfssl")
    //   - SCTP:  SctpStubFactory       (id="stub"; v0.11.0 adds
    //                                    "usrsctp" alongside)
    //   - raw_udp: ArqRawUdpFactory    (id="arq")
    //   - transport: CapabilitySelectorStackFactory (id="default")
    // Adding a new transport-layer backend means adding a new entry
    // here AND a new typed factory registration in the matching
    // module's `register_default_plugins()` body. The seam-level
    // pattern is identical across DTLS / SCTP / raw_udp / transport
    // (see the Slice 8 commits in the relevant .cpp files).
    &nimrtc::dtls::register_default_plugins,
    &nimrtc::sctp::register_default_plugins,
    &nimrtc::raw_udp::register_default_plugins,
    &nimrtc::transport::register_default_plugins,
    // v0.14.0-beta.1 (ADR-014): WebTransport / QUIC seam factory.
    // Produces IWebTransportSession instances backed by msquic (MIT-licensed,
    // Windows-first, cross-platform). Registered alongside the transport-layer
    // seams above so `register_all_default_plugins()` covers the full
    // transport stack. The web_transport module links nimrtc::vendor::msquic
    // so callers that don't need WebTransport don't need to link it.
    &nimrtc::web_transport::register_default_plugins,
};

// ---------------------------------------------------------------------------
// Iteration helpers — used by tests and by the rewritten
// `register_all_default_plugins()` in `registry.hpp`.
// ---------------------------------------------------------------------------

/** Number of entries in the default-registrar table. */
constexpr std::size_t kDefaultRegistrarCount =
    sizeof(kDefaultRegistrars) / sizeof(kDefaultRegistrars[0]);

} // namespace nimrtc::core::detail
