/**
 * @file src/engine/src/engine_rtcp_helpers.hpp
 * @brief PRIVATE engine helpers: `a=rtcp-fb` and `a=rtcp-xr` parsing.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): these helpers
 * used to live in `engine_impl.hpp`, which made the engine's internal
 * header a 1100-line catch-all.  They are grouped here by *concern* so
 * that `engine_impl.hpp` can shrink to the state it actually owns.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What belongs here
 *
 *   - the `a=rtcp-fb` grammar (RFC 4585 §4.1) — the feedback types the
 *     engine supports and the codec-control-message parameters
 *     (RFC 5104 §7.1);
 *   - the `a=rtcp-xr` grammar (RFC 3611 §5.1) — the extended-report
 *     formats the engine produces and the stat-summary field list.
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::engine::detail {

/** True when an `a=rtcp-fb` entry mentions the given feedback type.
 *
 *  The entry is the SDP text that follows "rtcp-fb:", i.e.
 *  "<pt> <type> [<type>…]" (RFC 4585 §4.1).  The payload type is
 *  scoped to its m-section, and browsers disagree about what they put
 *  there — Chrome uses the *media* payload type, not 205 — so the
 *  feedback *type* token is what identifies the format. */
inline bool rtcp_fb_has_type(std::string_view entry,
                             std::string_view type) noexcept {
    // Skip the payload-type token.
    std::size_t i = 0;
    while (i < entry.size() && entry[i] != ' ') ++i;
    while (i < entry.size()) {
        while (i < entry.size() && entry[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < entry.size() && entry[i] != ' ') ++i;
        if (entry.compare(start, i - start, type) == 0) return true;
    }
    return false;
}

/** The feedback types of an `a=rtcp-fb` entry — everything after the
 *  payload-type token. */
inline std::string_view rtcp_fb_types(std::string_view entry) noexcept {
    const std::size_t sp = entry.find(' ');
    if (sp == std::string_view::npos) return {};
    std::size_t i = sp + 1;
    while (i < entry.size() && entry[i] == ' ') ++i;
    return entry.substr(i);
}

/** True when every feedback type of an `a=rtcp-fb` entry is one the
 *  engine supports.
 *
 *  RFC 3264 §6.1: an answer may only carry the feedback types the
 *  answerer supports, so the answer echoes the offer's set intersected
 *  with the supported one.  An entry that mixes a supported type with
 *  an unsupported one ("111 nack trr-int") is dropped as a whole — the
 *  engine cannot honour half of it.
 *
 *  "tmmbr" is in the supported set (v1.2.0, session plan X5): the
 *  RFC 5104 §7.2 codec-control-message form is
 *  `a=rtcp-fb:<pt> ccm tmmbr`, i.e. the generic "ccm" type with the
 *  "tmmbr" parameter — both tokens are part of one entry and both are
 *  understood by the engine. */
inline bool rtcp_fb_entry_supported(std::string_view entry) noexcept {
    std::size_t i = entry.find(' ');
    if (i == std::string_view::npos) return false;
    ++i;
    bool any = false;
    while (i < entry.size()) {
        while (i < entry.size() && entry[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < entry.size() && entry[i] != ' ') ++i;
        const std::string_view tok = entry.substr(start, i - start);
        if (tok.empty()) continue;
        if (tok != "nack" && tok != "pli" && tok != "ccm" && tok != "fir" &&
            tok != "goog-remb" && tok != "transport-cc" && tok != "tmmbr") {
            return false;
        }
        any = true;
    }
    return any;
}

/** The whitespace-separated feedback-type tokens of an `a=rtcp-fb`
 *  entry — everything after the payload-type token.
 *
 *  RFC 4585 §4.1: the entry is "<pt> <type> [<type>…]"; RFC 5104 §7.1
 *  extends the type list with the codec-control-message family, whose
 *  *parameters* ("fir", "tmmbr", …) follow the "ccm" type token. */
inline std::vector<std::string_view> rtcp_fb_type_tokens(std::string_view entry) noexcept {
    std::vector<std::string_view> out;
    const std::size_t sp = entry.find(' ');
    if (sp == std::string_view::npos) return out;
    std::size_t i = sp + 1;
    while (i < entry.size()) {
        while (i < entry.size() && entry[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < entry.size() && entry[i] != ' ') ++i;
        if (i > start) out.push_back(entry.substr(start, i - start));
    }
    return out;
}

/** True when an `a=rtcp-fb` entry carries the given codec-control-message
 *  parameter (RFC 5104 §7.1).
 *
 *  The parameter must follow the "ccm" type token — a bare "tmmbr"
 *  without "ccm" is not the codec-control form. */
inline bool rtcp_fb_has_ccm_param(std::string_view entry,
                                   std::string_view param) noexcept {
    const auto tokens = rtcp_fb_type_tokens(entry);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] != "ccm") continue;
        // The parameters follow "ccm".  RFC 5104 §7.1 allows a
        // sub-parameter ("smaxpr=…") to follow the parameter it belongs
        // to, so the scan continues until the next non-sub-parameter.
        for (std::size_t k = i + 1; k < tokens.size(); ++k) {
            if (tokens[k] == param) return true;
            if (tokens[k].find('=') == std::string_view::npos) break;
        }
        return false;
    }
    return false;
}

/** Filter one `a=rtcp-fb` entry down to the feedback types the engine
 *  supports and return them as an SDP-ready type list.
 *
 *  RFC 3264 §6.1: an answer carries the subset of the offer's feedback
 *  types that the answerer supports.  RFC 5104 §7.2 makes the removal
 *  granularity explicit for the codec-control-message (CCM) family: the
 *  answerer removes the *CCM parameters* it does not support — not the
 *  whole entry — so "111 ccm fir tmmbr" becomes "111 ccm fir" when the
 *  TMMBR is off.  An entry whose every parameter was removed is dropped
 *  as a whole.
 *
 *  The other feedback families keep the pre-v1.2.0 rule: an entry that
 *  mixes a supported type with an unsupported one is dropped whole,
 *  because the engine cannot honour half of it.
 *
 *  @param entry        The offer's entry ("<pt> <type>…").
 *  @param want_tmmbr   True when the engine supports TMMBR (RFC 5104).
 *  @return The filtered type list; empty when nothing survived. */
inline std::string filter_rtcp_fb_types(std::string_view entry,
                                        bool want_tmmbr) {
    const auto tokens = rtcp_fb_type_tokens(entry);

    // ---- The CCM family (RFC 5104 §7.1) --------------------------------
    if (!tokens.empty() && tokens[0] == "ccm") {
        std::string out = "ccm";
        bool have_tmmbr = false;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::string_view tok = tokens[i];
            if (tok == "fir") {
                out += " fir";
            } else if (tok == "tmmbr") {
                if (!want_tmmbr) continue;
                out += " tmmbr";
                have_tmmbr = true;
            } else if (tok.starts_with("smaxpr=")) {
                // The optional sub-parameter of "tmmbr" (RFC 5104
                // §7.1).  It only makes sense together with it.
                if (!have_tmmbr) continue;
                out += ' ';
                out += tok;
            }
            // "tstr", "vbcm" and anything unknown are removed — the
            // engine implements neither.
        }
        if (out == "ccm") return {};   // no parameter survived
        return out;
    }

    // ---- The other families (RFC 4585 §4.1) ----------------------------
    if (!rtcp_fb_entry_supported(entry)) return {};
    const std::string_view types = rtcp_fb_types(entry);
    if (types.empty()) return {};
    return std::string(types);
}

/** True when the `a=rtcp-xr` parameter list names the given XR format.
 *
 *  RFC 3611 §5.1: the attribute's value is a space-separated list of XR
 *  formats — "rcvr-rtt=all stat-summary=loss,jitt".  A format is named
 *  by its leading token, optionally followed by "=" and the format's own
 *  parameters, so a format is present when one of the tokens starts with
 *  "<name>=" or *is* "<name>". */
inline bool rtcp_xr_has_format(std::string_view params,
                                std::string_view name) noexcept {
    std::size_t i = 0;
    while (i < params.size()) {
        while (i < params.size() && params[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < params.size() && params[i] != ' ') ++i;
        const std::string_view tok = params.substr(start, i - start);
        if (tok.size() < name.size()) continue;
        if (tok.compare(0, name.size(), name) != 0) continue;
        if (tok.size() == name.size()) return true;      // bare format
        if (tok[name.size()] == '=') return true;        // format with params
    }
    return false;
}

/** The `stat-summary` field indicators of an `a=rtcp-xr` parameter list.
 *
 *  RFC 3611 §5.1: "stat-summary" carries a comma-separated list of field
 *  indicators — "loss", "dup", "jitt", "TTL", "HL".  Returns the raw
 *  list; empty when the format is absent. */
inline std::string rtcp_xr_stat_summary_flags(std::string_view params) {
    const std::string_view kName = "stat-summary";
    std::size_t i = 0;
    while (i < params.size()) {
        while (i < params.size() && params[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < params.size() && params[i] != ' ') ++i;
        const std::string_view tok = params.substr(start, i - start);
        if (tok.size() <= kName.size()) continue;
        if (tok.compare(0, kName.size(), kName) != 0) continue;
        if (tok[kName.size()] != '=') continue;
        return std::string(tok.substr(kName.size() + 1));
    }
    return {};
}

/** The `stat-summary` indicators the engine both understands and
 *  produces — "loss" and "jitt".
 *
 *  RFC 3611 §4.6: the duplicate and TTL / Hop-Limit groups are optional
 *  (they are behind their own flag bits), and NimRTC does not measure
 *  them, so the negotiated set is the intersection of the peer's list and
 *  the engine's.
 *
 *  RFC 3611 §5.1 makes the field list *optional* ("stat-summary"
 *  ["=" stat-flag *("," stat-flag)]), so a bare "stat-summary" asks for
 *  the summary as a whole — the engine then reports every field it
 *  produces.  An empty result means "the peer asked for something the
 *  engine does not produce", and the format is then not echoed at all. */
inline std::string rtcp_xr_stat_summary_supported(std::string_view params) {
    if (!rtcp_xr_has_format(params, "stat-summary")) return {};
    const std::string flags = rtcp_xr_stat_summary_flags(params);
    // The format is named but no field was requested — report the
    // engine's whole set.
    if (flags.empty()) return "loss,jitt";
    std::string out;
    // NOTE: the view is taken over the *named* string.  `std::string::substr`
    // returns a temporary, and a `string_view` bound to it would dangle as
    // soon as the full expression ended — the token would then read as
    // garbage (an empty one, in practice) and every field would be dropped.
    const std::string_view fv(flags);
    std::size_t i = 0;
    while (i < fv.size()) {
        while (i < fv.size() && fv[i] == ',') ++i;
        const std::size_t start = i;
        while (i < fv.size() && fv[i] != ',') ++i;
        const std::string_view tok = fv.substr(start, i - start);
        if (tok != "loss" && tok != "jitt") continue;
        if (!out.empty()) out += ',';
        out += std::string(tok);
    }
    return out;
}

}  // namespace nimrtc::engine::detail
