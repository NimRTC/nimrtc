/**
 * @file nimrtc/core/src/plugin_registry.cpp
 * @brief PluginRegistry out-of-line definitions (Windows DLL boundary fix).
 *
 * ## Why this file exists
 *
 * Prior to the Transport PAL Slice 8 / Slice 4 follow-up, `PluginRegistry`
 * was a header-only class declared in
 * `src/core/include/nimrtc/core/registry.hpp`. Its `instance()` method
 * was:
 *
 * ```cpp
 * static PluginRegistry& instance() {
 *     static PluginRegistry inst;
 *     return inst;
 * }
 * ```
 *
 * On GCC/Clang this works because the linker folds identical inline
 * function bodies across translation units — every consumer sees the same
 * `PluginRegistry inst`.
 *
 * On MSVC the same code DOES NOT work. Function-local statics in inline
 * functions are NOT subject to COMDAT folding; each translation unit
 * that includes `registry.hpp` emits its own `PluginRegistry inst`.  When
 * that TU is compiled into a static library and the .lib is consumed by
 * a second static library, the resulting `.obj` files end up duplicated
 * — each consuming static library has its own copy of `PluginRegistry`.
 *
 * Concrete failure observed in v0.10.x (CI Windows runner):
 *
 *   test_dtls_factory.exe       — `nimrtc_dtls_seam.lib` registers
 *                                 "wolfssl" → PluginRegistry instance A
 *   test_engine_plugin_loading.exe — reads from PluginRegistry instance B
 *                                     → size 0, lookup returns nullptr
 *
 * ## Fix
 *
 * Move the `instance()` body to this single .cpp file. Compile it into a
 * STATIC library (was OBJECT — see the `src/core/CMakeLists.txt` note for
 * why that change matters: an OBJECT library's `.obj` files get embedded
 * into every consuming static library, perpetuating the duplication).
 *
 * The `register_*` methods are also defined here. They were inlined in
 * the header to avoid the cost of a non-inline call for what looked like
 * trivial setters, but inlining them defeats the singleton: every TU that
 * included the header was emitting its own copy. Marking them with
 * `NIMRTC_API` (see `nimrtc/core/nimrtc_export.h`) and providing a
 * single definition here keeps the singleton unified AND keeps the API
 * surface DLL-friendly for a future Slice 7.5 SHARED-library conversion.
 *
 * The `get_*` and `list_*` methods stay inline in the header — they
 * delegate to the matching `TypedRegistry<T>::get/list_ids` template
 * methods, which are already defined exactly once in
 * `registry.hpp`.  Inlining them is safe and produces zero per-call
 * overhead.
 *
 * @note P1 — added as part of the Windows DLL boundary fix for the
 *       Transport PAL Slice 4 / Slice 8 registry singleton (v0.11.0).
 */

#include <nimrtc/core/registry.hpp>

namespace nimrtc::core {

// ---------------------------------------------------------------------------
// PluginRegistry::instance — the singleton accessor.
//
// The function-local static `PluginRegistry inst` lives here and ONLY
// here.  C++11 guarantees thread-safe first-time initialisation, and the
// destruction order is the reverse of construction at program exit.  See
// the file header for the pre-/post-fix behavioural contrast.
// ---------------------------------------------------------------------------
PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry inst;
    return inst;
}

// ---------------------------------------------------------------------------
// PluginRegistry::register_* — one definition per slot.
//
// These were inline in the header prior to this file landing.  Marking
// them `NIMRTC_API` and moving the body here is required for the Windows
// DLL boundary fix: see the file header for the rationale.
// ---------------------------------------------------------------------------

void PluginRegistry::register_transport(std::string_view id,
                                        const plugins::ITransportFactory* f) {
    transport_.register_one(id, f);
}

void PluginRegistry::register_ice_transport(std::string_view id,
                                            const plugins::IICETransportFactory* f) {
    ice_transport_.register_one(id, f);
}

void PluginRegistry::register_rtp(std::string_view id,
                                  const plugins::IRTPFactory* f) {
    rtp_.register_one(id, f);
}

void PluginRegistry::register_sdp(std::string_view id,
                                  const plugins::ISDPFactory* f) {
    sdp_.register_one(id, f);
}

void PluginRegistry::register_jb(std::string_view id,
                                 const plugins::IJBFactory* f) {
    jb_.register_one(id, f);
}

void PluginRegistry::register_audio3a(std::string_view id,
                                      const plugins::IAudio3AFactory* f) {
    audio3a_.register_one(id, f);
}

void PluginRegistry::register_codec(std::string_view id,
                                    const plugins::ICodecFactory* f) {
    codec_.register_one(id, f);
}

void PluginRegistry::register_video_codec(std::string_view id,
                                          const plugins::IVideoCodecFactory* f) {
    video_codec_.register_one(id, f);
}

void PluginRegistry::register_video_source(std::string_view id,
                                           const plugins::IVideoSourceFactory* f) {
    video_source_.register_one(id, f);
}

void PluginRegistry::register_video_sink(std::string_view id,
                                         const plugins::IVideoSinkFactory* f) {
    video_sink_.register_one(id, f);
}

void PluginRegistry::register_video_receiver(std::string_view id,
                                             const plugins::IVideoReceiverFactory* f) {
    video_receiver_.register_one(id, f);
}

void PluginRegistry::register_video_sender(std::string_view id,
                                           const plugins::IVideoSenderFactory* f) {
    video_sender_.register_one(id, f);
}

void PluginRegistry::register_bwe(std::string_view id,
                                  const plugins::IBweFactory* f) {
    bwe_.register_one(id, f);
}

void PluginRegistry::register_scheduler(std::string_view id,
                                        const plugins::ISchedulerFactory* f) {
    scheduler_.register_one(id, f);
}

void PluginRegistry::register_datachannel(std::string_view id,
                                          const plugins::IDataChannelFactory* f) {
    datachannel_.register_one(id, f);
}

void PluginRegistry::register_dtls_session(std::string_view id,
                                           const dtls::IDtlsSessionFactory* f) {
    dtls_session_.register_one(id, f);
}

void PluginRegistry::register_sctp_socket(std::string_view id,
                                          const sctp::ISctpSocketFactory* f) {
    sctp_socket_.register_one(id, f);
}

void PluginRegistry::register_raw_udp_datagram(std::string_view id,
                                               const raw_udp::IRawUdpFactory* f) {
    raw_udp_.register_one(id, f);
}

void PluginRegistry::register_transport_stack(std::string_view id,
                                              const ITransportStackFactory* f) {
    transport_stack_.register_one(id, f);
}

} // namespace nimrtc::core
