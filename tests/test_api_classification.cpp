// Note: This file intentionally saved as UTF-8 with BOM so that MSVC
// (compiling with a non-UTF-8 code page such as CP936/GBK on Chinese Windows)
// does not emit C4819 for any future non-ASCII characters in comments.

/**
 * @file tests/test_api_classification.cpp
 * @brief v1.0.0-alpha.1 Sec.12.3.1 DoD gate #6 (extension) -- public header
 *        classification coverage check.
 *
 * ## What this test verifies
 *
 * Per ADR-015 (`docs/adr/ADR-015-api-stability-policy.md`) and
 * `docs/plan/v1.0-plan.md` Sec.12.3.1, every public symbol under
 * `src/<mod>/include/nimrtc/<mod>/<name>.hpp` must carry a stability
 * classification.  In v1.0.0-alpha.1 that means every public
 * header file must contain at least one `NIMRTC_API_STABLE` or
 * `NIMRTC_API_UNSTABLE` marker.
 *
 * This test walks the source tree at runtime and asserts:
 *
 * 1. `every_header_has_at_least_one_marker` -- for every `.hpp`
 *    under `<root>/src/<mod>/include/nimrtc/<mod>` (excluding
 *    `<root>/src/third_party/<mod>`), the file content contains at
 *    least one occurrence of `NIMRTC_API_STABLE` or
 *    `NIMRTC_API_UNSTABLE`.  Failures print the full missing list
 *    so the next PR can apply the missing markers mechanically.
 *
 * 2. `total_header_count_matches_baseline` -- the count of public
 *    headers equals the v1.0.0-beta.1 baseline of 100.  A drift
 *    here signals that new public headers were added without
 *    updating the classification sweep; bump the baseline in the
 *    same PR that adds the header.
 *
 * ## Why runtime (not compile-time) check
 *
 * `NIMRTC_API_STABLE` and `NIMRTC_API_UNSTABLE` are empty macros
 * in v1.0.0-alpha.1 (visual markers only).  A compile-time check
 * cannot detect "header X has no markers" because the preprocessor
 * erases them.  A runtime filesystem walk is the authoritative
 * way to count marker occurrences per file.
 *
 * ## Configuration
 *
 * `NIMRTC_SOURCE_ROOT` is injected by CMake
 * (`target_compile_definitions(... NIMRTC_SOURCE_ROOT=${CMAKE_SOURCE_DIR})`).
 * The test must not hardcode any path; it only uses the compile-time
 * injection so the same binary runs unmodified from any build
 * directory.
 *
 * ## Platform coverage
 *
 * Windows MSVC / Linux GCC / macOS Clang.  Uses C++17 `<filesystem>`.
 * No network access.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef NIMRTC_SOURCE_ROOT
    #error "NIMRTC_SOURCE_ROOT must be defined via CMake target_compile_definitions"
#endif

namespace fs = std::filesystem;

namespace {

// -----------------------------------------------------------------------------
// Filesystem walker
// -----------------------------------------------------------------------------

/// Recursively collect every `.hpp` under `<root>/src/<mod>/include/nimrtc/<mod>`
/// that is NOT under `<root>/src/third_party/<mod>`.
///
/// Returns paths sorted lexicographically (stable ordering for ctest output).
std::vector<fs::path> collect_public_headers(const fs::path& root) {
    std::vector<fs::path> result;
    fs::path src = root / "src";
    if (!fs::exists(src) || !fs::is_directory(src)) {
        return result;
    }
    for (auto entry = fs::recursive_directory_iterator(
                 src, fs::directory_options::skip_permission_denied);
         entry != fs::recursive_directory_iterator();
         ++entry) {
        if (!entry->is_regular_file()) {
            continue;
        }
        const fs::path& p = entry->path();
        // Normalize path separators to '/' for substring matching so the
        // check works identically on Windows (backslash) and POSIX (slash).
        std::string path_str = p.generic_string();
        // Must live under .../include/nimrtc/...
        if (path_str.find("/include/nimrtc/") == std::string::npos) {
            continue;
        }
        // Must not live under src/third_party/.
        if (path_str.find("/third_party/") != std::string::npos) {
            continue;
        }
        if (p.extension() != ".hpp") {
            continue;
        }
        result.push_back(p);
    }
    std::sort(result.begin(), result.end());
    return result;
}

// -----------------------------------------------------------------------------
// Marker counter (per file)
// -----------------------------------------------------------------------------

struct MarkerCounts {
    int stable     = 0;
    int unstable   = 0;
    int deprecated = 0;
};

/// Count occurrences of NIMRTC_API_STABLE / UNSTABLE / DEPRECATED in `file`.
///
/// `std::regex` is used (not <regex> std::search) so the match is anchored
/// at a word boundary and avoids false positives in unrelated identifiers
/// (e.g. `NIMRTC_API_STABLE_FOO`).
MarkerCounts count_markers(const fs::path& file) {
    std::ifstream f(file);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();

    auto count = [&content](const char* pat) -> int {
        // The sregex_iterator constructor takes a const std::basic_regex&,
        // not an rvalue, so we must store the regex in a named local.
        // Passing a temporary `std::regex(pat)` directly triggers C2280
        // ("attempting to reference a deleted function") on MSVC.
        const std::regex re(pat);
        const auto begin = std::sregex_iterator(content.begin(), content.end(), re);
        const auto end = std::sregex_iterator();
        return static_cast<int>(std::distance(begin, end));
    };

    MarkerCounts c;
    c.stable     = count(R"(NIMRTC_API_STABLE\b)");
    c.unstable   = count(R"(NIMRTC_API_UNSTABLE\b)");
    c.deprecated = count(R"(NIMRTC_API_DEPRECATED\b)");
    return c;
}

} // namespace

// -----------------------------------------------------------------------------
// Test 1 -- every public header has >=1 stability classification marker
// -----------------------------------------------------------------------------

TEST(public_header_classification, every_header_has_at_least_one_marker) {
    const fs::path root = fs::path(NIMRTC_SOURCE_ROOT);
    const auto headers = collect_public_headers(root);

    ASSERT_FALSE(headers.empty())
        << "No public headers found under " << root
        << "/src/<mod>/include/nimrtc/<mod>.  Check NIMRTC_SOURCE_ROOT and the "
        << "include-directory layout in the project.";

    std::vector<fs::path> missing;
    int total_stable = 0;
    int total_unstable = 0;
    int total_deprecated = 0;

    for (const auto& h : headers) {
        const MarkerCounts c = count_markers(h);
        total_stable     += c.stable;
        total_unstable   += c.unstable;
        total_deprecated += c.deprecated;
        if (c.stable == 0 && c.unstable == 0) {
            missing.push_back(h);
        }
    }

    // Print a human-readable summary to stdout so ctest -V shows it on
    // both pass and fail runs.  Engineers triaging the sweep can read the
    // exact list without rerunning with -V -V.
    std::cout << "[classification] source_root=" << root.generic_string() << '\n'
              << "[classification] headers=" << headers.size()
              << " stable=" << total_stable
              << " unstable=" << total_unstable
              << " deprecated=" << total_deprecated
              << " missing_markers=" << missing.size() << '\n';

    if (!missing.empty()) {
        std::cout << "[classification] missing files (" << missing.size()
                  << "):\n";
        for (const auto& m : missing) {
            // Strip the source-root prefix for readability.
            std::string rel = m.generic_string();
            const std::string root_prefix =
                std::string(root.generic_string()) + "/";
            if (rel.rfind(root_prefix, 0) == 0) {
                rel = rel.substr(root_prefix.size());
            }
            std::cout << "  " << rel << '\n';
        }
    }

    EXPECT_TRUE(missing.empty())
        << missing.size() << " of " << headers.size()
        << " public headers have no NIMRTC_API_STABLE/UNSTABLE marker. "
        << "See the [classification] section above for the full list. "
        << "Per ADR-015, every public symbol must carry a stability "
        << "classification; apply NIMRTC_API_STABLE to all public "
        << "declarations in the missing files.";
}

// -----------------------------------------------------------------------------
// Test 2 -- total header count matches the v1.0.0-alpha.1 baseline
// -----------------------------------------------------------------------------

TEST(public_header_classification, total_header_count_matches_baseline) {
    const fs::path root = fs::path(NIMRTC_SOURCE_ROOT);
    const auto headers = collect_public_headers(root);

    // v1.0.0-alpha.2 baseline: 97 public headers (89 from alpha.1
    // plus 8 new headers landing with v0.14.0 alpha.2:
    //   7 new headers under src/web_transport/include/nimrtc/
    //   web_transport/ (the WebTransport / QUIC seam, per
    //   ADR-014 + v0.14-plan.md section 2.1.1 WT-SEAM):
    //     - web_transport_session_iface.hpp
    //     - web_transport_session_factory.hpp
    //     - bidi_stream_iface.hpp
    //     - uni_stream_iface.hpp
    //     - datagram_facade.hpp
    //     - web_transport_config.hpp
    //     - web_transport_state.hpp
    //   1 new header under src/modules/dtls/include/nimrtc/dtls/
    //     (the DTLS handshake reassembler seam):
    //     - dtls_handshake_reassembler.hpp
    // Verified 2026-10-16 by the alpha.2 classification sweep;
    // see docs/plan/v0.14-plan.md section 3.1 Gate #2 (the alpha.2
    // seam compile-only test pins the same header set).
    //
    // v0.14.0-beta.1 bump: +2 public headers (97 -> 99) for the
    //   `WebTransportDataChannel` data-plane landing (DC-1 /
    //   ADR-014):
    //     - web_transport_data_channel.hpp
    //     - web_transport_plugin.hpp
    //
    // v0.14.0-beta.2 bump: +1 public header (99 -> 100) for the
    //   WebTransport server seam (Case E / WT-SEVER):
    //     - web_transport_server.hpp
    //
    // ADR-017 bump: +1 public header (100 -> 101) for the
    //   NTP <-> RTP-TS timeline seam:
    //     - rtp/timeline.hpp
    //
    // v0.15.0 bump: +1 public header (101 -> 102) for the
    //   SCTP DCEP codec (DATA_CHANNEL_OPEN/ACK encode/decode):
    //     - sctp/dcep.hpp
    //
    // v1.1 M2 bump: +1 public header (102 -> 103) for the
    //   StrategyValidator p99 budget enforcement helper:
    //     - transport/strategy_validator.hpp
    //
    // v1.1 Q2 bump: +1 public header (103 -> 104) for the shared
    //   adaptive jitter-buffer strategy (audio + video front-ends):
    //     - jb_strategy/adaptive_delay.hpp
    //
    // v1.1 X2 bump: +2 public headers (104 -> 106) for the SRTP
    //   backend seam (seam only, SM4-GCM out of scope for v1.1):
    //     - plugins/srtp_backend.hpp (ISrtpSessionBackend + factory)
    //     - srtp/libsrtp_backend.hpp (libsrtp-backed seam adapter)
    //
    // v1.2.0 bump: +3 public headers (106 -> 109) for the audio
    //   pipeline's two plugin seams and the audio_rx module header
    //   (spec E-8; audio-pipeline plan WP-1 / WP-3):
    //     - plugins/audio_decoder.hpp (IAudioDecoder + factory)
    //     - plugins/audio_receiver.hpp (IAudioReceiver + factory)
    //     - audio_rx/audio_rx.hpp (the module's public header)
    //
    // v1.2.0 M1 bump: +1 public header (109 -> 110) for the RTCP
    //   compound-packet / REMB demux types:
    //     - rtp/rtcp.hpp
    //
    // v1.2.0 M1 bump: +2 public headers (110 -> 112) for the
    //   receiver-side network-feedback seam and its module header
    //   (video-pipeline plan F2):
    //     - plugins/net_feedback.hpp (INetFeedback + factory)
    //     - net_feedback/net_feedback.hpp (the module's public header)
    //
    // P1 hardening bump (2026-11-09): +1 public header (112 -> 113)
    //   for the registry umbrella that replaces the seam includes
    //   that used to live at the bottom of nimrtc/core/registry.hpp:
    //     - plugins/registry.hpp (the registry + the 13 nimrtc/plugins/
    //       seam interfaces; the optional-module seams stay out of it)
    //
    // v1.2.0 bump (2026-11-10): +4 public headers (113 -> 117) for the
    //   session-model seam, the session module's model header, and the
    //   RFC 4733 (DTMF) seam (session capability plan N1 / T1 / D1):
    //     - plugins/session.hpp (ISession + factory) -- the seam the
    //       engine drives to build the SDP media list; the concrete
    //       model stays in src/modules/session/ and is reached through
    //       the registry, not by including the module's headers.
    //     - session/session_model.hpp (the module's public header --
    //       the `plugins::ISession` implementation the engine drives)
    //     - plugins/dtmf.hpp (IDtmfCodec -- the RFC 4733
    //       telephone-event seam; DTMF is off by default, see the
    //       session capability plan's decision OD-B)
    //     - dtmf/dtmf.hpp (the DTMF module's own public header -- the
    //       concrete RFC 4733 codec; the seam above is what the engine
    //       resolves through the registry)
    //
    // v1.2.0 M3 bump: +1 public header (117 -> 118) for the token-bucket
    //   pacer (video-pipeline plan P1):
    //     - pacing/pacing.hpp (the token bucket on a monotonic clock)
    //
    //
    // v1.3 bump: +2 public headers (121 -> 123) for the H.265 / AV1 RTP
    //   payload formats (video plan C4 / C6):
    //     - video_payload/h265.hpp (RFC 7798 Single NAL / AP / FU)
    //     - video_payload/av1.hpp (draft-ietf-payload-av1 framing)
    //
    // v1.3 bump: +3 public headers (123 -> 126) for the shared `nvcodec`
    //   module -- the NVIDIA driver integration (NVENC / NVDEC) that the
    //   H.264, H.265 and AV1 codec modules consume:
    //     - nvcodec/nv_codec_kind.hpp (the codec descriptor table)
    //     - nvcodec/nv_backends.hpp (the codec-generic NVENC / NVDEC
    //       entry points)
    //     - nvcodec/hw_composite_codec.hpp (the encoder+decoder adapter)
    //
    // v1.3 bump: +6 public headers (126 -> 132) for the H.265 and AV1
    //   codec modules (video plan C4 / C8) — the codec-specific halves
    //   that sit on top of the shared `nvcodec` driver integration:
    //     - h265/codec_plugin.hpp (the H.265 plugin factory)
    //     - h265/decoder.hpp (the H.265 software decoder)
    //     - h265/hw_backends.hpp (the H.265 NVENC / NVDEC backend ids)
    //     - av1/codec_plugin.hpp (the AV1 plugin factory)
    //     - av1/decoder.hpp (the AV1 software decoder)
    //     - av1/hw_backends.hpp (the AV1 NVENC / NVDEC backend ids)
    //
    // v1.2.0 (session capability plan X1 / X3) bump: +4 public headers
    //   (118 -> 122) for the RTCP Extended Report seam and the TMMBR
    //   messages:
    //     - plugins/xr_report.hpp (IXrReport + factory -- the RFC 3611
    //       seam the engine routes the inbound XR sub-packets into)
    //     - rtp/rtcp_xr.hpp (the RFC 3611 XR block parser / builder)
    //     - rtp/rtcp_tmmbr.hpp (the RFC 5104 TMMBR / TMMBN messages)
    //     - rtp/xr_report_plugin.hpp (the module-side adapter that puts
    //       the XR parser behind the seam)
    //
    // v1.3 bump: +3 public headers (122 -> 125) for the shared `nvcodec`
    //   module -- the NVIDIA driver integration (NVENC / NVDEC) that the
    //   H.264, H.265 and AV1 codec modules consume:
    //     - nvcodec/nv_codec_kind.hpp (the codec descriptor table)
    //     - nvcodec/nv_backends.hpp (the codec-generic NVENC / NVDEC
    //       entry points)
    //     - nvcodec/hw_composite_codec.hpp (the encoder+decoder adapter)
    //
    // v1.3 bump: +2 public headers (125 -> 127) for the H.265 / AV1 RTP
    //   payload formats (video plan C4 / C6):
    //     - video_payload/h265.hpp (RFC 7798 Single NAL / AP / FU)
    //     - video_payload/av1.hpp (draft-ietf-payload-av1 framing)
    //
    // v1.3 bump: +6 public headers (127 -> 133) for the H.265 and AV1
    //   codec modules (video plan C4 / C8) — the codec-specific halves
    //   that sit on top of the shared `nvcodec` driver integration:
    //     - h265/codec_plugin.hpp (the H.265 plugin factory)
    //     - h265/decoder.hpp (the H.265 software decoder)
    //     - h265/hw_backends.hpp (the H.265 NVENC / NVDEC backend ids)
    //     - av1/codec_plugin.hpp (the AV1 plugin factory)
    //     - av1/decoder.hpp (the AV1 software decoder)
    //     - av1/hw_backends.hpp (the AV1 NVENC / NVDEC backend ids)
    //
    // `session/track.hpp` already existed at v1.1.0 and is part of the
    // 113 baseline; it is only listed above because the two session
    // headers that are new are siblings of it.
    //
    // v1.3 bump: +3 public headers (133 -> 136) for the video-capture
    //   layer (remote-desktop plan A2 / A2b) — the bounded frame queue
    //   the capture backends hand frames to, and the plugin seam the
    //   engine resolves the capture device through:
    //     - plugins/video_capture.hpp (IVideoCapture + factory seam)
    //     - video_capture/capture_queue.hpp (the portable bounded queue)
    //     - video_capture/synthetic_capture.hpp (the synthetic device)
    //
    // Logged baseline values from the alpha.1 dry run:
    //     2/2 PASS, headers=89 stable=107 unstable=6 deprecated=8
    //     missing_markers=0.
    // Bump this constant in the same PR that intentionally adds/removes
    // a public header.
    constexpr std::size_t kBaselineHeaderCount = 136;

    EXPECT_EQ(headers.size(), kBaselineHeaderCount)
        << "Public header count drifted from the v1.0.0-beta.1 baseline "
        << "of " << kBaselineHeaderCount << ".  Either new public headers "
        << "were added (update the baseline + run the classification sweep) "
        << "or a public header was removed (update the baseline + ADR-015).";
}
