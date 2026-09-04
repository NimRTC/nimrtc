/**
 * @file nimrtc/sdp/sdp_plugin.hpp
 * @brief Plugin adapter: wraps nimrtc::sdp::Parser/Munger behind plugins::ISDP.
 *
 * Implements the plugin interface defined in <nimrtc/plugins/sdp.hpp> by
 * delegating to the concrete SDP module (sdp::Parser + sdp::Munger).
 *
 * ## Type conversion
 *
 * - `plugins::SdpSession` ← `sdp::SessionDescription` (stringified view)
 * - `plugins::SdpMedia`   ← `sdp::MediaDescription` (flatten named fields
 *   into the generic `(key, value)` attribute list)
 *
 * ## Lifetime contract
 *
 * The adapter owns the last parsed SessionDescription. The returned
 * SdpSession's `string_view` fields point into either:
 *   (a) the input buffer passed to parse() — caller must keep it alive, OR
 *   (b) internal adapter-owned strings (e.g. origin) — caller must not
 *       destroy the adapter.
 *
 * For practical use, treat SdpSession as short-lived (one engine tick).
 *
 * ## Registration
 *
 * Registered as id "webrtc" (matches EngineConfig::sdp_name default).
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/sdp.hpp>             // plugins::ISDP / SdpSession / SdpMedia
#include <nimrtc/sdp/session_description.hpp>   // concrete sdp::SessionDescription / MediaDescription

namespace nimrtc::sdp {

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

/**
 * @brief Wraps concrete sdp::Parser + sdp::Munger behind plugins::ISDP.
 *
 * Holds the last-parsed SessionDescription in storage; the returned
 * SdpSession's string_views point into adapter-owned memory + the input
 * buffer. Call parse() / serialize() per engine tick — old views become
 * invalid once a new parse() runs.
 */
class PluginAdapter : public plugins::ISDP {
public:
    PluginAdapter();
    ~PluginAdapter() override;

    PluginAdapter(const PluginAdapter&)            = delete;
    PluginAdapter& operator=(const PluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void          close() noexcept override;

    // ---- plugins::ISDP ----------------------------------------------------

    std::optional<plugins::SdpSession>
    parse(std::string_view sdp_text,
          plugins::SdpParseErrorCallback on_error) const noexcept override;

    std::size_t
    serialize(const plugins::SdpSession& session,
              char* out,
              std::size_t len_hint) const noexcept override;

    std::string
    mung(const plugins::SdpSession& session,
         const plugins::MungOptions& opts,
         plugins::SdpParseErrorCallback on_error) const noexcept override;

    bool
    is_compatible(const plugins::SdpSession& local,
                  const plugins::SdpSession& remote) const noexcept override;

    std::optional<std::string_view>
    pick_codec(const plugins::SdpMedia& local,
               const plugins::SdpMedia& remote) const noexcept override;

private:
    /** Owned concrete parser + munger. */
    std::unique_ptr<sdp::Parser> parser_;
    std::unique_ptr<sdp::Munger> munger_;

    /** Cached last-parsed SessionDescription (owned by adapter). */
    mutable sdp::SessionDescription last_parsed_;

    /** Cached reconstructed SdpSession matching last_parsed_; views point into
     *  these strings. Mutated only inside parse(). */
    mutable std::string cached_origin_;
    mutable std::string cached_session_name_;
    mutable std::vector<std::string> cached_attrs_kv_;   // serialized "k:v"
    mutable std::vector<plugins::SdpMedia> cached_medias_;
    mutable plugins::SdpSession cached_view_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing PluginAdapter instances.
 *
 * Registered as id "webrtc" (matches EngineConfig::sdp_name default).
 */
class PluginFactory : public plugins::ISDPFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::ISDP*   create()      const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point (MSVC static-link workaround)
// ---------------------------------------------------------------------------

namespace detail {
/** Defined in sdp_plugin.cpp. Forces .obj linkage on consumer call. */
void do_register_default_plugins() noexcept;
} // namespace detail

inline void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::sdp
