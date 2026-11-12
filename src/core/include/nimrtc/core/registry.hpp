/**
 * @file nimrtc/core/registry.hpp
 * @brief PluginRegistry ??global factory registry for NimRTC plugins.
 *
 * Per ARCHITECTURE.md Layout Invariant 6:
 *   "Singletons (Logger, config registries) live under nimrtc::core:: only."
 *
 * Registry lives in nimrtc::core:: for this reason.
 *
 * Convenience aliases live in nimrtc::plugins:: (via using declarations) so
 * existing code that includes <nimrtc/plugins/registry.hpp> continues to work.
 *
 * All built-in plugins are auto-registered at static-init time.
 * User plugins register themselves before calling NimRTCEngine::open().
 *
 * ## Registering a custom plugin
 *
 * @code
 * // my_transport.cpp
 * #include "nimrtc/core/registry.hpp"
 *
 * class MyTransport : public nimrtc::plugins::ITransport { ... };
 *
 * static const nimrtc::plugins::SimpleTransportFactory<MyTransport>
 *     factory{"my_udp", "My UDP transport"};
 *
 * NIMRTC_REGISTER_TRANSPORT(my_udp, &factory);
 * @endcode
 *
 * Then in main():
 * @code
 * NimRTCEngine::Config cfg;
 * cfg.transport_name = "my_udp";  // look up by ID
 * cfg.jb_name       = "adaptive";
 * NimRTCEngine engine(cfg);
 * engine.open();
 * @endcode
 *
 * @note The RTP and SDP factories are **not** selectable through
 * `EngineConfig` at this cut: `rtp_name` / `sdp_name` exist for
 * forward compatibility but the engine always uses the built-in RTP and
 * SDP implementations.  Do not promise a custom RTP/SDP backend in
 * documentation or marketing until the engine actually resolves them
 * through the registry.
 *
 * @note `cfg.transport_name` selects the **ICE transport** plugin
 * (it is forwarded to `StackConfig::ice_id`); it does not select the
 * generic `ITransport` slot.
 *
 * @note Thread-safe for get_*() calls. register_*() is not thread-safe
 * and must be called before any engine is created.
 *
 * @note P0 scaffold ??registry is in-memory only; binary-safe plugin loading
 * (dlopen / LoadLibrary) is TBD P4.
 */

#ifndef NIMRTC_CORE_REGISTRY_HPP
#define NIMRTC_CORE_REGISTRY_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <string_view>
#include <optional>
#include <vector>
#include <functional>
#include <mutex>
#include <cassert>

#include <nimrtc/core/nimrtc_export.h>   // NIMRTC_API ??see header for
                                          // Windows DLL boundary rationale.
#include <nimrtc/core/api_version.hpp>    // NIMRTC_API_STABLE

// Forward declarations of plugin interfaces ??no full definitions needed here,
// just the factory type names. Full definitions are in the respective headers.
namespace nimrtc::plugins {
class NIMRTC_API_STABLE ITransportFactory;
class NIMRTC_API_STABLE IICETransportFactory;
class NIMRTC_API_STABLE IRTPFactory;
class NIMRTC_API_STABLE ISDPFactory;
class NIMRTC_API_STABLE IJBFactory;
class NIMRTC_API_STABLE IAudio3AFactory;
class NIMRTC_API_STABLE ICodecFactory;
class NIMRTC_API_STABLE IVideoCodecFactory;
class NIMRTC_API_STABLE IVideoSourceFactory;
class NIMRTC_API_STABLE IVideoCaptureFactory;
class NIMRTC_API_STABLE IVideoSinkFactory;
class NIMRTC_API_STABLE IVideoReceiverFactory;
class NIMRTC_API_STABLE IVideoSenderFactory;
class NIMRTC_API_STABLE IBweFactory;
class NIMRTC_API_STABLE ISchedulerFactory;
class NIMRTC_API_STABLE IDataChannelFactory;
// v1.2.0 M1 (F2): the receiver-side network-feedback collector seam.
// Forward-declared like the other factory types so the registry's
// TypedRegistry<> field can reference it without pulling the seam header
// into core (Layout Invariant 1).
class NIMRTC_API_STABLE INetFeedbackFactory;
// v1.2.0 (audio pipeline): the two new audio seams -- the codec-side
// decoder and the pipeline-side receiver. Forward-declared like the
// other factory types so the registry's TypedRegistry<> fields can
// reference them without pulling the seam headers into core
// (Layout Invariant 1).
class NIMRTC_API_STABLE IAudioDecoderFactory;
class NIMRTC_API_STABLE IAudioReceiverFactory;
// v1.2.0 (session capability plan, N1 / T1): the session-model seam.
// The engine drives renegotiation through this interface and never
// includes `src/modules/session/`'s headers (Layout Invariant 4).
class NIMRTC_API_STABLE ISessionFactory;
// v1.2.0 (session capability plan, D1 / D2): the RFC 4733
// (telephone-event) seam.  A telephone-event payload is *not* audio, so
// it does not belong to the audio-decoder category -- it has its own
// slot and its own exit next to the audio routing table.
class NIMRTC_API_STABLE IDtmfCodecFactory;
// v1.2.0 (session capability plan, X1): the RTCP Extended Report
// consumer seam.  The engine routes inbound XR sub-packets (payload
// type 207) into it; it decodes the RFC 3611 report blocks for the
// host.  Forward-declared like the other factory types so the
// registry's TypedRegistry<> field can reference it without pulling the
// seam header into core (Layout Invariant 1).
class NIMRTC_API_STABLE IXrReportFactory;
} // namespace plugins

// Transport PAL Slice 8 (v0.10.2): typed factory slots for DTLS / SCTP /
// raw_udp / transport-stack. Each module's seam factory inherits its
// respective interface (IDtlsSessionFactory / ISctpSocketFactory /
// IRawUdpFactory / ITransportStackFactory) and is published via the
// matching `register_*` hook on the registry below.
//
// Forward-declared here so the registry's TypedRegistry<...> fields and
// public method signatures can reference them without pulling the seam
// headers into core (preserves Layout Invariant 1 ??core stays header-
// only + the single .cpp exception in `pal_default_registrars.cpp`).
namespace nimrtc::dtls    { class NIMRTC_API_STABLE IDtlsSessionFactory; }
namespace nimrtc::sctp     { class NIMRTC_API_STABLE ISctpSocketFactory; }
namespace nimrtc::raw_udp  { class NIMRTC_API_STABLE IRawUdpFactory; }
namespace nimrtc           { class NIMRTC_API_STABLE ITransportStackFactory; }

// v0.14.0 alpha.2 (ADR-014): typed factory slot for the WebTransport /
// QUIC seam. The seam itself (`IWebTransportSession` + companion
// headers) lands in alpha.2; the msquic-backed factory lands in
// beta.1.  Forward-declared here so the registry's TypedRegistry<...>
// field and public method signatures can reference the factory type
// without pulling the seam headers into core (same Layout Invariant 1
// rationale as the DTLS / SCTP / raw_udp / transport-stack slots
// above).
namespace nimrtc::web_transport { class NIMRTC_API_STABLE IWebTransportSessionFactory; }

// ---------------------------------------------------------------------------
// Forward declarations for unified registration entry point.
//
// Each concrete module exposes `register_default_plugins()` (defined in its
// plugin adapter .cpp). The unified `core::register_all_default_plugins()`
// below calls each enabled module. Consumers linking the unified entry point
// MUST also link the corresponding module libraries ??the forward declarations
// below intentionally avoid pulling any module header into core (Layout Invariant 1).
// ---------------------------------------------------------------------------

namespace nimrtc {
NIMRTC_API_STABLE namespace ice     { void register_default_plugins() noexcept; }
namespace rtp     { void register_default_plugins() noexcept; }
namespace sdp     { void register_default_plugins() noexcept; }
namespace jb      { void register_default_plugins() noexcept; }
namespace audio3a { void register_default_plugins() noexcept; }
#ifdef NIMRTC_HAS_OPUS
namespace opus    { void register_default_plugins() noexcept; }
#endif
#ifdef NIMRTC_HAS_H264
namespace h264    { void register_default_plugins() noexcept; }
#endif
#ifdef NIMRTC_HAS_H265
namespace h265    { void register_default_plugins() noexcept; }
#endif
#ifdef NIMRTC_HAS_AV1
namespace av1     { void register_default_plugins() noexcept; }
#endif
#ifdef NIMRTC_HAS_VIDEO_SOURCE
namespace video_source    { void register_default_plugins() noexcept; }
#endif
// Remote-desktop plan A1: the capture-device module.
#ifdef NIMRTC_HAS_VIDEO_CAPTURE
namespace video_capture   { void register_default_plugins() noexcept; }
#endif
#ifdef NIMRTC_HAS_VIDEO_PIPELINE
namespace video_pipeline  { void register_default_plugins() noexcept; }
#endif
#ifdef NIMRTC_HAS_VIDEO_SINK
namespace video_sink      { void register_default_plugins() noexcept; }
#endif
namespace bwe             { void register_default_plugins() noexcept; }
namespace sched           { void register_default_plugins() noexcept; }
// v1.2.0 M1 (F2): the receiver-side network-feedback collector module.
// Its entry point populates the registry's net-feedback slot
// (register_net_feedback) so `core::register_all_default_plugins()` can
// wire the built-in collector through the unified table.
namespace net_feedback   { void register_default_plugins() noexcept; }
namespace datachannel      { void register_default_plugins() noexcept; }
// v1.2.0 (audio pipeline): the audio receive-pipeline module. Its entry
// point populates the audio-receiver registry slot
// (register_audio_receiver) so `core::register_all_default_plugins()`
// can wire the built-in receiver through the unified table.
//
// The declaration is unconditional -- like the ice / rtp / sdp / jb /
// datachannel entries above -- so every consumer of
// <nimrtc/core/registry.hpp> sees the same set of module entry points.
// The *definition* lands with the module (WP-3), and the matching
// kDefaultRegistrars[] entry lands with WP-7 -- which is also where the
// engine starts linking nimrtc::audio_rx.  Until then nobody takes the
// address of this symbol, so the declaration alone is harmless.
namespace audio_rx { void register_default_plugins() noexcept; }
// v1.2.0 (session capability plan, N1 / T1): the session-model module.
// Its entry point populates the session registry slot
// (register_session) so `core::register_all_default_plugins()` can
// wire the built-in session model through the unified table.  The
// engine then resolves it through
// `core::PluginRegistry::get_session(EngineConfig::session_name)`.
namespace session    { void register_default_plugins() noexcept; }
// v1.2.0 (session capability plan, D1 / D2): the RFC 4733
// (telephone-event) codec module.  Its entry point populates the DTMF
// registry slot (register_dtmf) so
// `core::register_all_default_plugins()` can wire the built-in codec
// through the unified table.  The engine then resolves it through
// `core::PluginRegistry::get_dtmf(EngineConfig::dtmf_codec_name)`.
namespace dtmf       { void register_default_plugins() noexcept; }
// Transport PAL Slice 8 (v0.10.2): module-level entry points for the
// DTLS / SCTP / raw_udp / transport-stack seams. Each one populates
// its matching typed registry slot (register_dtls_session /
// register_sctp_socket / register_raw_udp_datagram /
// register_transport_stack) so `core::register_all_default_plugins()`
// can wire every built-in factory through the unified table.
namespace dtls     { void register_default_plugins() noexcept; }
namespace sctp     { void register_default_plugins() noexcept; }
namespace raw_udp  { void register_default_plugins() noexcept; }
namespace transport { void register_default_plugins() noexcept; }
// v0.14.0 alpha.2 (ADR-014): WebTransport / QUIC seam module entry
// point.  The forward declaration is here so the inline
// `register_all_default_plugins()` body in `nimrtc::core` can
// reference the symbol when it lands (beta.1) without forcing
// every TU to include the web_transport module header (Layout
// Invariant 1).  At the alpha.2 cut the symbol is not yet
// defined; the matching kDefaultRegistrars[] entry is added in
// beta.1 alongside the factory implementation.
namespace web_transport { void register_default_plugins() noexcept; }
} // namespace nimrtc

namespace nimrtc::core {

// ---------------------------------------------------------------------------
// Detail
// ---------------------------------------------------------------------------

template<class T>
class NIMRTC_API_STABLE TypedRegistry {
    // Keys are stored by VALUE, not by view.
    //
    // A third-party plugin author may reasonably write
    //     reg.register_video_codec(std::string("myid"), &factory);
    // With a `std::string_view` key that call would store a view onto a
    // temporary that dies at the end of the full expression, leaving a
    // dangling key in the registry: the entry becomes unreachable
    // (lookup by "myid" never matches) and any subsequent read of the
    // key is undefined behaviour.  Owning the key removes the whole
    // class of bug at the cost of one small allocation per registration
    // (registrations happen at static-init time, so the cost is
    // irrelevant at run time).
    std::vector<std::pair<std::string, const T*>> entries_;
    mutable std::mutex mutex_;

public:
    void register_one(std::string_view id, const T* factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [k, v] : entries_) {
            if (k == id) {
                // Overwrite allowed (allows late binding).
                v = factory;
                return;
            }
        }
        entries_.emplace_back(std::string(id), factory);
    }

    [[nodiscard]] const T* get(std::string_view id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [k, v] : entries_) {
            if (k == id) return v;
        }
        return nullptr;
    }

    /// Remove a single entry by id.  Returns true if a matching
    /// entry was removed.  Used by tests to clean up after
    /// themselves (the registry is a process-wide singleton, so a
    /// test that calls `register_one("foo", ...)` would otherwise
    /// leak "foo" into every later test in the same process).
    bool unregister_one(std::string_view id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->first == id) {
                entries_.erase(it);
                return true;
            }
        }
        return false;
    }

    /// List every registered id.
    ///
    /// Returns owned strings, not views: the views would dangle as
    /// soon as a later registration mutates `entries_` (reallocation
    /// invalidates the moved-from `std::string`s).  Callers that only
    /// compare against literals are unaffected; callers that need a
    /// stable view can copy the string themselves.
    [[nodiscard]] std::vector<std::string> list_ids() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.reserve(entries_.size());
        for (auto& [k, _] : entries_) out.push_back(k);
        return out;
    }

    template<class Pred>
    [[nodiscard]] const T* find_if(Pred p) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [k, v] : entries_) {
            if (p(k, v)) return v;
        }
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// PluginRegistry ??lives in nimrtc::core:: per Layout Invariant 6.
//
// `NIMRTC_API` is applied at the class level so the singleton's
// vtable (and, more importantly, the function-local static inside
// `instance()`) is unified across the static-library boundaries that
// `nimrtc_dtls_seam.lib` / `test_engine_plugin_loading.exe` introduce
// on Windows.  See `nimrtc/core/nimrtc_export.h` for the Windows DLL
// rationale; on GCC / Clang the macro is an empty attribute.
//
// IMPORTANT: MSVC rejects `__declspec(dllimport)` / `__declspec(dllexport)`
// on individual members when the enclosing class already has the attribute
// (C2487 ??"dll interface class member cannot be declared with dll
// interface").  All public methods therefore inherit the DLL attribute
// from the class declaration; do NOT re-add `NIMRTC_API` on the
// individual `register_*` declarations below.
//
// `instance()` is intentionally NOT inline ??its body lives in
// `src/core/src/plugin_registry.cpp` so there is exactly one definition
// of the function-local static `PluginRegistry inst` across the whole
// program.  Without that, MSVC would emit a fresh copy in every TU
// that includes this header, and each static library would end up with
// its own `PluginRegistry` instance.  See the file header for the
// pre-/post-fix behavioural contrast.
// ---------------------------------------------------------------------------

class NIMRTC_API_STABLE PluginRegistry {
    PluginRegistry() = default;
    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

public:
    // Out-of-line definition in `src/core/src/plugin_registry.cpp`.
    // C++11 guarantees thread-safe first-time initialisation of the
    // function-local static; destruction is at program exit.
    static PluginRegistry& instance();

    // -- Transport -----------------------------------------------------------
    void register_transport(std::string_view id,
                            const plugins::ITransportFactory* f);
    [[nodiscard]] const plugins::ITransportFactory*
    get_transport(std::string_view id) const {
        return transport_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_transports() const {
        return transport_.list_ids();
    }

    // -- ICE Transport (ICE-specific factory, see plugins/ice_transport.hpp) -
    //
    // Mirrors the transport registry but is typed as
    // `IICETransportFactory*` so callers can resolve the ICE-aware surface
    // (state, credentials, gathering, remote SDP) without a
    // dynamic_cast to the concrete `ice::IceTransport`. The factory itself
    // inherits from `ITransportFactory` and `IICETransportFactory::create()`
    // forwards to `create_ice()`, so registering via `register_ice_transport`
    // also satisfies `get_transport` lookups for the same id.
    void register_ice_transport(std::string_view id,
                                const plugins::IICETransportFactory* f);
    [[nodiscard]] const plugins::IICETransportFactory*
    get_ice_transport(std::string_view id) const {
        return ice_transport_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_ice_transports() const {
        return ice_transport_.list_ids();
    }

    // -- RTP ----------------------------------------------------------------
    void register_rtp(std::string_view id, const plugins::IRTPFactory* f);
    [[nodiscard]] const plugins::IRTPFactory* get_rtp(std::string_view id) const {
        return rtp_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_rtp() const {
        return rtp_.list_ids();
    }

    // -- SDP ----------------------------------------------------------------
    void register_sdp(std::string_view id, const plugins::ISDPFactory* f);
    [[nodiscard]] const plugins::ISDPFactory* get_sdp(std::string_view id) const {
        return sdp_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_sdp() const {
        return sdp_.list_ids();
    }

    // -- JB -----------------------------------------------------------------
    void register_jb(std::string_view id, const plugins::IJBFactory* f);
    [[nodiscard]] const plugins::IJBFactory* get_jb(std::string_view id) const {
        return jb_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_jb() const {
        return jb_.list_ids();
    }

    // -- Audio 3A -----------------------------------------------------------
    void register_audio3a(std::string_view id, const plugins::IAudio3AFactory* f);
    [[nodiscard]] const plugins::IAudio3AFactory*
    get_audio3a(std::string_view id) const {
        return audio3a_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_audio3a() const {
        return audio3a_.list_ids();
    }

    // -- Codec --------------------------------------------------------------
    void register_codec(std::string_view id,
                        const plugins::ICodecFactory* f);
    [[nodiscard]] const plugins::ICodecFactory*
    get_codec(std::string_view id) const {
        return codec_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_codecs() const {
        return codec_.list_ids();
    }

    // -- Audio Decoder (v1.2.0, audio pipeline) ----------------------------
    // The codec-side seam of the audio pipeline. Ids are per-category,
    // so the Opus decoder registers under "opus" in this category even
    // though the Opus *codec* plugin already uses that id in the codec
    // category.
    void register_audio_decoder(std::string_view id,
                                const plugins::IAudioDecoderFactory* f);
    [[nodiscard]] const plugins::IAudioDecoderFactory*
    get_audio_decoder(std::string_view id) const {
        return audio_decoder_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_audio_decoders() const {
        return audio_decoder_.list_ids();
    }

    // -- Audio Receiver (v1.2.0, audio pipeline) ---------------------------
    // The pipeline-side seam of the audio pipeline.
    void register_audio_receiver(std::string_view id,
                                 const plugins::IAudioReceiverFactory* f);
    [[nodiscard]] const plugins::IAudioReceiverFactory*
    get_audio_receiver(std::string_view id) const {
        return audio_receiver_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_audio_receivers() const {
        return audio_receiver_.list_ids();
    }

    // -- Session (v1.2.0, session capability plan N1 / T1) ---------------
    //
    // Typed slot for `plugins::ISessionFactory*` — the session model
    // the engine drives to build the SDP media list.  The built-in
    // implementation lives in `src/modules/session/` and registers
    // under the id "session"; a host can register its own to change
    // the mid-allocation or BUNDLE policy without touching the engine.
    //
    // The engine resolves this through `EngineConfig::session_name`.
    void register_session(std::string_view id,
                          const plugins::ISessionFactory* f);
    [[nodiscard]] const plugins::ISessionFactory*
    get_session(std::string_view id) const {
        return session_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_sessions() const {
        return session_.list_ids();
    }

    // -- DTMF (v1.2.0, session capability plan D1 / D2) ------------------
    //
    // Typed slot for `plugins::IDtmfCodecFactory*` — the RFC 4733
    // (telephone-event) codec.  It is a slot of its own because a
    // telephone-event payload is not audio: handing it to the audio
    // decoder would push a four-byte event header into an Opus decoder.
    //
    // The built-in implementation lives in `src/modules/dtmf/` and
    // registers under the id "dtmf"; the engine resolves it through
    // `EngineConfig::dtmf_codec_name` and drives it through the
    // interface declared in `nimrtc/plugins/dtmf.hpp`.
    void register_dtmf(std::string_view id,
                       const plugins::IDtmfCodecFactory* f);
    [[nodiscard]] const plugins::IDtmfCodecFactory*
    get_dtmf(std::string_view id) const {
        return dtmf_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_dtmfs() const {
        return dtmf_.list_ids();
    }

    // -- Video Codec --------------------------------------------------------
    void register_video_codec(std::string_view id,
                              const plugins::IVideoCodecFactory* f);
    [[nodiscard]] const plugins::IVideoCodecFactory*
    get_video_codec(std::string_view id) const {
        return video_codec_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_video_codecs() const {
        return video_codec_.list_ids();
    }

    // -- Video Source -------------------------------------------------------
    void register_video_source(std::string_view id,
                               const plugins::IVideoSourceFactory* f);
    [[nodiscard]] const plugins::IVideoSourceFactory*
    get_video_source(std::string_view id) const {
        return video_source_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_video_sources() const {
        return video_source_.list_ids();
    }

    // -- Video Capture (remote-desktop plan A1) ----------------------------
    void register_video_capture(std::string_view id,
                                const plugins::IVideoCaptureFactory* f);
    [[nodiscard]] const plugins::IVideoCaptureFactory*
    get_video_capture(std::string_view id) const {
        return video_capture_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_video_captures() const {
        return video_capture_.list_ids();
    }

    // -- Video Sink ---------------------------------------------------------
    void register_video_sink(std::string_view id,
                             const plugins::IVideoSinkFactory* f);
    [[nodiscard]] const plugins::IVideoSinkFactory*
    get_video_sink(std::string_view id) const {
        return video_sink_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_video_sinks() const {
        return video_sink_.list_ids();
    }

    // -- Video Receiver -----------------------------------------------------
    void register_video_receiver(std::string_view id,
                                 const plugins::IVideoReceiverFactory* f);
    [[nodiscard]] const plugins::IVideoReceiverFactory*
    get_video_receiver(std::string_view id) const {
        return video_receiver_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_video_receivers() const {
        return video_receiver_.list_ids();
    }

    // -- Video Sender -------------------------------------------------------
    void register_video_sender(std::string_view id,
                               const plugins::IVideoSenderFactory* f);
    [[nodiscard]] const plugins::IVideoSenderFactory*
    get_video_sender(std::string_view id) const {
        return video_sender_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_video_senders() const {
        return video_sender_.list_ids();
    }

    // -- BWE (bandwidth estimator) -----------------------------------------
    void register_bwe(std::string_view id,
                      const plugins::IBweFactory* f);
    [[nodiscard]] const plugins::IBweFactory*
    get_bwe(std::string_view id) const {
        return bwe_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_bwes() const {
        return bwe_.list_ids();
    }

    // -- Scheduler (unified sending) ---------------------------------------
    void register_scheduler(std::string_view id,
                            const plugins::ISchedulerFactory* f);
    [[nodiscard]] const plugins::ISchedulerFactory*
    get_scheduler(std::string_view id) const {
        return scheduler_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_schedulers() const {
        return scheduler_.list_ids();
    }

    // -- Net feedback (v1.2.0 M1, F2) -------------------------------------
    //
    // Typed slot for the receiver-side network-feedback collector seam
    // (`plugins::INetFeedback`).  The collector sits between the wire and
    // the bandwidth estimator: the engine routes inbound RTCP to it, it
    // aggregates the signals, and the engine hands the result to
    // `IBwe::on_feedback()`.
    void register_net_feedback(std::string_view id,
                               const plugins::INetFeedbackFactory* f);
    [[nodiscard]] const plugins::INetFeedbackFactory*
    get_net_feedback(std::string_view id) const {
        return net_feedback_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_net_feedbacks() const {
        return net_feedback_.list_ids();
    }

    // -- XR report (v1.2.0, session capability plan X1) ------------------
    //
    // Typed slot for the RTCP Extended Report consumer seam
    // (`plugins::IXrReport`).  The engine routes inbound RTCP XR
    // sub-packets (payload type 207) into it, and it decodes the
    // RFC 3611 report blocks for the host.
    //
    // The built-in implementation lives in `src/modules/rtp/` and
    // registers under the id "nimrtc_xr"; the engine resolves it
    // through `EngineConfig::xr_report_name`.
    void register_xr_report(std::string_view id,
                            const plugins::IXrReportFactory* f);
    [[nodiscard]] const plugins::IXrReportFactory*
    get_xr_report(std::string_view id) const {
        return xr_report_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_xr_reports() const {
        return xr_report_.list_ids();
    }

    // -- DataChannel (P2: typed factory slot for IDataChannelFactory*) -----
    //
    // Mirrors the SCTP / DTLS / raw_udp / transport-stack slots above.
    // Per `docs/plan/transport-selection.md` section 5.2 (Slice 5 follow-up) and
    // `src/plugins/include/nimrtc/plugins/datachannel.hpp`, the engine
    // resolves a channel backend by id (default "sctp") through this slot.
    // The public API surface (`register_*` / `get_*` / `list_*`) matches
    // every other typed registry hook; callers can register custom channel
    // backends here without depending on any concrete module.
    void register_datachannel(std::string_view id,
                              const plugins::IDataChannelFactory* f);
    /// Remove a datachannel factory by id.  Returns true if a
    /// matching entry was removed.  Test-only helper; mirrors
    /// `unregister_web_transport()` and exists so a TU-scope stub
    /// factory does not leak into later tests in the same process.
    /// The production path always registers through static-init
    /// registration and never removes a factory at runtime.
    bool unregister_datachannel(std::string_view id) {
        return datachannel_.unregister_one(id);
    }
    [[nodiscard]] const plugins::IDataChannelFactory*
    get_datachannel(std::string_view id) const {
        return datachannel_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_datachannels() const {
        return datachannel_.list_ids();
    }

    // -- DTLS session (Transport PAL Slice 4 / Slice 8 hook) --------------
    //
    // Typed slot for `nimrtc::dtls::IDtlsSessionFactory*`. The built-in
    // factory is `WolfsslDtlsFactory` (id = "wolfssl"); the seam is
    // open for replacement by OpenSSL / BoringSSL / mbedTLS / GMSSL
    // backends (see `docs/plan/transport-selection.md` ?5.2 / ?6.1).
    void register_dtls_session(std::string_view id,
                               const dtls::IDtlsSessionFactory* f);
    [[nodiscard]] const dtls::IDtlsSessionFactory*
    get_dtls_session(std::string_view id) const {
        return dtls_session_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_dtls_sessions() const {
        return dtls_session_.list_ids();
    }

    // -- SCTP socket (Transport PAL Slice 5 / Slice 8 hook) ---------------
    //
    // Typed slot for `nimrtc::sctp::ISctpSocketFactory*`. The Slice 5
    // default is the stub factory (id = "stub") ??every send returns
    // kErrNotReady so callers can detect the missing usrsctp backend.
    // v0.11.0 will register the production `UsrsctpSocketFactory`
    // (id = "usrsctp") alongside the stub (see
    // `docs/plan/transport-selection.md` ?6.2 / ?7).
    void register_sctp_socket(std::string_view id,
                              const sctp::ISctpSocketFactory* f);
    [[nodiscard]] const sctp::ISctpSocketFactory*
    get_sctp_socket(std::string_view id) const {
        return sctp_socket_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_sctp_sockets() const {
        return sctp_socket_.list_ids();
    }

    // -- Raw UDP datagram (Transport PAL Slice 6 / Slice 8 hook) ----------
    //
    // Typed slot for `nimrtc::raw_udp::IRawUdpFactory*`. The Slice 6
    // default is `ArqRawUdpFactory` (id = "arq"); v0.11.0 / Slice 7
    // will wire the factory into the ITransportStack composition so the
    // Selector can pick `raw-udp-arq` for the control stack per
    // `docs/plan/transport-selection.md` ?5.3 rule 1.
    void register_raw_udp_datagram(std::string_view id,
                                   const raw_udp::IRawUdpFactory* f);
    [[nodiscard]] const raw_udp::IRawUdpFactory*
    get_raw_udp_datagram(std::string_view id) const {
        return raw_udp_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_raw_udp_datagrams() const {
        return raw_udp_.list_ids();
    }

    // -- Transport stack (Transport PAL Slice 7 / Slice 8 hook) ------------
    //
    // Typed slot for `nimrtc::ITransportStackFactory*`. Slice 7 ships
    // the interface + CapabilitySelector; Slice 7.5 lands the concrete
    // `WebRtcClassicStackFactory` / `RawUdpArqStackFactory` impls. The
    // Slice 8 engine integration looks up a factory by id (e.g.
    // "webrtc-classic") and delegates ICE + DTLS + RTP + SCTP
    // composition to the resulting `ITransportStack`.
    //
    // For v0.10.2 (Slice 8) the slot is registered but the engine does
    // not yet switch to it ??that's a follow-up patch gated on
    // `WebRtcClassicStackFactory` (Slice 7.5) landing. The hook itself
    // is in place so the engine.cpp / Profile-loader changes can be
    // additive (no public API churn when Slice 7.5 lands).
    void register_transport_stack(std::string_view id,
                                  const ITransportStackFactory* f);
    [[nodiscard]] const ITransportStackFactory*
    get_transport_stack(std::string_view id) const {
        return transport_stack_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_transport_stacks() const {
        return transport_stack_.list_ids();
    }

    // -- WebTransport / QUIC session (v0.14.0 alpha.2 / ADR-014) -----------
    //
    // Typed slot for `nimrtc::web_transport::IWebTransportSessionFactory*`.
    // The seam (`IWebTransportSession` + companion headers) lands in
    // v0.14.0 alpha.2 (this PR); the msquic-backed factory
    // (`WebTransportMsQuicSessionFactory`, id = `"msquic"`) lands in
    // v0.14.0-beta.1 alongside the msquic integration.  At the alpha.2
    // cut, the slot exists but `list_web_transports()` returns an
    // empty vector -- no factory is registered yet.  The
    // `EngineConfig::datachannel_backend` field (lands in beta.1)
    // resolves factories by id through this slot; the runtime error
    // for a v0.14.0 alpha.2 caller that tries to use a web_transport
    // id is documented in `docs/plan/v0.14-plan.md` §3.1 Gate #3
    // ("loud-fail" contract; no silent fallback to wolfSSL).
    void register_web_transport(std::string_view id,
                                const web_transport::IWebTransportSessionFactory* f);
    /// Remove a web_transport factory by id.  Returns true if a
    /// matching entry was removed.  Test-only helper; the
    /// production path always registers through
    /// `NIMRTC_REGISTER_WEB_TRANSPORT(id, factory)` at static-init
    /// time and never removes a factory at runtime.
    bool unregister_web_transport(std::string_view id) {
        return web_transport_.unregister_one(id);
    }
    [[nodiscard]] const web_transport::IWebTransportSessionFactory*
    get_web_transport(std::string_view id) const {
        return web_transport_.get(id);
    }
    [[nodiscard]] std::vector<std::string> list_web_transports() const {
        return web_transport_.list_ids();
    }

private:
    TypedRegistry<plugins::ITransportFactory>    transport_;
    TypedRegistry<plugins::IICETransportFactory> ice_transport_;
    TypedRegistry<plugins::IRTPFactory>          rtp_;
    TypedRegistry<plugins::ISDPFactory>          sdp_;
    TypedRegistry<plugins::IJBFactory>           jb_;
    TypedRegistry<plugins::IAudio3AFactory>      audio3a_;
    TypedRegistry<plugins::ICodecFactory>        codec_;
    TypedRegistry<plugins::IAudioDecoderFactory> audio_decoder_;
    TypedRegistry<plugins::IAudioReceiverFactory> audio_receiver_;
    // v1.2.0 (session capability plan N1 / T1): the session-model
    // slot.  The built-in model registers under "session".
    TypedRegistry<plugins::ISessionFactory>     session_;
    // v1.2.0 (session capability plan D1 / D2): the RFC 4733
    // (telephone-event) slot.  The built-in codec registers under
    // "dtmf".
    TypedRegistry<plugins::IDtmfCodecFactory>    dtmf_;
    TypedRegistry<plugins::IVideoCodecFactory>   video_codec_;
    TypedRegistry<plugins::IVideoSourceFactory>  video_source_;
    // Remote-desktop plan A1: the capture-*device* seam.  Distinct from
    // the source seam above: a source produces frames, a capture produces
    // frames *from a device*, with enumeration, negotiation, region
    // selection and dirty-rect metadata.
    TypedRegistry<plugins::IVideoCaptureFactory> video_capture_;
    TypedRegistry<plugins::IVideoSinkFactory>    video_sink_;
    TypedRegistry<plugins::IVideoReceiverFactory> video_receiver_;
    TypedRegistry<plugins::IVideoSenderFactory>   video_sender_;
    TypedRegistry<plugins::IBweFactory>           bwe_;
    TypedRegistry<plugins::ISchedulerFactory>     scheduler_;
    // v1.2.0 M1 (F2): typed factory slot for the network-feedback
    // collector seam.  The built-in collector registers under
    // "nimrtc_net_feedback".
    TypedRegistry<plugins::INetFeedbackFactory>    net_feedback_;
    // v1.2.0 (session capability plan X1): typed factory slot for the
    // RTCP Extended Report consumer seam.  The built-in consumer
    // registers under "nimrtc_xr".
    TypedRegistry<plugins::IXrReportFactory>       xr_report_;
    // DataChannel P2 typed slot — see register_datachannel() above for
    // the rationale.
    TypedRegistry<plugins::IDataChannelFactory>   datachannel_;
    // Transport PAL Slice 8 (v0.10.2) ??typed factory slots for the
    // DTLS / SCTP / raw_udp / transport-stack seams. Each module's
    // `register_default_plugins()` publishes a single factory into
    // its matching slot via the `register_*` public methods above.
    TypedRegistry<dtls::IDtlsSessionFactory>       dtls_session_;
    TypedRegistry<sctp::ISctpSocketFactory>        sctp_socket_;
    TypedRegistry<raw_udp::IRawUdpFactory>         raw_udp_;
    TypedRegistry<ITransportStackFactory>          transport_stack_;
    // v0.14.0 alpha.2 (ADR-014): typed factory slot for the
    // WebTransport / QUIC seam.  Empty at the alpha.2 cut; the
    // msquic factory lands in beta.1 and is published via
    // `register_web_transport("msquic", &factory)`.  The slot
    // exists so the engine can resolve the factory id from
    // `EngineConfig::datachannel_backend` at any point in the
    // cycle.
    TypedRegistry<web_transport::IWebTransportSessionFactory>
                                                web_transport_;
};

// ---------------------------------------------------------------------------
// Registration macros (convenience, no linking magic)
// ---------------------------------------------------------------------------

/** Register a transport plugin by ID and factory pointer.
 *  Call at static-init time (global scope). */
#define NIMRTC_REGISTER_TRANSPORT(id, factory_ptr)                       \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_transport_){                              \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kTransport,      \
            #id, factory_ptr }

/** Register an ICE-aware transport factory.
 *  Equivalent to NIMRTC_REGISTER_TRANSPORT but typed as IICETransportFactory*
 *  so the registry exposes the ICE-specific surface (state, credentials,
 *  gathering, remote SDP). */
#define NIMRTC_REGISTER_ICE_TRANSPORT(id, factory_ptr)                   \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_ice_transport_){                          \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kICETransport,   \
            #id, factory_ptr }

/** Register an RTP plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_RTP(id, factory_ptr)                             \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_rtp_){                                   \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kRTP,             \
            #id, factory_ptr }

/** Register an SDP plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_SDP(id, factory_ptr)                             \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_sdp_){                                   \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kSDP,             \
            #id, factory_ptr }

/** Register a JB plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_JB(id, factory_ptr)                             \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_jb_){                                   \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kJB,            \
            #id, factory_ptr }

/** Register an audio 3A plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_AUDIO3A(id, factory_ptr)                        \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_audio3a_){                               \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kAudio3A,       \
            #id, factory_ptr }

/** Register a codec plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_CODEC(id, factory_ptr)                           \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_codec_){                                 \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kCodec,          \
            #id, factory_ptr }

/** Register an audio decoder plugin by ID and factory pointer.
 *  v1.2.0 (audio pipeline) -- the codec-side seam.
 *
 *  @param id         A string literal, e.g. "opus". Unlike the legacy
 *                    NIMRTC_REGISTER_* family (which stringifies a bare
 *                    identifier token), the two audio-seam macros take the
 *                    literal as-is -- the audio pipeline documents its
 *                    plugin ids as string literals everywhere else.
 *  @param factory_ptr A pointer to a static IAudioDecoderFactory.
 *
 *  Usage: NIMRTC_REGISTER_AUDIO_DECODER("opus", &g_opus_decoder_factory); */
#define NIMRTC_REGISTER_AUDIO_DECODER(id, factory_ptr)                   \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_audio_decoder_){                          \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kAudioDecoder,   \
            id, factory_ptr }

/** Register an audio receiver pipeline plugin by ID and factory pointer.
 *  v1.2.0 (audio pipeline) -- the pipeline-side seam.
 *
 *  @param id         A string literal, e.g. "nimrtc_audio_rx".
 *  @param factory_ptr A pointer to a static IAudioReceiverFactory.
 *
 *  Usage: NIMRTC_REGISTER_AUDIO_RECEIVER("nimrtc_audio_rx", &g_factory); */
#define NIMRTC_REGISTER_AUDIO_RECEIVER(id, factory_ptr)                  \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_audio_receiver_){                        \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kAudioReceiver,  \
            id, factory_ptr }

/** Register a session-model plugin by ID and factory pointer.
 *  v1.2.0 (session capability plan, N1 / T1) — typed slot for
 *  `plugins::ISessionFactory*`.
 *
 *  @param id         A string literal, e.g. "session".
 *  @param factory_ptr A pointer to a static ISessionFactory.
 *
 *  Usage: NIMRTC_REGISTER_SESSION("session", &g_factory); */
#define NIMRTC_REGISTER_SESSION(id, factory_ptr)                     \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_session_){                             \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kSession,     \
            id, factory_ptr }

/** Register an RFC 4733 (telephone-event) codec by ID and factory pointer.
 *  v1.2.0 (session capability plan, D1 / D2) — typed slot for
 *  `plugins::IDtmfCodecFactory*`.
 *
 *  @param id         A string literal, e.g. "dtmf".
 *  @param factory_ptr A pointer to a static IDtmfCodecFactory.
 *
 *  Usage: NIMRTC_REGISTER_DTMF("dtmf", &g_factory); */
#define NIMRTC_REGISTER_DTMF(id, factory_ptr)                       \
    static ::nimrtc::core::detail::Registrar                        \
        NIMRTC_UNIQUE_NAME(_reg_dtmf_){                              \
            ::nimrtc::core::PluginRegistry::instance(),              \
            ::nimrtc::core::detail::Registrar::Category::kDtmf,      \
            id, factory_ptr }

/** Register a video codec plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_VIDEO_CODEC(id, factory_ptr)                     \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_video_codec_){                           \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kVideoCodec,     \
            #id, factory_ptr }

/** Register a video source plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_VIDEO_SOURCE(id, factory_ptr)                    \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_video_source_){                          \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kVideoSource,    \
            #id, factory_ptr }

/** Register a capture-device plugin by ID and factory pointer
 *  (remote-desktop plan A1). */
#define NIMRTC_REGISTER_VIDEO_CAPTURE(id, factory_ptr)                   \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_video_capture_){                         \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kVideoCapture,   \
            #id, factory_ptr }

/** Register a video sink plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_VIDEO_SINK(id, factory_ptr)                      \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_video_sink_){                            \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kVideoSink,      \
            #id, factory_ptr }

/** Register a video receiver pipeline plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_VIDEO_RECEIVER(id, factory_ptr)                  \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_video_receiver_){                        \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kVideoReceiver,  \
            #id, factory_ptr }

/** Register a video sender pipeline plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_VIDEO_SENDER(id, factory_ptr)                    \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_video_sender_){                          \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kVideoSender,    \
            #id, factory_ptr }

/** Register a BWE (bandwidth estimator) plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_BWE(id, factory_ptr)                            \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_bwe_){                                   \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kBwe,           \
            #id, factory_ptr }

/** Register a sending-scheduler plugin by ID and factory pointer. */
#define NIMRTC_REGISTER_SCHEDULER(id, factory_ptr)                      \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_scheduler_){                             \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kScheduler,     \
            #id, factory_ptr }

/** Register a network-feedback collector plugin by ID and factory pointer.
 *  v1.2.0 M1 (F2) — typed slot for `plugins::INetFeedbackFactory*`. */
#define NIMRTC_REGISTER_NET_FEEDBACK(id, factory_ptr)                  \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_net_feedback_){                         \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kNetFeedback,  \
            #id, factory_ptr }

/** Register an RTCP Extended Report consumer plugin by ID and factory pointer.
 *  v1.2.0 (session capability plan X1) — typed slot for
 *  `plugins::IXrReportFactory*`. */
#define NIMRTC_REGISTER_XR_REPORT(id, factory_ptr)                     \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_xr_report_){                            \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kXrReport,     \
            #id, factory_ptr }

/** Register a DataChannel plugin by ID and factory pointer.
 *  P2 typed slot ??mirrors the SCTP / DTLS / raw_udp / transport-stack
 *  hooks above; see `register_datachannel()` for the canonical
 *  rationale. */
#define NIMRTC_REGISTER_DATACHANNEL(id, factory_ptr)                     \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_datachannel_){                           \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kDataChannel,   \
            #id, factory_ptr }

/** Register a DTLS session-factory plugin by ID and factory pointer.
 *  Transport PAL Slice 4 / Slice 8 (v0.10.2) ??typed slot for
 *  `nimrtc::dtls::IDtlsSessionFactory*`. */
#define NIMRTC_REGISTER_DTLS_SESSION(id, factory_ptr)                    \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_dtls_session_){                           \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kDtlsSession,   \
            #id, factory_ptr }

/** Register an SCTP socket-factory plugin by ID and factory pointer.
 *  Transport PAL Slice 5 / Slice 8 (v0.10.2) ??typed slot for
 *  `nimrtc::sctp::ISctpSocketFactory*`. */
#define NIMRTC_REGISTER_SCTP_SOCKET(id, factory_ptr)                     \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_sctp_socket_){                            \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kSctpSocket,    \
            #id, factory_ptr }

/** Register a raw-UDP datagram-factory plugin by ID and factory pointer.
 *  Transport PAL Slice 6 / Slice 8 (v0.10.2) ??typed slot for
 *  `nimrtc::raw_udp::IRawUdpFactory*`. */
#define NIMRTC_REGISTER_RAW_UDP_DATAGRAM(id, factory_ptr)                \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_raw_udp_datagram_){                       \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kRawUdpDatagram, \
            #id, factory_ptr }

/** Register a transport-stack factory plugin by ID and factory pointer.
 *  Transport PAL Slice 7 / Slice 8 (v0.10.2) ??typed slot for
 *  `nimrtc::ITransportStackFactory*`. The Slice 8 engine integration
 *  will use this hook when the engine switches to `ITransportStack*`
 *  composition (gated on Slice 7.5's `WebRtcClassicStackFactory`
 *  landing ??see `docs/plan/transport-selection.md` ?6.5). */
#define NIMRTC_REGISTER_TRANSPORT_STACK(id, factory_ptr)                 \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_transport_stack_){                       \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kTransportStack, \
            #id, factory_ptr }

/** Register a WebTransport / QUIC session-factory plugin by ID and
 *  factory pointer.  v0.14.0 alpha.2 (ADR-014) — typed slot for
 *  `nimrtc::web_transport::IWebTransportSessionFactory*`.  At the
 *  alpha.2 cut, no factory is auto-registered (the slot exists but
 *  `list_web_transports()` returns an empty vector); the msquic
 *  factory lands in beta.1 via
 *  `NIMRTC_REGISTER_WEB_TRANSPORT("msquic", &factory)`. */
#define NIMRTC_REGISTER_WEB_TRANSPORT(id, factory_ptr)                  \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_web_transport_){                        \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kWebTransport, \
            #id, factory_ptr }

namespace detail {

// Tiny utility macros
#define NIMRTC_UNIQUE_NAME(prefix) NIMRTC_CONCAT(prefix, __LINE__)
#define NIMRTC_CONCAT(a, b) NIMRTC_CONCAT2(a, b)
#define NIMRTC_CONCAT2(a, b) a##b

/**
 * @brief Function-pointer signature for the per-module `register_default_plugins()`
 *        entry points listed in `kDefaultRegistrars[]`.
 *
 * Defined here (header) so the inline `register_all_default_plugins()`
 * body in `nimrtc::core` can reference it without an extra include.
 * The concrete array `detail::kDefaultRegistrars` and its size
 * `detail::kDefaultRegistrarCount` live in
 * `src/core/src/pal_default_registrars.cpp` (PAL Slice 2) and are
 * forward-declared here so the inline body can link them.
 */
using RegistrarFn = void(*)() noexcept;

/**
 * @brief Array of function pointers to every built-in plugin's
 *        `register_default_plugins()`.  Defined in
 *        `src/core/src/pal_default_registrars.cpp`.
 */
extern const RegistrarFn kDefaultRegistrars[];

/**
 * @brief Number of entries in `kDefaultRegistrars`.  Defined alongside
 *        the array in `src/core/src/pal_default_registrars.cpp`.
 */
extern const std::size_t kDefaultRegistrarCount;

class Registrar {
public:
    enum class Category {
        kTransport, kICETransport, kRTP, kSDP, kJB, kAudio3A, kCodec, kVideoCodec,
        // v1.2.0 (audio pipeline): the two new audio seams -- the
        // codec-side decoder and the pipeline-side receiver.
        kAudioDecoder, kAudioReceiver,
        // v1.2.0 (session capability plan N1 / T1): the session-model
        // seam -- see register_session().
        kSession,
        // v1.2.0 (session capability plan D1 / D2): the RFC 4733
        // (telephone-event) seam -- see register_dtmf().  It has its
        // own category because a telephone-event payload is not audio.
        kDtmf,
        kVideoSource, kVideoSink, kVideoReceiver, kVideoSender,
        // Remote-desktop plan A1: the capture-device seam.  It has its
        // own category because a capture device is not a frame source:
        // it enumerates devices and negotiates a format.
        kVideoCapture,
        kBwe, kScheduler,
        // v1.2.0 M1 (F2): the receiver-side network-feedback collector
        // seam -- see register_net_feedback().
        kNetFeedback,
        // v1.2.0 (session capability plan X1): the RTCP Extended Report
        // consumer seam -- see register_xr_report().
        kXrReport,
        // P2: DataChannel typed factory slot ??see register_datachannel().
        kDataChannel,
        // Transport PAL Slice 8 (v0.10.2): typed factory slots for the
        // DTLS / SCTP / raw_udp / transport-stack seams. See the
        // NIMRTC_REGISTER_DTLS_SESSION / SCTP_SOCKET / RAW_UDP_DATAGRAM /
        // TRANSPORT_STACK macros above for the matching registration
        // entry points.
        kDtlsSession, kSctpSocket, kRawUdpDatagram, kTransportStack,
        // v0.14.0 alpha.2 (ADR-014): WebTransport / QUIC seam.
        // The factory slot lands in alpha.2; the msquic factory
        // lands in beta.1 via NIMRTC_REGISTER_WEB_TRANSPORT().
        kWebTransport
    };

    Registrar(core::PluginRegistry& reg, Category cat,
              std::string_view id, const void* factory) {
        switch (cat) {
            case Category::kTransport:
                reg.register_transport(id,
                    static_cast<const plugins::ITransportFactory*>(factory));
                break;
            case Category::kICETransport:
                reg.register_ice_transport(id,
                    static_cast<const plugins::IICETransportFactory*>(factory));
                break;
            case Category::kRTP:
                reg.register_rtp(id,
                    static_cast<const plugins::IRTPFactory*>(factory));
                break;
            case Category::kSDP:
                reg.register_sdp(id,
                    static_cast<const plugins::ISDPFactory*>(factory));
                break;
            case Category::kJB:
                reg.register_jb(id,
                    static_cast<const plugins::IJBFactory*>(factory));
                break;
            case Category::kAudio3A:
                reg.register_audio3a(id,
                    static_cast<const plugins::IAudio3AFactory*>(factory));
                break;
            case Category::kCodec:
                reg.register_codec(id,
                    static_cast<const plugins::ICodecFactory*>(factory));
                break;
            case Category::kAudioDecoder:
                reg.register_audio_decoder(id,
                    static_cast<const plugins::IAudioDecoderFactory*>(factory));
                break;
            case Category::kAudioReceiver:
                reg.register_audio_receiver(id,
                    static_cast<const plugins::IAudioReceiverFactory*>(factory));
                break;
            case Category::kSession:
                reg.register_session(id,
                    static_cast<const plugins::ISessionFactory*>(factory));
                break;
            case Category::kDtmf:
                reg.register_dtmf(id,
                    static_cast<const plugins::IDtmfCodecFactory*>(factory));
                break;
            case Category::kVideoCodec:
                reg.register_video_codec(id,
                    static_cast<const plugins::IVideoCodecFactory*>(factory));
                break;
            case Category::kVideoSource:
                reg.register_video_source(id,
                    static_cast<const plugins::IVideoSourceFactory*>(factory));
                break;
            case Category::kVideoCapture:
                reg.register_video_capture(id,
                    static_cast<const plugins::IVideoCaptureFactory*>(factory));
                break;
            case Category::kVideoSink:
                reg.register_video_sink(id,
                    static_cast<const plugins::IVideoSinkFactory*>(factory));
                break;
            case Category::kVideoReceiver:
                reg.register_video_receiver(id,
                    static_cast<const plugins::IVideoReceiverFactory*>(factory));
                break;
            case Category::kVideoSender:
                reg.register_video_sender(id,
                    static_cast<const plugins::IVideoSenderFactory*>(factory));
                break;
            case Category::kBwe:
                reg.register_bwe(id,
                    static_cast<const plugins::IBweFactory*>(factory));
                break;
            case Category::kScheduler:
                reg.register_scheduler(id,
                    static_cast<const plugins::ISchedulerFactory*>(factory));
                break;
            case Category::kNetFeedback:
                reg.register_net_feedback(id,
                    static_cast<const plugins::INetFeedbackFactory*>(factory));
                break;
            case Category::kXrReport:
                reg.register_xr_report(id,
                    static_cast<const plugins::IXrReportFactory*>(factory));
                break;
            case Category::kDataChannel:
                reg.register_datachannel(id,
                    static_cast<const plugins::IDataChannelFactory*>(factory));
                break;
            case Category::kDtlsSession:
                reg.register_dtls_session(id,
                    static_cast<const dtls::IDtlsSessionFactory*>(factory));
                break;
            case Category::kSctpSocket:
                reg.register_sctp_socket(id,
                    static_cast<const sctp::ISctpSocketFactory*>(factory));
                break;
            case Category::kRawUdpDatagram:
                reg.register_raw_udp_datagram(id,
                    static_cast<const raw_udp::IRawUdpFactory*>(factory));
                break;
            case Category::kTransportStack:
                reg.register_transport_stack(id,
                    static_cast<const ITransportStackFactory*>(factory));
                break;
            case Category::kWebTransport:
                reg.register_web_transport(id,
                    static_cast<const web_transport::IWebTransportSessionFactory*>(factory));
                break;
        }
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// Unified registration entry point
// ---------------------------------------------------------------------------
//
// Iterates `detail::kDefaultRegistrars[]` (defined in
// `src/core/src/pal_default_registrars.cpp`, PAL Slice 2) and calls every
// module's `register_default_plugins()`.  Idempotent ??each module uses
// Meyer's-singleton latches internally.
//
// MUST be called once at program startup before any PluginRegistry lookup.
// Consumers linking this function MUST also link every concrete module
// library (audio3a, bwe, h264, ice, jb, opus, rtp, sched, sdp,
// video_pipeline, video_sink, video_source) ??the forward declarations above
// resolve at link time.
//
// If you only need a subset of modules (e.g. a test that just exercises
// audio3a), call the module-specific entry point directly:
//   nimrtc::audio3a::register_default_plugins();
//
// NOTE: `detail::kDefaultRegistrarCount` is declared in
// `src/core/src/pal_default_registrars.cpp` and referenced here via the
// `detail` namespace.  This file is compiled into `nimrtc_core_objects`
// (an OBJECT library linked INTERFACE by `nimrtc::core`), so both symbols
// are available at link time without any additional header.
// ---------------------------------------------------------------------------

inline void register_all_default_plugins() noexcept {
    for (std::size_t i = 0; i < detail::kDefaultRegistrarCount; ++i) {
        detail::kDefaultRegistrars[i]();
    }
}

} // namespace nimrtc::core

// ---------------------------------------------------------------------------
// Backward-compatibility shim: expose the registry also in plugins namespace.
// Existing code that includes <nimrtc/plugins/registry.hpp> gets this
// automatically.  New code should use nimrtc/core/registry.hpp directly.
//
// Layering (P1 hardening, 2026-11-09):
//   This header previously included 18 seam headers (13 under
//   nimrtc/plugins/ plus the DTLS / SCTP / raw_udp / transport-stack /
//   WebTransport factory headers from five OPTIONAL modules).  That made
//   every translation unit including <nimrtc/core/registry.hpp> pull in
//   the whole optional-dependency surface, defeating the compile-time
//   module trimming the CMake options provide, and made core depend on
//   modules it must not know about (ARCHITECTURE.md Layout Invariant 1).
//
//   The seam headers now live behind explicit opt-in headers:
//     - <nimrtc/plugins/registry.hpp>  -- the 13 nimrtc/plugins/ seams
//     - <nimrtc/dtls/dtls_session_factory.hpp>, <nimrtc/sctp/...>,
//       <nimrtc/raw_udp/...>, <nimrtc/transport/transport_stack.hpp>,
//       <nimrtc/web_transport/...>  -- the optional-module seams,
//       included directly by the code that needs them.
//
//   The registry API only needs POINTERS to the factory interfaces, so
//   the forward declarations above are sufficient: a caller that
//   registers a factory already has the complete type (it implements
//   it), and a caller that only looks one up needs no definition.
// ---------------------------------------------------------------------------

namespace nimrtc::plugins {

// Convenience using-declarations so callers can use
// plugins::PluginRegistry::instance() without change.
using core::PluginRegistry;

} // namespace nimrtc::plugins

#endif // NIMRTC_CORE_REGISTRY_HPP
