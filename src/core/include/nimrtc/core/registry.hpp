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
} // namespace plugins

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

namespace detail {

// Tiny utility macros
#define NIMRTC_UNIQUE_NAME(prefix) NIMRTC_CONCAT(prefix, __LINE__)
#define NIMRTC_CONCAT(a, b) NIMRTC_CONCAT2(a, b)
#define NIMRTC_CONCAT2(a, b) a##b

class Registrar {
public:
    enum class Category {
        kTransport, kICETransport, kRTP, kSDP, kJB, kAudio3A, kCodec, kVideoCodec,
        kVideoSource, kVideoSink, kVideoReceiver, kVideoSender,
        kBwe, kScheduler
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
        }
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// Unified registration entry point
// ---------------------------------------------------------------------------
//
// Convenience wrapper that calls every module's `register_default_plugins()`.
// Idempotent — each module uses Meyer's-singleton latches internally.
//
// MUST be called once at program startup before any PluginRegistry lookup.
// Consumers linking this function MUST also link every concrete module
// library (ice, rtp, sdp, jb, audio3a) — the forward declarations above
// resolve at link time.
//
// If you only need a subset of modules (e.g. a test that just exercises
// audio3a), call the module-specific entry point directly:
//   nimrtc::audio3a::register_default_plugins();
// ---------------------------------------------------------------------------

inline void register_all_default_plugins() noexcept {
    nimrtc::ice::register_default_plugins();
    nimrtc::rtp::register_default_plugins();
    nimrtc::sdp::register_default_plugins();
    nimrtc::jb::register_default_plugins();
    nimrtc::audio3a::register_default_plugins();
#ifdef NIMRTC_HAS_OPUS
    nimrtc::opus::register_default_plugins();
#endif
#ifdef NIMRTC_HAS_H264
    nimrtc::h264::register_default_plugins();
#endif
#ifdef NIMRTC_HAS_VIDEO_SOURCE
    nimrtc::video_source::register_default_plugins();
#endif
#ifdef NIMRTC_HAS_VIDEO_PIPELINE
    nimrtc::video_pipeline::register_default_plugins();
#endif
#ifdef NIMRTC_HAS_VIDEO_SINK
    nimrtc::video_sink::register_default_plugins();
#endif
    nimrtc::bwe::register_default_plugins();
    nimrtc::sched::register_default_plugins();
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

namespace nimrtc::plugins {

// Convenience using-declarations so callers can use
// plugins::PluginRegistry::instance() without change.
using core::PluginRegistry;

} // namespace nimrtc::plugins

#endif // NIMRTC_CORE_REGISTRY_HPP
