/**
 * @file src/modules/sdp/src/sdp_plugin.cpp
 * @brief PluginAdapter + PluginFactory implementation for nimrtc::sdp.
 *
 * Bridges concrete sdp::SessionDescription (owned strings) ↔ plugins::SdpSession
 * (string_view-based generic model). Conversion is lossy in the direction
 * concrete → plugin (some named fields collapse into the generic attr list),
 * but round-trippable for the WebRTC subset the engine needs.
 *
 * Per ADR-001 + MSVC static-link workaround.
 */

#include <nimrtc/sdp/sdp_plugin.hpp>

#include <cstdint>
#include <cstring>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::sdp {

// ---------------------------------------------------------------------------
// Type conversion helpers
// ---------------------------------------------------------------------------

namespace {

/** Translate plugins::SdpMedia → sdp::MediaDescription (best-effort reverse). */
sdp::MediaDescription media_to_concrete(const plugins::SdpMedia& m) {
    sdp::MediaDescription out;
    if      (m.media == "audio")       out.type = sdp::MediaType::Audio;
    else if (m.media == "video")       out.type = sdp::MediaType::Video;
    else if (m.media == "application") out.type = sdp::MediaType::Application;
    else if (m.media == "data")        out.type = sdp::MediaType::Data;
    else                               out.type = sdp::MediaType::Other;
    out.port     = m.port;
    out.protocol = std::string(m.proto);
    for (auto f : m.formats) {
        out.formats.emplace_back(std::to_string(f));
    }
    for (auto& [k, v] : m.attrs) {
        // Translate well-known attrs back to concrete fields.
        if      (k == "mid")               out.mid = std::string(v);
        else if (k == "ice-ufrag")         out.ice_ufrag = std::string(v);
        else if (k == "ice-pwd")           out.ice_pwd = std::string(v);
        else if (k == "ice-options")       out.ice_options = std::string(v);
        else if (k == "rtcp-mux")          out.rtcp_mux_value = std::string(v);
        else if (k == "setup")             out.dtls_setup = std::string(v);
        else if (k == "fingerprint") {
            // "sha-256 BASE64" — split on first space
            auto sp = v.find(' ');
            if (sp != std::string_view::npos) {
                out.dtls_fingerprint_algo  = std::string(v.substr(0, sp));
                out.dtls_fingerprint_value = std::string(v.substr(sp + 1));
            }
        }
        else if (k == "sendrecv") out.direction = sdp::Direction::SendRecv;
        else if (k == "sendonly") out.direction = sdp::Direction::SendOnly;
        else if (k == "recvonly") out.direction = sdp::Direction::RecvOnly;
        else if (k == "inactive") out.direction = sdp::Direction::Inactive;
        else if (k == "candidate") {
            out.candidates.emplace_back(v);
        }
        else {
            out.extra_attrs.emplace_back(std::string(k), std::string(v));
        }
    }
    return out;
}

/** Translate sdp::MediaDescription → plugins::SdpMedia (flatten into attrs). */
plugins::SdpMedia concrete_to_media(const sdp::MediaDescription& m,
                                    std::vector<std::string>& attr_storage) {
    plugins::SdpMedia out;
    out.media = (m.type == sdp::MediaType::Audio)       ? "audio"
              : (m.type == sdp::MediaType::Video)       ? "video"
              : (m.type == sdp::MediaType::Application) ? "application"
              : (m.type == sdp::MediaType::Data)        ? "data"
              : "text";
    out.port  = m.port;
    out.proto = m.protocol;
    for (auto& f : m.formats) {
        try {
            out.formats.push_back(static_cast<std::uint8_t>(std::stoi(f)));
        } catch (...) {
            // Range strings ("96-127") → store as 0 placeholder.
            // Plugin interface can't model ranges; caller should use mung().
            out.formats.push_back(0);
        }
    }
    // Populate attrs in RFC 8829 §5 order so consumers see the same sequence
    // as raw SDP.
    if (!m.ice_ufrag.empty()) {
        attr_storage.emplace_back("ice-ufrag");
        attr_storage.emplace_back(m.ice_ufrag);
        out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                               attr_storage[attr_storage.size()-1]);
    }
    if (!m.ice_pwd.empty()) {
        attr_storage.emplace_back("ice-pwd");
        attr_storage.emplace_back(m.ice_pwd);
        out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                               attr_storage[attr_storage.size()-1]);
    }
    if (!m.rtcp_mux_value.empty()) {
        attr_storage.emplace_back("rtcp-mux");
        attr_storage.emplace_back("");
        out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                               attr_storage[attr_storage.size()-1]);
    }
    if (!m.mid.empty()) {
        attr_storage.emplace_back("mid");
        attr_storage.emplace_back(m.mid);
        out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                               attr_storage[attr_storage.size()-1]);
    }
    if (!m.dtls_fingerprint_algo.empty() && !m.dtls_fingerprint_value.empty()) {
        std::string fp_value = m.dtls_fingerprint_algo + " " + m.dtls_fingerprint_value;
        attr_storage.emplace_back("fingerprint");
        attr_storage.emplace_back(std::move(fp_value));
        out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                               attr_storage[attr_storage.size()-1]);
    }
    if (!m.dtls_setup.empty()) {
        attr_storage.emplace_back("setup");
        attr_storage.emplace_back(m.dtls_setup);
        out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                               attr_storage[attr_storage.size()-1]);
    }
    // Direction
    const char* dir_str =
        (m.direction == sdp::Direction::SendRecv) ? "sendrecv" :
        (m.direction == sdp::Direction::SendOnly) ? "sendonly" :
        (m.direction == sdp::Direction::RecvOnly) ? "recvonly" : "inactive";
    attr_storage.emplace_back(dir_str);
    attr_storage.emplace_back("");
    out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                           attr_storage[attr_storage.size()-1]);
    // ICE candidates
    for (auto& c : m.candidates) {
        attr_storage.emplace_back("candidate");
        attr_storage.emplace_back(c);
        out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                               attr_storage[attr_storage.size()-1]);
    }
    // Extra attrs verbatim
    for (auto& [k, v] : m.extra_attrs) {
        attr_storage.emplace_back(k);
        attr_storage.emplace_back(v);
        out.attrs.emplace_back(attr_storage[attr_storage.size()-2],
                               attr_storage[attr_storage.size()-1]);
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

PluginAdapter::PluginAdapter()
    : parser_(std::make_unique<sdp::Parser>())
    , munger_(std::make_unique<sdp::Munger>()) {
    core::log::Logger::instance().debug("sdp::PluginAdapter created");
}

PluginAdapter::~PluginAdapter() = default;

const char* PluginAdapter::name() const noexcept {
    return "nimrtc::sdp::PluginAdapter (RFC 4566 + RFC 8829)";
}

plugins::Status PluginAdapter::open() noexcept { return plugins::kOk; }
void PluginAdapter::close() noexcept { /* stateless */ }

std::optional<plugins::SdpSession>
PluginAdapter::parse(std::string_view sdp_text,
                     plugins::SdpParseErrorCallback on_error) const noexcept {
    auto parsed = parser_->parse(sdp_text);
    if (!parsed) {
        if (on_error) on_error(plugins::kErrCorrupt, "SDP parse failed");
        return std::nullopt;
    }
    last_parsed_ = std::move(parsed.value());

    // Build cached view strings.
    cached_origin_ = last_parsed_.origin_username + " " +
                     last_parsed_.origin_session_id + " " +
                     last_parsed_.origin_session_version + " IN IP4 " +
                     last_parsed_.origin_address;
    cached_session_name_ = last_parsed_.session_name;
    cached_attrs_kv_.clear();
    cached_medias_.clear();
    cached_medias_.reserve(last_parsed_.media.size());
    for (auto& m : last_parsed_.media) {
        cached_medias_.push_back(concrete_to_media(m, cached_attrs_kv_));
    }

    cached_view_ = plugins::SdpSession{};
    cached_view_.version      = "0";
    cached_view_.origin       = cached_origin_;
    cached_view_.session_name = cached_session_name_;
    cached_view_.session_info = last_parsed_.session_info;
    cached_view_.uri          = last_parsed_.uri;
    cached_view_.medias       = cached_medias_;
    // Surface session-level a=ice-ufrag / a=ice-pwd / a=ice-options / etc. so
    // BUNDLE-style SDPs (where ICE credentials live at the session level, not
    // per-media) remain readable.  The Parser already populates
    // last_parsed_.extra_attrs for these — we just need to keep the views alive.
    cached_session_attrs_.clear();
    cached_session_attrs_.reserve(last_parsed_.extra_attrs.size());
    for (const auto& kv : last_parsed_.extra_attrs) {
        cached_attrs_kv_.emplace_back(kv.first);
        cached_attrs_kv_.emplace_back(kv.second);
        cached_session_attrs_.emplace_back(
            cached_attrs_kv_[cached_attrs_kv_.size() - 2],
            cached_attrs_kv_[cached_attrs_kv_.size() - 1]);
    }
    cached_view_.session_attrs = cached_session_attrs_;
    return cached_view_;
}

std::size_t
PluginAdapter::serialize(const plugins::SdpSession& session,
                         char* out, std::size_t len_hint) const noexcept {
    // Build a minimal concrete SessionDescription from the plugin session.
    sdp::SessionDescription sd;
    sd.version = 0;
    sd.session_name = std::string(session.session_name);
    sd.session_info = std::string(session.session_info);
    sd.uri = std::string(session.uri);
    sd.media.reserve(session.medias.size());
    for (auto& m : session.medias) {
        sd.media.push_back(media_to_concrete(m));
    }
    auto result = munger_->to_sdp(sd);
    if (!result) return 0;
    auto& text = result.value();
    if (text.size() + 1 > len_hint) return 0;
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return text.size();
}

std::string
PluginAdapter::mung(const plugins::SdpSession& /*session*/,
                    const plugins::MungOptions& /*opts*/,
                    plugins::SdpParseErrorCallback on_error) const noexcept {
    if (on_error) on_error(plugins::kErrUnsupported,
                           "mung() not yet implemented in adapter");
    return {};
}

bool PluginAdapter::is_compatible(const plugins::SdpSession& local,
                                  const plugins::SdpSession& remote) const noexcept {
    // Simple intersection test: at least one common payload type per direction
    // and matching proto.
    if (local.medias.empty() || remote.medias.empty()) return false;
    for (auto& lm : local.medias) {
        for (auto& rm : remote.medias) {
            if (lm.media != rm.media) continue;
            if (lm.proto != rm.proto) continue;
            for (auto f : lm.formats) {
                for (auto rf : rm.formats) {
                    if (f != 0 && f == rf) return true;
                }
            }
        }
    }
    return false;
}

std::optional<std::string_view>
PluginAdapter::pick_codec(const plugins::SdpMedia& local,
                          const plugins::SdpMedia& remote) const noexcept {
    // Best-effort: look for matching payload-type attr key "rtpmap:<fmt>".
    // Returns the encoding name of the first common fmt.
    for (auto& [lk, lv] : local.attrs) {
        if (!lk.starts_with("rtpmap:")) continue;
        auto fmt_str = std::string(lk.substr(7));
        // Compare against remote
        for (auto& [rk, rv] : remote.attrs) {
            if (rk != lk) continue;
            // Found match: encoding is "<encoding>/<clock>"
            auto slash = lv.find('/');
            if (slash != std::string_view::npos) {
                return lv.substr(0, slash);
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// PluginFactory
// ---------------------------------------------------------------------------

std::string_view PluginFactory::id() const noexcept {
    return "webrtc";   // matches EngineConfig::sdp_name default
}

std::string_view PluginFactory::display_name() const noexcept {
    return "SDP — RFC 4566 + RFC 8829 WebRTC extensions";
}

plugins::ISDP* PluginFactory::create() const {
    return new PluginAdapter();
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static nimrtc::sdp::PluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_sdp(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in sdp_plugin.hpp) so the symbol is guaranteed
// in nimrtc_sdp.lib for consumers that link via static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::sdp
