/**
 * @file src/modules/ice/src/ice.cpp
 * @brief ICE Transport — libjuice-backed ITransport implementation.
 *
 * Wires the libjuice ICE agent (vendored under third_party/libjuice/) into the
 * NimRTC plugin transport interface (ITransport). All STUN/TURN/connectivity
 * check work is delegated to libjuice; this file is a thin adapter.
 *
 * ## Threading
 *
 * - libjuice runs in JUICE_CONCURRENCY_MODE_MUX by default. Its internal
 *   thread owns the UDP socket and dispatches all STUN/TURN/data I/O.
 * - Inbound data packets and state-change/candidate/gathering-done events
 *   arrive on libjuice's thread. We forward them to the engine via the
 *   ITransport callbacks after a mutex-protected handoff.
 * - The engine thread drives `recv()` to drain queued packets and calls
 *   all other IceTransport methods. These are serialized against the
 *   libjuice callbacks via the same mutex.
 */

#include <nimrtc/ice/ice.hpp>

#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <vector>

#include <juice/juice.h>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>   // PluginRegistry (Layout Invariant 6)

namespace nimrtc::ice {

namespace {

// -----------------------------------------------------------------------------
// libjuice → nimrtc::core::log bridge (one-shot init).
// -----------------------------------------------------------------------------

std::atomic<bool> g_log_hooked{false};

void hook_libjuice_logging_once() noexcept {
    if (g_log_hooked.exchange(true)) return;

    juice_set_log_level(JUICE_LOG_LEVEL_WARN);

    juice_set_log_handler([](juice_log_level_t level, const char* message) {
        auto& logger = core::log::Logger::instance();
        switch (level) {
            case JUICE_LOG_LEVEL_ERROR:
            case JUICE_LOG_LEVEL_FATAL:
                logger.error(message);
                break;
            case JUICE_LOG_LEVEL_WARN:
                logger.warn(message);
                break;
            case JUICE_LOG_LEVEL_INFO:
                logger.info(message);
                break;
            case JUICE_LOG_LEVEL_DEBUG:
            case JUICE_LOG_LEVEL_VERBOSE:
                logger.debug(message);
                break;
            default:
                break;
        }
    });
}

// -----------------------------------------------------------------------------
// State / packet handoff (libjuice thread → engine thread).
// -----------------------------------------------------------------------------

struct Packet {
    std::vector<std::uint8_t> data;
    plugins::Addr             src;     // best-effort source (may be empty)
};

IceState map_juice_state(juice_state_t s) noexcept {
    switch (s) {
        case JUICE_STATE_DISCONNECTED: return IceState::Disconnected;
        case JUICE_STATE_GATHERING:    return IceState::Gathering;
        case JUICE_STATE_CONNECTING:   return IceState::Connecting;
        case JUICE_STATE_CONNECTED:    return IceState::Connected;
        case JUICE_STATE_COMPLETED:    return IceState::Completed;
        case JUICE_STATE_FAILED:       return IceState::Failed;
    }
    return IceState::Disconnected;
}

juice_concurrency_mode_t map_mode(IceConfig::Mode m) noexcept {
    switch (m) {
        case IceConfig::Mode::Poll:   return JUICE_CONCURRENCY_MODE_POLL;
        case IceConfig::Mode::Mux:    return JUICE_CONCURRENCY_MODE_MUX;
        case IceConfig::Mode::Thread: return JUICE_CONCURRENCY_MODE_THREAD;
    }
    return JUICE_CONCURRENCY_MODE_MUX;
}

} // anonymous namespace

// =============================================================================
// Candidate helpers
// =============================================================================

std::optional<Candidate> parse_candidate(std::string_view sdp) {
    // Accept "a=candidate:..." or "candidate:..."; drop the optional prefix.
    constexpr std::string_view kPrefixA = "a=candidate:";
    constexpr std::string_view kPrefix  = "candidate:";
    if (sdp.substr(0, kPrefixA.size()) == kPrefixA) {
        sdp.remove_prefix(kPrefixA.size());
    } else if (sdp.substr(0, kPrefix.size()) == kPrefix) {
        sdp.remove_prefix(kPrefix.size());
    } else {
        return std::nullopt;
    }

    Candidate c;
    std::istringstream iss{std::string{sdp}};

    std::string tok;
    unsigned tmp_component_id = 0;  // uint8_t reads as char otherwise
    iss >> c.foundation;        // 1
    iss >> tmp_component_id;    // 2
    c.component_id = static_cast<std::uint8_t>(tmp_component_id);
    iss >> c.transport;         // 3
    iss >> c.priority;          // 4
    iss >> c.host;              // 5
    iss >> c.port;              // 6
    iss >> tok;                 // "typ"
    std::string type_str;
    iss >> type_str;
    if (type_str == "host")  c.type = CandidateType::Host;
    else if (type_str == "srflx") c.type = CandidateType::Srflx;
    else if (type_str == "prflx") c.type = CandidateType::Prflx;
    else if (type_str == "relay") c.type = CandidateType::Relay;
    else return std::nullopt;

    // Optional "raddr ... rport ..."
    if (iss >> tok && tok == "raddr") {
        iss >> c.related_address;
        iss >> tok; // "rport"
        iss >> c.related_port;
    }
    return c;
}

std::string Candidate::to_sdp() const {
    static const char* kTypeNames[] = {"host", "srflx", "prflx", "relay"};
    std::ostringstream oss;
    oss << "candidate:" << foundation
        << ' ' << static_cast<unsigned>(component_id)
        << ' ' << transport
        << ' ' << priority
        << ' ' << host
        << ' ' << port
        << " typ " << kTypeNames[static_cast<int>(type)];
    if (!related_address.empty()) {
        oss << " raddr " << related_address << " rport " << related_port;
    }
    return oss.str();
}

// =============================================================================
// IceTransport::Impl
// =============================================================================

struct IceTransport::Impl {
    IceConfig                            config;
    juice_agent_t*                       agent = nullptr;

    // libjuice callbacks fire on libjuice's thread; protect shared state.
    mutable std::mutex                   mu;
    std::condition_variable              cv_gather;

    // Local candidates collected via cb_candidate (each entry is the
    // libjuice-formatted SDP body, e.g.
    //   "candidate:1 1 UDP 2113929471 192.0.2.1 5000 typ host").
    std::vector<std::string>             local_candidates;
    bool                                gathering_done = false;

    IceState                             state = IceState::Disconnected;
    std::deque<Packet>                   rx_queue;
    plugins::RecvCallback                recv_cb_;     // renamed to avoid collision with static on_recv
    plugins::ErrorCallback               on_error;

    // Latest selected addresses, refreshed on state → Connected/Completed.
    std::string                          selected_local_str;
    std::string                          selected_remote_str;

    // Cached credentials parsed from the remote SDP (for diagnostics).
    std::string                          remote_ufrag_str;
    std::string                          remote_pwd_str;

    // ---- libjuice static callbacks ---------------------------------------

    static void on_state(juice_agent_t*, juice_state_t st, void* user) {
        auto* self = static_cast<Impl*>(user);
        std::lock_guard<std::mutex> lk(self->mu);
        self->state = map_juice_state(st);

        if (st == JUICE_STATE_CONNECTED || st == JUICE_STATE_COMPLETED) {
            char local[JUICE_MAX_ADDRESS_STRING_LEN] = {0};
            char remote[JUICE_MAX_ADDRESS_STRING_LEN] = {0};
            if (juice_get_selected_addresses(self->agent, local, sizeof(local),
                                             remote, sizeof(remote)) == JUICE_ERR_SUCCESS) {
                self->selected_local_str  = local;
                self->selected_remote_str = remote;
            }
        }
    }

    static void on_candidate(juice_agent_t*, const char* sdp, void* user) {
        // New local candidate discovered (host / srflx / relay / prflx).
        // We push it onto the local_candidates list so the engine can
        // include it in SDP.  Mutex held briefly; this fires on libjuice's
        // thread.
        auto* self = static_cast<Impl*>(user);
        if (!sdp) return;
        std::lock_guard<std::mutex> lk(self->mu);
        self->local_candidates.emplace_back(sdp);
    }

    static void on_gathering_done(juice_agent_t*, void* user) {
        auto* self = static_cast<Impl*>(user);
        {
            std::lock_guard<std::mutex> lk(self->mu);
            self->gathering_done = true;
        }
        self->cv_gather.notify_all();
        core::log::Logger::instance().info(
            "nimrtc::ice: local candidate gathering done");
    }

    static void juice_recv_cb(juice_agent_t*, const char* data, std::size_t size, void* user) {
        auto* self = static_cast<Impl*>(user);
        Packet pkt;
        pkt.data.assign(reinterpret_cast<const std::uint8_t*>(data),
                        reinterpret_cast<const std::uint8_t*>(data) + size);
        // src: best effort — in MUX mode the selected remote is the source.
        {
            std::lock_guard<std::mutex> lk(self->mu);
            pkt.src = self->encode_addr(self->selected_remote_str);
            self->rx_queue.push_back(std::move(pkt));
        }
    }

    // ---- helpers ---------------------------------------------------------

    /** Encode a "host:port" string into a plugins::Addr (text-only encoding
     *  internal to IceTransport). */
    plugins::Addr encode_addr(const std::string& s) const noexcept {
        plugins::Addr a{};
        const std::size_t n = std::min(s.size(), sizeof(a.data));
        std::memcpy(a.data, s.data(), n);
        a.len = static_cast<std::uint32_t>(n);
        return a;
    }

    /** Build the juice_config_t from IceConfig and create the agent. */
    bool create_agent() {
        juice_config_t jc{};
        jc.concurrency_mode = map_mode(config.mode);
        jc.stun_server_host = config.stun_server_host.empty()
                                  ? nullptr
                                  : config.stun_server_host.c_str();
        jc.stun_server_port = config.stun_server_port;
        jc.bind_address = config.bind_address.empty()
                              ? nullptr
                              : config.bind_address.c_str();
        jc.local_port_range_begin = config.local_port_range_begin;
        jc.local_port_range_end   = config.local_port_range_end;

        std::vector<juice_turn_server_t> jt;
        jt.reserve(config.turn_servers.size());
        for (const auto& t : config.turn_servers) {
            juice_turn_server_t s{};
            s.host = t.host.c_str();
            s.port = t.port;
            s.username = t.username.empty() ? nullptr : t.username.c_str();
            s.password = t.password.empty() ? nullptr : t.password.c_str();
            jt.push_back(s);
        }
        if (!jt.empty()) {
            jc.turn_servers      = jt.data();
            jc.turn_servers_count = static_cast<int>(jt.size());
        }

        jc.cb_state_changed    = &Impl::on_state;
        jc.cb_candidate        = &Impl::on_candidate;
        jc.cb_gathering_done   = &Impl::on_gathering_done;
        jc.cb_recv             = &Impl::juice_recv_cb;
        jc.user_ptr            = this;

        juice_agent_t* a = juice_create(&jc);
        if (!a) return false;
        agent = a;

        if (!config.local_ufrag.empty() || !config.local_password.empty()) {
            juice_set_local_ice_attributes(agent,
                                           config.local_ufrag.c_str(),
                                           config.local_password.c_str());
        }
        return true;
    }
};

// =============================================================================
// IceTransport — public interface
// =============================================================================

IceTransport::IceTransport(IceConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    hook_libjuice_logging_once();
}

void IceTransport::set_config(const IceConfig& cfg) noexcept {
    if (!impl_->agent) {
        impl_->config = cfg;
    }
}

void IceTransport::set_bind_address(std::string_view addr) noexcept {
    impl_->config.bind_address = std::string(addr);
}

void IceTransport::set_stun_server(std::string_view host, std::uint16_t port) noexcept {
    impl_->config.stun_server_host = std::string(host);
    impl_->config.stun_server_port = port;
}

void IceTransport::set_local_port_range(std::uint16_t begin, std::uint16_t end) noexcept {
    impl_->config.local_port_range_begin = begin;
    impl_->config.local_port_range_end   = end;
}

IceTransport::~IceTransport() {
    close();
}

IceTransport::IceTransport(IceTransport&& other) noexcept = default;
IceTransport& IceTransport::operator=(IceTransport&& other) noexcept = default;

const char* IceTransport::name() const noexcept {
    return "nimrtc::ice::IceTransport (libjuice)";
}

plugins::Status IceTransport::open() noexcept {
    if (impl_->agent) {
        return plugins::kOk;   // already open
    }
    if (!impl_->create_agent()) {
        return plugins::kErrInternal;
    }
    if (juice_gather_candidates(impl_->agent) != JUICE_ERR_SUCCESS) {
        return plugins::kErrInternal;
    }
    return plugins::kOk;
}

void IceTransport::close() noexcept {
    if (impl_->agent) {
        juice_destroy(impl_->agent);
        impl_->agent = nullptr;
    }
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->rx_queue.clear();
    impl_->state = IceState::Disconnected;
}

void IceTransport::set_callbacks(plugins::RecvCallback on_recv,
                                 plugins::ErrorCallback on_error) noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->recv_cb_ = std::move(on_recv);
    impl_->on_error = std::move(on_error);
}

plugins::Status IceTransport::send(plugins::BufferView view,
                                    plugins::Addr /*dst_addr*/) noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;
    if (view.empty())   return plugins::kOk;

    const int rc = juice_send(impl_->agent,
                              reinterpret_cast<const char*>(view.data()),
                              view.size());
    return rc == JUICE_ERR_SUCCESS ? plugins::kOk : plugins::kErrInternal;
}

int IceTransport::recv() noexcept {
    plugins::RecvCallback cb;
    std::deque<Packet>    drained;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->rx_queue.empty()) return 0;
        cb = impl_->recv_cb_;
        drained.swap(impl_->rx_queue);
    }

    int dispatched = 0;
    if (cb) {
        for (auto& pkt : drained) {
            plugins::BufferView bv{pkt.data.data(), pkt.data.size()};
            cb(bv);
            ++dispatched;
        }
    }
    return dispatched;
}

plugins::Addr IceTransport::local_addr() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->encode_addr(impl_->selected_local_str);
}

plugins::Addr IceTransport::remote_addr() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->encode_addr(impl_->selected_remote_str);
}

IceState IceTransport::state() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->state;
}

Role IceTransport::role() const noexcept {
    return impl_->config.role;
}

std::string IceTransport::local_description() const {
    if (!impl_->agent) return {};
    char buf[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (juice_get_local_description(impl_->agent, buf, sizeof(buf))
            != JUICE_ERR_SUCCESS) {
        return {};
    }
    return std::string{buf};
}

plugins::Status IceTransport::set_remote_description(std::string_view sdp) noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;
    // juice_set_remote_description expects a NUL-terminated C string.
    std::string tmp{sdp};
    const int rc = juice_set_remote_description(impl_->agent, tmp.c_str());
    // Cache the parsed credentials so callers can read them back via
    // remote_ufrag() / remote_password() without going through libjuice.
    auto ufrag_pos = tmp.find("a=ice-ufrag:");
    auto pwd_pos   = tmp.find("a=ice-pwd:");
    if (ufrag_pos != std::string::npos) {
        ufrag_pos += std::strlen("a=ice-ufrag:");
        auto eol = tmp.find('\n', ufrag_pos);
        impl_->remote_ufrag_str = tmp.substr(
            ufrag_pos,
            eol == std::string::npos ? std::string::npos : eol - ufrag_pos);
        while (!impl_->remote_ufrag_str.empty() &&
               (impl_->remote_ufrag_str.back() == '\r' ||
                impl_->remote_ufrag_str.back() == '\n')) {
            impl_->remote_ufrag_str.pop_back();
        }
    }
    if (pwd_pos != std::string::npos) {
        pwd_pos += std::strlen("a=ice-pwd:");
        auto eol = tmp.find('\n', pwd_pos);
        impl_->remote_pwd_str = tmp.substr(
            pwd_pos,
            eol == std::string::npos ? std::string::npos : eol - pwd_pos);
        while (!impl_->remote_pwd_str.empty() &&
               (impl_->remote_pwd_str.back() == '\r' ||
                impl_->remote_pwd_str.back() == '\n')) {
            impl_->remote_pwd_str.pop_back();
        }
    }
    return rc == JUICE_ERR_SUCCESS ? plugins::kOk : plugins::kErrInvalidParam;
}

plugins::Status IceTransport::add_remote_candidate(std::string_view sdp) noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;
    std::string tmp{sdp};
    const int rc = juice_add_remote_candidate(impl_->agent, tmp.c_str());
    return rc == JUICE_ERR_SUCCESS ? plugins::kOk : plugins::kErrInvalidParam;
}

std::string IceTransport::local_ufrag() const noexcept {
    if (!impl_->agent) return impl_->config.local_ufrag;
    char buf[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (juice_get_local_description(impl_->agent, buf, sizeof(buf))
            != JUICE_ERR_SUCCESS) {
        return impl_->config.local_ufrag;
    }
    std::string sdp{buf};
    auto pos = sdp.find("a=ice-ufrag:");
    if (pos == std::string::npos) return impl_->config.local_ufrag;
    pos += std::strlen("a=ice-ufrag:");
    // libjuice uses \r\n line endings — strip the trailing \r.
    auto end = sdp.find('\n', pos);
    auto end_pos = end == std::string::npos ? std::string::npos : end - pos;
    std::string ufrag = sdp.substr(pos, end_pos);
    while (!ufrag.empty() && (ufrag.back() == '\r' || ufrag.back() == '\n')) {
        ufrag.pop_back();
    }
    return ufrag;
}

std::string IceTransport::local_password() const noexcept {
    if (!impl_->agent) return impl_->config.local_password;
    char buf[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (juice_get_local_description(impl_->agent, buf, sizeof(buf))
            != JUICE_ERR_SUCCESS) {
        return impl_->config.local_password;
    }
    std::string sdp{buf};
    auto pos = sdp.find("a=ice-pwd:");
    if (pos == std::string::npos) return impl_->config.local_password;
    pos += std::strlen("a=ice-pwd:");
    auto end = sdp.find('\n', pos);
    auto end_pos = end == std::string::npos ? std::string::npos : end - pos;
    std::string pwd = sdp.substr(pos, end_pos);
    while (!pwd.empty() && (pwd.back() == '\r' || pwd.back() == '\n')) {
        pwd.pop_back();
    }
    return pwd;
}

std::string IceTransport::remote_ufrag() const noexcept {
    return impl_->remote_ufrag_str;
}

std::string IceTransport::remote_password() const noexcept {
    return impl_->remote_pwd_str;
}

std::optional<Candidate> IceTransport::selected_local() const noexcept {
    if (!impl_->agent) return std::nullopt;
    char buf[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {0};
    if (juice_get_selected_candidates(impl_->agent, buf, sizeof(buf),
                                      nullptr, 0) != JUICE_ERR_SUCCESS) {
        return std::nullopt;
    }
    return parse_candidate(buf);
}

std::optional<Candidate> IceTransport::selected_remote() const noexcept {
    if (!impl_->agent) return std::nullopt;
    char buf[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {0};
    if (juice_get_selected_candidates(impl_->agent, nullptr, 0,
                                      buf, sizeof(buf)) != JUICE_ERR_SUCCESS) {
        return std::nullopt;
    }
    return parse_candidate(buf);
}

plugins::Status IceTransport::gather_candidates() noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;
    return juice_gather_candidates(impl_->agent) == JUICE_ERR_SUCCESS
               ? plugins::kOk
               : plugins::kErrInternal;
}

std::vector<std::string>
IceTransport::gathered_local_candidates() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->local_candidates;
}

bool IceTransport::wait_for_gathering(std::int64_t timeout_ms) noexcept {
    if (!impl_->agent) return false;
    std::unique_lock<std::mutex> lk(impl_->mu);
    // Treat Connected/Completed state as gathering-is-sufficient too.
    auto already = [&] {
        return impl_->gathering_done ||
               impl_->state == IceState::Connected ||
               impl_->state == IceState::Completed;
    };
    if (already()) return true;

    if (timeout_ms <= 0) return already();

    return impl_->cv_gather.wait_for(lk, std::chrono::milliseconds(timeout_ms), already);
}

// =============================================================================
// IceTransportFactory
// =============================================================================

IceTransportFactory::IceTransportFactory() = default;
IceTransportFactory::IceTransportFactory(IceConfig default_config)
    : default_config_(std::move(default_config)) {}

std::string_view IceTransportFactory::id() const noexcept {
    return "ice";
}

std::string_view IceTransportFactory::display_name() const noexcept {
    return "ICE (libjuice, RFC 8445)";
}

plugins::ITransport* IceTransportFactory::create() const {
    return new IceTransport(default_config_);
}

// ---------------------------------------------------------------------------
// Public registration entry point — replaces the anonymous-namespace static
// registrar that MSVC strips from static libraries (same workaround as
// nimrtc::audio3a::register_default_plugins()).
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    // static ⇒ address stable for process lifetime; runs once at first call.
    static const struct Registrar {
        Registrar() {
            static nimrtc::ice::IceTransportFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_transport(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in ice.hpp) so the symbol is guaranteed
// in nimrtc_ice.lib for consumers that link via static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::ice
