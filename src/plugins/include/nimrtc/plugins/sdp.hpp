/**
 * @file nimrtc/plugins/sdp.hpp
 * @brief ISDP — pluggable SDP parsing and munging interface.
 *
 * Replace this to support proprietary SDP extensions (e.g. TURN+ICE
 * credentials, codec-specific attributes) without touching the engine.
 *
 * ## Implementing a custom SDP plugin
 *
 * 1. Implement `ISDP` for your SDP variant.
 * 2. Register: `PluginRegistry::instance().register_sdp("my_sdp", factory);`
 * 3. Set `NimRTCEngine::Config::sdp_name = "my_sdp"`.
 *
 * @note P0 scaffold — interface stable, binary layout TBD P1.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_SDP_HPP
#define NIMRTC_PLUGINS_SDP_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// SDP model (codec-agnostic)
// ---------------------------------------------------------------------------

/** Media description (one "m=" line and its attributes). */
struct SdpMedia {
    std::string_view media;          // "audio", "video", "application", …
    uint16_t port = 0;
    std::string_view proto;          // "RTP/AVP", "RTP/SAVPF", …
    std::vector<uint8_t> formats;   // payload type numbers

    std::vector<std::pair<std::string_view, std::string_view>> attrs;

    /** Get first attribute matching the given key. */
    std::optional<std::string_view>
    attr(std::string_view key) const noexcept {
        for (auto& [k, v] : attrs) {
            if (k == key) return v;
        }
        return std::nullopt;
    }
};

/** Session-level description. */
struct SdpSession {
    std::string_view version;         // "v=0"
    std::string_view origin;          // "o=- …"
    std::string_view session_name;    // "s=-"
    std::string_view session_info;    // "i=…" (may be empty)
    std::string_view uri;              // "u=…" (may be empty)

    std::vector<std::pair<std::string_view, std::string_view>> session_attrs;

    std::vector<SdpMedia> medias;

    std::optional<std::string_view>
    attr(std::string_view key) const noexcept {
        for (auto& [k, v] : session_attrs) {
            if (k == key) return v;
        }
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// Munging operations
// ---------------------------------------------------------------------------

/** Per-codec constraints for munging. */
struct CodecConfig {
    std::string_view codec_name;    // "opus", "vp8", "h264", …
    int ptime_ms = 0;               // 0 = keep default
    int max_ptime_ms = 0;
    std::vector<int> supported_bitrate;  // b=AS value in kbps, empty = no limit
    bool disable_nack = false;
    bool disable_tmmbr = false;
    bool enable_red = false;
};

/** Munging options for rewriting an SDP offer/answer. */
struct MungOptions {
    /** Replace connection address (c=) with this host. */
    std::string_view rewrite_c_addr;

    /** Replace media port with this. Zero = reject the media. */
    uint16_t rewrite_port = 0;

    /** Restrict codecs to this list (empty = all allowed). */
    std::vector<std::string_view> allowed_codecs;

    /** Per-media-type codec overrides. */
    std::vector<CodecConfig> codec_configs;

    /** Add extra session-level attribute "a=key:value". */
    std::vector<std::pair<std::string_view, std::string_view>> extra_attrs;

    /** ICE role override: "active", "passive", "actpass", or empty = keep. */
    std::string_view ice_role;

    /** Remove non-matching SSRC group fingerprints. */
    bool strip_ssrc_groups = false;
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

using SdpParseErrorCallback = std::function<void(Status, std::string_view)>;

// ---------------------------------------------------------------------------
// ISDP
// ---------------------------------------------------------------------------

class ISDP : public IPlugin {
public:
    /** Parse an SDP string into an SdpSession.
     *  @return parsed session, or nullopt on error (error callback fired). */
    virtual std::optional<SdpSession>
    parse(std::string_view sdp_text,
          SdpParseErrorCallback on_error) const noexcept = 0;

    /** Serialize an SdpSession back to SDP text.
     *  @param[out] out  Buffer to write into (size = len_hint).
     *  @return bytes written, or 0 on error. */
    virtual size_t
    serialize(const SdpSession& session,
              char* out,
              size_t len_hint) const noexcept = 0;

    /** Rewrite an SDP offer/answer per MungOptions.
     *  @return rewritten SDP text (caller owns the string). */
    virtual std::string
    mung(const SdpSession& session,
         const MungOptions& opts,
         SdpParseErrorCallback on_error) const noexcept = 0;

    /** Check if a remote SDP is compatible with our local capabilities.
     *  @return true if at least one media stream can be established. */
    virtual bool
    is_compatible(const SdpSession& local,
                  const SdpSession& remote) const noexcept = 0;

    /** Pick the best compatible codec for a media line.
     *  @return codec name, or nullopt. */
    virtual std::optional<std::string_view>
    pick_codec(const SdpMedia& local,
               const SdpMedia& remote) const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class ISDPFactory {
public:
    virtual ~ISDPFactory() = default;
    virtual std::string_view id()          const noexcept = 0;
    virtual std::string_view display_name()const noexcept = 0;
    virtual ISDP* create()                 const = 0;
};

template<class T>
class SimpleSDPFactory : public ISDPFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleSDPFactory(std::string_view id, std::string_view name) noexcept
        : id_(id), name_(name) {}
    virtual std::string_view id()          const noexcept override { return id_; }
    virtual std::string_view display_name()const noexcept override { return name_; }
    virtual ISDP* create()                 const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_SDP_HPP
