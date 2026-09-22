/**
 * @file nimrtc/core/registry.hpp
 * @brief PluginRegistry — global factory registry for NimRTC plugins.
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
 * cfg.rtp_name      = "webrtc";
 * cfg.jb_name       = "adaptive";
 * NimRTCEngine engine(cfg);
 * engine.open();
 * @endcode
 *
 * @note Thread-safe for get_*() calls. register_*() is not thread-safe
 * and must be called before any engine is created.
 *
 * @note P0 scaffold — registry is in-memory only; binary-safe plugin loading
 * (dlopen / LoadLibrary) is TBD P4.
 */

#ifndef NIMRTC_CORE_REGISTRY_HPP
#define NIMRTC_CORE_REGISTRY_HPP

#include <string_view>
#include <optional>
#include <vector>
#include <functional>
#include <mutex>
#include <cassert>

// Forward declarations of plugin interfaces — no full definitions needed here,
// just the factory type names. Full definitions are in the respective headers.
namespace nimrtc::plugins {
class ITransportFactory;
class IICETransportFactory;
class IRTPFactory;
class ISDPFactory;
class IJBFactory;
class IAudio3AFactory;
class ICodecFactory;
class IVideoCodecFactory;
class IVideoSourceFactory;
class IVideoSinkFactory;
class IVideoReceiverFactory;
class IVideoSenderFactory;
class IBweFactory;
class ISchedulerFactory;
class IDataChannelFactory;
} // namespace plugins

// Transport PAL Slice 8 (v0.10.2): typed factory slots for DTLS / SCTP /
// raw_udp / transport-stack. Each module's seam factory inherits its
// respective interface (IDtlsSessionFactory / ISctpSocketFactory /
// IRawUdpFactory / ITransportStackFactory) and is published via the
// matching `register_*` hook on the registry below.
//
// Forward-declared here so the registry's TypedRegistry<...> fields and
// public method signatures can reference them without pulling the seam
// headers into core (preserves Layout Invariant 1 — core stays header-
// only + the single .cpp exception in `pal_default_registrars.cpp`).
namespace nimrtc::dtls    { class IDtlsSessionFactory; }
namespace nimrtc::sctp     { class ISctpSocketFactory; }
namespace nimrtc::raw_udp  { class IRawUdpFactory; }
namespace nimrtc           { class ITransportStackFactory; }

// ---------------------------------------------------------------------------
// Forward declarations for unified registration entry point.
//
// Each concrete module exposes `register_default_plugins()` (defined in its
// plugin adapter .cpp). The unified `core::register_all_default_plugins()`
// below calls each enabled module. Consumers linking the unified entry point
// MUST also link the corresponding module libraries — the forward declarations
// below intentionally avoid pulling any module header into core (Layout Invariant 1).
// ---------------------------------------------------------------------------

namespace nimrtc {
namespace ice     { void register_default_plugins() noexcept; }
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
#ifdef NIMRTC_HAS_VIDEO_SOURCE
namespace video_source    { void register_default_plugins() noexcept; }
#endif
#ifdef NIMRTC_HAS_VIDEO_PIPELINE
namespace video_pipeline  { void register_default_plugins() noexcept; }
#endif
#ifdef NIMRTC_HAS_VIDEO_SINK
namespace video_sink      { void register_default_plugins() noexcept; }
#endif
namespace bwe             { void register_default_plugins() noexcept; }
namespace sched           { void register_default_plugins() noexcept; }
namespace datachannel      { void register_default_plugins() noexcept; }
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
} // namespace nimrtc

namespace nimrtc::core {

// ---------------------------------------------------------------------------
// Detail
// ---------------------------------------------------------------------------

template<class T>
class TypedRegistry {
    std::vector<std::pair<std::string_view, const T*>> entries_;
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
        entries_.push_back({id, factory});
    }

    [[nodiscard]] const T* get(std::string_view id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [k, v] : entries_) {
            if (k == id) return v;
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<std::string_view> list_ids() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string_view> out;
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
// PluginRegistry — lives in nimrtc::core:: per Layout Invariant 6
// ---------------------------------------------------------------------------

class PluginRegistry {
    PluginRegistry() = default;
    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

public:
    static PluginRegistry& instance() {
        // Guaranteed destroyed, thread-safe in C++11+.
        static PluginRegistry inst;
        return inst;
    }

    // -- Transport -----------------------------------------------------------
    void register_transport(std::string_view id,
                            const plugins::ITransportFactory* f) {
        transport_.register_one(id, f);
    }
    [[nodiscard]] const plugins::ITransportFactory*
    get_transport(std::string_view id) const {
        return transport_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_transports() const {
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
                                const plugins::IICETransportFactory* f) {
        ice_transport_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IICETransportFactory*
    get_ice_transport(std::string_view id) const {
        return ice_transport_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_ice_transports() const {
        return ice_transport_.list_ids();
    }

    // -- RTP ----------------------------------------------------------------
    void register_rtp(std::string_view id, const plugins::IRTPFactory* f) {
        rtp_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IRTPFactory* get_rtp(std::string_view id) const {
        return rtp_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_rtp() const {
        return rtp_.list_ids();
    }

    // -- SDP ----------------------------------------------------------------
    void register_sdp(std::string_view id, const plugins::ISDPFactory* f) {
        sdp_.register_one(id, f);
    }
    [[nodiscard]] const plugins::ISDPFactory* get_sdp(std::string_view id) const {
        return sdp_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_sdp() const {
        return sdp_.list_ids();
    }

    // -- JB -----------------------------------------------------------------
    void register_jb(std::string_view id, const plugins::IJBFactory* f) {
        jb_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IJBFactory* get_jb(std::string_view id) const {
        return jb_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_jb() const {
        return jb_.list_ids();
    }

    // -- Audio 3A -----------------------------------------------------------
    void register_audio3a(std::string_view id, const plugins::IAudio3AFactory* f) {
        audio3a_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IAudio3AFactory*
    get_audio3a(std::string_view id) const {
        return audio3a_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_audio3a() const {
        return audio3a_.list_ids();
    }

    // -- Codec --------------------------------------------------------------
    void register_codec(std::string_view id,
                        const plugins::ICodecFactory* f) {
        codec_.register_one(id, f);
    }
    [[nodiscard]] const plugins::ICodecFactory*
    get_codec(std::string_view id) const {
        return codec_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_codecs() const {
        return codec_.list_ids();
    }

    // -- Video Codec --------------------------------------------------------
    void register_video_codec(std::string_view id,
                              const plugins::IVideoCodecFactory* f) {
        video_codec_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IVideoCodecFactory*
    get_video_codec(std::string_view id) const {
        return video_codec_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_video_codecs() const {
        return video_codec_.list_ids();
    }

    // -- Video Source -------------------------------------------------------
    void register_video_source(std::string_view id,
                               const plugins::IVideoSourceFactory* f) {
        video_source_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IVideoSourceFactory*
    get_video_source(std::string_view id) const {
        return video_source_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_video_sources() const {
        return video_source_.list_ids();
    }

    // -- Video Sink ---------------------------------------------------------
    void register_video_sink(std::string_view id,
                             const plugins::IVideoSinkFactory* f) {
        video_sink_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IVideoSinkFactory*
    get_video_sink(std::string_view id) const {
        return video_sink_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_video_sinks() const {
        return video_sink_.list_ids();
    }

    // -- Video Receiver -----------------------------------------------------
    void register_video_receiver(std::string_view id,
                                 const plugins::IVideoReceiverFactory* f) {
        video_receiver_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IVideoReceiverFactory*
    get_video_receiver(std::string_view id) const {
        return video_receiver_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_video_receivers() const {
        return video_receiver_.list_ids();
    }

    // -- Video Sender -------------------------------------------------------
    void register_video_sender(std::string_view id,
                               const plugins::IVideoSenderFactory* f) {
        video_sender_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IVideoSenderFactory*
    get_video_sender(std::string_view id) const {
        return video_sender_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_video_senders() const {
        return video_sender_.list_ids();
    }

    // -- BWE (bandwidth estimator) -----------------------------------------
    void register_bwe(std::string_view id,
                      const plugins::IBweFactory* f) {
        bwe_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IBweFactory*
    get_bwe(std::string_view id) const {
        return bwe_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_bwes() const {
        return bwe_.list_ids();
    }

    // -- Scheduler (unified sending) ---------------------------------------
    void register_scheduler(std::string_view id,
                            const plugins::ISchedulerFactory* f) {
        scheduler_.register_one(id, f);
    }
    [[nodiscard]] const plugins::ISchedulerFactory*
    get_scheduler(std::string_view id) const {
        return scheduler_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_schedulers() const {
        return scheduler_.list_ids();
    }

    // -- DataChannel (P2 — typed factory slot for IDataChannelFactory*) -----
    //
    // Mirrors the SCTP / DTLS / raw_udp / transport-stack slots above.
    // Per `docs/plan/transport-selection.md` §5.2 (Slice 5 follow-up) and
    // `src/plugins/include/nimrtc/plugins/datachannel.hpp`, the engine
    // resolves a channel backend by id (default "sctp") through this slot.
    //
    // Subagent B (core/PluginRegistry owner) is the canonical owner of this
    // hook. This inline stub was added by the datachannel subagent because
    // (a) the registry slot is required for `SctpDataChannelFactory::create()`
    //     consumers to actually receive a working channel, and (b) the
    //     subagent boundary is "additive only" — Subagent B can replace
    //     this stub with its own equivalent implementation without breaking
    //     callers because the public API surface (the three methods below)
    //     is the same shape as every other typed registry hook.
    void register_datachannel(std::string_view id,
                              const plugins::IDataChannelFactory* f) {
        datachannel_.register_one(id, f);
    }
    [[nodiscard]] const plugins::IDataChannelFactory*
    get_datachannel(std::string_view id) const {
        return datachannel_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_datachannels() const {
        return datachannel_.list_ids();
    }

    // -- DTLS session (Transport PAL Slice 4 / Slice 8 hook) --------------
    //
    // Typed slot for `nimrtc::dtls::IDtlsSessionFactory*`. The built-in
    // factory is `WolfsslDtlsFactory` (id = "wolfssl"); the seam is
    // open for replacement by OpenSSL / BoringSSL / mbedTLS / 国密
    // backends (see `docs/plan/transport-selection.md` §5.2 / §6.1).
    void register_dtls_session(std::string_view id,
                               const dtls::IDtlsSessionFactory* f) {
        dtls_session_.register_one(id, f);
    }
    [[nodiscard]] const dtls::IDtlsSessionFactory*
    get_dtls_session(std::string_view id) const {
        return dtls_session_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_dtls_sessions() const {
        return dtls_session_.list_ids();
    }

    // -- SCTP socket (Transport PAL Slice 5 / Slice 8 hook) ---------------
    //
    // Typed slot for `nimrtc::sctp::ISctpSocketFactory*`. The Slice 5
    // default is the stub factory (id = "stub") — every send returns
    // kErrNotReady so callers can detect the missing usrsctp backend.
    // v0.11.0 will register the production `UsrsctpSocketFactory`
    // (id = "usrsctp") alongside the stub (see
    // `docs/plan/transport-selection.md` §6.2 / §7).
    void register_sctp_socket(std::string_view id,
                              const sctp::ISctpSocketFactory* f) {
        sctp_socket_.register_one(id, f);
    }
    [[nodiscard]] const sctp::ISctpSocketFactory*
    get_sctp_socket(std::string_view id) const {
        return sctp_socket_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_sctp_sockets() const {
        return sctp_socket_.list_ids();
    }

    // -- Raw UDP datagram (Transport PAL Slice 6 / Slice 8 hook) ----------
    //
    // Typed slot for `nimrtc::raw_udp::IRawUdpFactory*`. The Slice 6
    // default is `ArqRawUdpFactory` (id = "arq"); v0.11.0 / Slice 7
    // will wire the factory into the ITransportStack composition so the
    // Selector can pick `raw-udp-arq` for the control stack per
    // `docs/plan/transport-selection.md` §5.3 rule 1.
    void register_raw_udp_datagram(std::string_view id,
                                   const raw_udp::IRawUdpFactory* f) {
        raw_udp_.register_one(id, f);
    }
    [[nodiscard]] const raw_udp::IRawUdpFactory*
    get_raw_udp_datagram(std::string_view id) const {
        return raw_udp_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_raw_udp_datagrams() const {
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
    // not yet switch to it — that's a follow-up patch gated on
    // `WebRtcClassicStackFactory` (Slice 7.5) landing. The hook itself
    // is in place so the engine.cpp / Profile-loader changes can be
    // additive (no public API churn when Slice 7.5 lands).
    void register_transport_stack(std::string_view id,
                                  const ITransportStackFactory* f) {
        transport_stack_.register_one(id, f);
    }
    [[nodiscard]] const ITransportStackFactory*
    get_transport_stack(std::string_view id) const {
        return transport_stack_.get(id);
    }
    [[nodiscard]] std::vector<std::string_view> list_transport_stacks() const {
        return transport_stack_.list_ids();
    }

private:
    TypedRegistry<plugins::ITransportFactory>    transport_;
    TypedRegistry<plugins::IICETransportFactory> ice_transport_;
    TypedRegistry<plugins::IRTPFactory>          rtp_;
    TypedRegistry<plugins::ISDPFactory>          sdp_;
    TypedRegistry<plugins::IJBFactory>           jb_;
    TypedRegistry<plugins::IAudio3AFactory>      audio3a_;
    TypedRegistry<plugins::ICodecFactory>        codec_;
    TypedRegistry<plugins::IVideoCodecFactory>   video_codec_;
    TypedRegistry<plugins::IVideoSourceFactory>  video_source_;
    TypedRegistry<plugins::IVideoSinkFactory>    video_sink_;
    TypedRegistry<plugins::IVideoReceiverFactory> video_receiver_;
    TypedRegistry<plugins::IVideoSenderFactory>   video_sender_;
    TypedRegistry<plugins::IBweFactory>           bwe_;
    TypedRegistry<plugins::ISchedulerFactory>     scheduler_;
    // DataChannel P2 typed slot — see register_datachannel() above for
    // the rationale (inline stub; Subagent B will replace with the
    // canonical implementation).
    TypedRegistry<plugins::IDataChannelFactory>   datachannel_;
    // Transport PAL Slice 8 (v0.10.2) — typed factory slots for the
    // DTLS / SCTP / raw_udp / transport-stack seams. Each module's
    // `register_default_plugins()` publishes a single factory into
    // its matching slot via the `register_*` public methods above.
    TypedRegistry<dtls::IDtlsSessionFactory>       dtls_session_;
    TypedRegistry<sctp::ISctpSocketFactory>        sctp_socket_;
    TypedRegistry<raw_udp::IRawUdpFactory>         raw_udp_;
    TypedRegistry<ITransportStackFactory>          transport_stack_;
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

/** Register a DataChannel plugin by ID and factory pointer.
 *  P2 typed slot — mirrors the SCTP / DTLS / raw_udp / transport-stack
 *  hooks above; see `register_datachannel()` for the canonical
 *  rationale. */
#define NIMRTC_REGISTER_DATACHANNEL(id, factory_ptr)                     \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_datachannel_){                           \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kDataChannel,   \
            #id, factory_ptr }

/** Register a DTLS session-factory plugin by ID and factory pointer.
 *  Transport PAL Slice 4 / Slice 8 (v0.10.2) — typed slot for
 *  `nimrtc::dtls::IDtlsSessionFactory*`. */
#define NIMRTC_REGISTER_DTLS_SESSION(id, factory_ptr)                    \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_dtls_session_){                           \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kDtlsSession,   \
            #id, factory_ptr }

/** Register an SCTP socket-factory plugin by ID and factory pointer.
 *  Transport PAL Slice 5 / Slice 8 (v0.10.2) — typed slot for
 *  `nimrtc::sctp::ISctpSocketFactory*`. */
#define NIMRTC_REGISTER_SCTP_SOCKET(id, factory_ptr)                     \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_sctp_socket_){                            \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kSctpSocket,    \
            #id, factory_ptr }

/** Register a raw-UDP datagram-factory plugin by ID and factory pointer.
 *  Transport PAL Slice 6 / Slice 8 (v0.10.2) — typed slot for
 *  `nimrtc::raw_udp::IRawUdpFactory*`. */
#define NIMRTC_REGISTER_RAW_UDP_DATAGRAM(id, factory_ptr)                \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_raw_udp_datagram_){                       \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kRawUdpDatagram, \
            #id, factory_ptr }

/** Register a transport-stack factory plugin by ID and factory pointer.
 *  Transport PAL Slice 7 / Slice 8 (v0.10.2) — typed slot for
 *  `nimrtc::ITransportStackFactory*`. The Slice 8 engine integration
 *  will use this hook when the engine switches to `ITransportStack*`
 *  composition (gated on Slice 7.5's `WebRtcClassicStackFactory`
 *  landing — see `docs/plan/transport-selection.md` §6.5). */
#define NIMRTC_REGISTER_TRANSPORT_STACK(id, factory_ptr)                 \
    static ::nimrtc::core::detail::Registrar                          \
        NIMRTC_UNIQUE_NAME(_reg_transport_stack_){                       \
            ::nimrtc::core::PluginRegistry::instance(),                \
            ::nimrtc::core::detail::Registrar::Category::kTransportStack, \
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
        kVideoSource, kVideoSink, kVideoReceiver, kVideoSender,
        kBwe, kScheduler,
        // P2: DataChannel typed factory slot — see register_datachannel().
        kDataChannel,
        // Transport PAL Slice 8 (v0.10.2): typed factory slots for the
        // DTLS / SCTP / raw_udp / transport-stack seams. See the
        // NIMRTC_REGISTER_DTLS_SESSION / SCTP_SOCKET / RAW_UDP_DATAGRAM /
        // TRANSPORT_STACK macros above for the matching registration
        // entry points.
        kDtlsSession, kSctpSocket, kRawUdpDatagram, kTransportStack
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
            case Category::kVideoCodec:
                reg.register_video_codec(id,
                    static_cast<const plugins::IVideoCodecFactory*>(factory));
                break;
            case Category::kVideoSource:
                reg.register_video_source(id,
                    static_cast<const plugins::IVideoSourceFactory*>(factory));
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
// module's `register_default_plugins()`.  Idempotent — each module uses
// Meyer's-singleton latches internally.
//
// MUST be called once at program startup before any PluginRegistry lookup.
// Consumers linking this function MUST also link every concrete module
// library (audio3a, bwe, h264, ice, jb, opus, rtp, sched, sdp,
// video_pipeline, video_sink, video_source) — the forward declarations above
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
// Existing code that includes plugins/registry.hpp gets this automatically.
// New code should use nimrtc/core/registry.hpp directly.
// ---------------------------------------------------------------------------
#include "nimrtc/plugins/transport.hpp"
#include "nimrtc/plugins/ice_transport.hpp"
#include "nimrtc/plugins/rtp.hpp"
#include "nimrtc/plugins/sdp.hpp"
#include "nimrtc/plugins/jb.hpp"
#include "nimrtc/plugins/audio3a.hpp"
#include "nimrtc/plugins/codec.hpp"
#include "nimrtc/plugins/video_codec.hpp"
#include "nimrtc/plugins/video_source.hpp"
#include "nimrtc/plugins/video_sink.hpp"
#include "nimrtc/plugins/video_pipeline.hpp"
#include "nimrtc/plugins/bwe.hpp"
#include "nimrtc/plugins/scheduler.hpp"

// Transport PAL Slice 8 (v0.10.2): seam headers for the typed factory
// slots (DTLS / SCTP / raw_udp / transport-stack). Including them here
// is what makes the back-compat shim work for old code that only
// includes <nimrtc/core/registry.hpp> and then references the new
// factory types — the include propagates through the rest of the
// project's translation units without forcing every TU to add the
// include itself.
#include "nimrtc/dtls/dtls_session_factory.hpp"
#include "nimrtc/sctp/sctp_socket_factory.hpp"
#include "nimrtc/raw_udp/raw_udp_factory_iface.hpp"
#include "nimrtc/transport/transport_stack.hpp"

namespace nimrtc::plugins {

// Convenience using-declarations so callers can use
// plugins::PluginRegistry::instance() without change.
using core::PluginRegistry;

} // namespace nimrtc::plugins

#endif // NIMRTC_CORE_REGISTRY_HPP
