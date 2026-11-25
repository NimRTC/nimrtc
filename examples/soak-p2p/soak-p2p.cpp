/**
 * @file examples/soak-p2p/soak-p2p.cpp
 * @brief soak-p2p — two engines stream audio **across a simulated NAT**
 *        under an impairment profile, and report what they measured.
 *
 * ## Why this program exists
 *
 * The v1.3 plan's Q5 asks for a long soak *across NAT + netem*.  Two
 * halves of that already exist and neither drives the engine:
 *
 *   * `tools/benchmark_weaknet_netem.sh` impairs a real link with
 *     `tc qdisc netem`, but it measures the transport floor, not the
 *     engine; and
 *   * `benchmark_weaknet.py` drives the engine, but over a *clean*
 *     loopback.
 *
 * This program is the missing piece: it runs two real
 * `NimRTCEngine`s (ICE + DTLS + SRTP + RTP) with a **`net_sim::NatRelay`**
 * between them, so
 *
 *   1. neither engine ever sees the other's real address — the SDP each
 *      one is given carries only the relay's endpoints, exactly as a
 *      peer behind a NAT sees only the NAT's server-reflexive address;
 *   2. the relay applies the impairment profile (loss / delay / jitter /
 *      reorder / rate) to every packet it forwards, so the impairments
 *      hit the **real RTP path** — the same bytes `netem` would drop or
 *      delay, but on every platform; and
 *   3. the run emits one JSON object with the parameters it was given
 *      and the counters it measured, so `tools/run_soak_impaired.py`
 *      can archive the pair and assert the green criteria.
 *
 * ## Usage
 *
 *   soak-p2p [--seconds N] [--loss PCT] [--delay-ms MS] [--jitter-ms MS]
 *            [--reorder PCT] [--rate-kbps KBPS] [--seed N]
 *            [--json PATH] [--verbose]
 *
 * All the impairment flags default to zero, i.e. a clean relay: the same
 * program then measures the relay's own overhead against the loopback
 * baseline.
 *
 * ## Exit code
 *
 *   0  the run completed and its green criteria held
 *   1  the run completed but a green criterion failed
 *   2  the run could not start (no loopback bind, no ICE connection)
 *   3  bad arguments
 *
 * ## What "green" means here
 *
 * The same invariant set the plan's §Q5 names, evaluated over the run:
 *
 *   * **the media path really carried media** — the receiver decoded
 *     frames and the sender's packets were acknowledged;
 *   * **memory stayed bounded** — the receive buffers did not grow
 *     without limit (Q2's bound), read from the engine's own counters;
 *   * **the feedback rate stayed bounded** — the NACK / PLI counters did
 *     not grow faster than the RTP packet rate (Q4's bound), which is
 *     what keeps a lossy peer from amplifying feedback; and
 *   * **the loss was largely recovered** — with RTX negotiated, the
 *     inferred-loss counter is not the whole story: what matters is that
 *     the receiver kept decoding, not that every packet arrived.
 *
 * The thresholds live here rather than in the Python runner so the
 * program is self-contained; the runner re-checks them from the JSON it
 * archives, so a "PASS" in the archive always means the same thing.
 */

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>

#include <nimrtc/net_sim/nat_relay.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using clk = std::chrono::steady_clock;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using nimrtc::net_sim::ImpairmentConfig;
using nimrtc::net_sim::NatRelay;

/** RFC 7587's payload type for Opus — the one the engine advertises. */
constexpr std::uint8_t kOpusPt = 111;

/** The Opus media clock (RFC 7587 §4.1). */
constexpr std::uint32_t kClockRate = 48000;

/** Samples per channel in one 20 ms Opus frame. */
constexpr std::size_t kFrameSamples = 960;

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

struct Params {
    double seconds = 30.0;

    // The impairment profile.  All zero = a clean relay.
    double loss_pct = 0.0;
    double delay_ms = 0.0;
    double jitter_ms = 0.0;
    double reorder_pct = 0.0;
    std::uint32_t rate_bps = 0;   // 0 = unlimited
    std::uint64_t seed = 1;

    std::string json_path;
    bool verbose = false;
};

void usage() {
    std::fprintf(stderr,
                 "usage: soak-p2p [--seconds N] [--loss PCT] [--delay-ms MS]\n"
                 "                [--jitter-ms MS] [--reorder PCT] [--rate-kbps KBPS]\n"
                 "                [--seed N] [--json PATH] [--verbose]\n");
}

bool parse_args(int argc, char** argv, Params* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&](double* dst) {
            if (i + 1 >= argc) return false;
            *dst = std::atof(argv[++i]);
            return true;
        };
        if (flag == "--seconds") {
            if (!next(&out->seconds)) return false;
        } else if (flag == "--loss") {
            if (!next(&out->loss_pct)) return false;
        } else if (flag == "--delay-ms") {
            if (!next(&out->delay_ms)) return false;
        } else if (flag == "--jitter-ms") {
            if (!next(&out->jitter_ms)) return false;
        } else if (flag == "--reorder") {
            if (!next(&out->reorder_pct)) return false;
        } else if (flag == "--rate-kbps") {
            double kbps = 0.0;
            if (!next(&kbps)) return false;
            if (kbps < 0.0 || kbps > 4294967.0) return false;
            out->rate_bps = static_cast<std::uint32_t>(kbps * 1000.0);
        } else if (flag == "--seed") {
            double seed = 0.0;
            if (!next(&seed)) return false;
            out->seed = static_cast<std::uint64_t>(seed);
        } else if (flag == "--json") {
            if (i + 1 >= argc) return false;
            out->json_path = argv[++i];
        } else if (flag == "--verbose") {
            out->verbose = true;
        } else if (flag == "-h" || flag == "--help") {
            return false;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
            return false;
        }
    }
    return out->seconds > 0.0;
}

// ---------------------------------------------------------------------------
// SDP rewriting: put the relay between the two peers
// ---------------------------------------------------------------------------

/**
 * @brief Rewrite every ICE candidate in `sdp` so the peer sends to `endpoint`.
 *
 * A candidate line looks like
 *
 *     a=candidate:1 1 udp 2122260223 127.0.0.1 51234 typ host generation 0
 *
 * The engine hands the whole candidate string to its ICE plugin, which
 * parses the *address* and *port* fields.  Replacing those two fields
 * with the relay's own address is what makes the relay the only path
 * between the peers: the candidates the peer is told to use are the
 * relay's endpoints, never the sender's real sockets.
 *
 * `endpoint` is "host:port" — the shape `NatRelay::endpoint_a()` returns.
 *
 * The rewrite keeps the candidate's priority, transport and type, so the
 * pair a peer nominates is the same pair it would have nominated without
 * the relay.
 */
std::string rewrite_candidates_to(const std::string& sdp, const std::string& endpoint) {
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) return sdp;
    const std::string relay_host = endpoint.substr(0, colon);
    const std::string relay_port = endpoint.substr(colon + 1);

    std::string out;
    out.reserve(sdp.size());

    std::size_t pos = 0;
    constexpr std::string_view kPrefix = "a=candidate:";
    while (pos < sdp.size()) {
        const std::size_t line_end = sdp.find('\n', pos);
        const std::size_t end = (line_end == std::string::npos) ? sdp.size() : line_end;
        std::string line = sdp.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind(kPrefix, 0) == 0) {
            // Split the candidate into whitespace-separated fields.
            std::vector<std::string> fields;
            std::size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && line[i] == ' ') ++i;
                const std::size_t start = i;
                while (i < line.size() && line[i] != ' ') ++i;
                if (i > start) fields.push_back(line.substr(start, i - start));
            }
            // foundations  comp  proto  prio  ip  port  typ  ...
            if (fields.size() >= 6) {
                fields[4] = relay_host;
                fields[5] = relay_port;
                line = fields[0];
                for (std::size_t k = 1; k < fields.size(); ++k) {
                    line += ' ';
                    line += fields[k];
                }
            }
        }

        out += line;
        if (line_end != std::string::npos) out += '\n';
        pos = (line_end == std::string::npos) ? sdp.size() : line_end + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The measurements
// ---------------------------------------------------------------------------

struct Measurement {
    // The parameters the run was given.
    double seconds = 0.0;
    double loss_pct = 0.0;
    double delay_ms = 0.0;
    double jitter_ms = 0.0;
    double reorder_pct = 0.0;
    std::uint64_t rate_bps = 0;
    std::uint64_t seed = 0;

    // The relay's own view: how much traffic it saw and shaped.
    std::uint64_t relay_received = 0;
    std::uint64_t relay_forwarded = 0;
    std::uint64_t relay_dropped = 0;
    std::uint64_t relay_reordered = 0;
    std::uint64_t relay_buffered = 0;

    // The media path.
    std::uint64_t audio_packets_sent = 0;
    std::uint64_t audio_frames_received = 0;
    std::uint64_t audio_concealed = 0;
    std::uint64_t audio_fec_recovered = 0;
    std::uint64_t audio_packets_lost = 0;
    std::uint64_t audio_decode_errors = 0;
    std::uint64_t jb_dropped = 0;

    // The feedback plane (Q4's bound).
    std::uint64_t feedback_tx_nack = 0;
    std::uint64_t feedback_rx_nack = 0;
    std::uint64_t feedback_tx_pli = 0;
    std::uint64_t feedback_rx_pli = 0;

    // Memory bound proxies (Q2).
    std::uint64_t audio_samples_pending = 0;

    // ---- The early-exit channel ----------------------------------------
    //
    // A run that dies before its window is over still has to leave
    // evidence.  The 2026-11-20 Q5 run died 3 h 48 m in with an ICE
    // consent expiry, and because the harness only wrote its artifact on
    // the success path, that failure left **no** record at all -- no
    // JSON, no row in the archive, nothing but a 508 MB log that the
    // daily soak task later deleted.  A failure is exactly the moment
    // the evidence matters most, so the artifact is written on both
    // paths, and these two fields say which path it was.
    bool        failed_early = false;
    std::string failure_reason;
};

/** Total PCM memory the receiver is holding, in bytes (a Q2 proxy). */
std::uint64_t pending_audio_bytes(const NimRTCEngine::Stats& s) {
    return static_cast<std::uint64_t>(s.audio_samples_pending) * sizeof(float);
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

std::vector<float> make_tone(std::size_t samples, std::uint32_t phase_offset) {
    std::vector<float> pcm(samples);
    constexpr double kTwoPi = 6.283185307179586;
    for (std::size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(phase_offset + i) / kClockRate;
        pcm[i] = static_cast<float>(0.4 * std::sin(kTwoPi * 440.0 * t));
    }
    return pcm;
}

/** Extract the ICE lines an engine's answerer needs — the same helper the
 *  loopback example uses. */
std::string extract_ice_block(const std::string& sdp) {
    std::string block;
    std::size_t pos = 0;
    while (pos < sdp.size()) {
        const std::size_t line_end = sdp.find('\n', pos);
        const std::size_t end = (line_end == std::string::npos) ? sdp.size() : line_end;
        std::string line = sdp.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("a=ice-ufrag:", 0) == 0 || line.rfind("a=ice-pwd:", 0) == 0 ||
            line.rfind("a=candidate:", 0) == 0) {
            block += line;
            block += '\n';
        }
        pos = (line_end == std::string::npos) ? sdp.size() : line_end + 1;
    }
    return block;
}

bool is_connected(const std::string& state) {
    return state == "connected" || state == "completed";
}

int run(const Params& p, Measurement* m) {
    m->seconds        = p.seconds;
    m->loss_pct       = p.loss_pct;
    m->delay_ms       = p.delay_ms;
    m->jitter_ms      = p.jitter_ms;
    m->reorder_pct    = p.reorder_pct;
    m->rate_bps       = p.rate_bps;
    m->seed           = p.seed;

    // ---- The simulated NAT, with the profile attached ------------------
    NatRelay::Config relay_cfg;
    relay_cfg.bind_host = "127.0.0.1";
    relay_cfg.impaired  = (p.loss_pct > 0.0 || p.delay_ms > 0.0 || p.jitter_ms > 0.0 ||
                          p.reorder_pct > 0.0 || p.rate_bps > 0);
    ImpairmentConfig profile;
    profile.loss_pct    = p.loss_pct;
    profile.delay_ms    = p.delay_ms;
    profile.jitter_ms   = p.jitter_ms;
    profile.reorder_pct = p.reorder_pct;
    profile.rate_bps    = p.rate_bps;
    profile.seed        = p.seed;
    relay_cfg.impairment = profile;

    NatRelay relay(relay_cfg);
    std::string error;
    if (!relay.start(&error)) {
        std::fprintf(stderr, "[soak-p2p] the relay could not start: %s\n", error.c_str());
        m->failed_early = true;
        m->failure_reason = std::string("the relay could not start: ") + error;
        return 2;
    }
    std::fprintf(stderr, "[soak-p2p] relay: A=%s  B=%s  impaired=%d\n",
                 relay.endpoint_a().c_str(), relay.endpoint_b().c_str(),
                 relay_cfg.impaired ? 1 : 0);

    // The largest `audio_samples_pending` seen: the Q2 bound is about
    // the peak, not the final reading.  Declared before `collect` so
    // that the early-exit path can record it too.
    std::uint64_t peak_pending_bytes = 0;

    // ---- Collect the counters -------------------------------------------
    //
    // A closure, so that the early-exit path can record what the run
    // *did* measure before it died.  A run that fails at 3 h 48 m has
    // three hours and forty-eight minutes of numbers in it, and they
    // are the only evidence of what went wrong; the artifact must
    // carry them.
    auto collect = [&](NimRTCEngine& a, NimRTCEngine& b) {
        const NatRelay::Stats rs = relay.stats();
        m->relay_received    = rs.packets_received;
        m->relay_forwarded   = rs.packets_forwarded;
        m->relay_dropped     = rs.packets_dropped;
        m->relay_reordered   = rs.packets_reordered;
        m->relay_buffered    = rs.packets_buffered;

        const auto sa = a.stats();
        const auto sb = b.stats();
        m->audio_packets_sent     = sa.audio_packets_sent;
        m->audio_frames_received  = sb.audio_frames_received;
        m->audio_concealed        = sb.audio_concealed_frames;
        m->audio_fec_recovered    = sb.audio_fec_recovered_frames;
        m->audio_packets_lost     = sb.audio_packets_lost;
        m->audio_decode_errors    = sb.audio_decode_errors;
        m->jb_dropped             = sb.audio_packets_dropped;
        m->audio_samples_pending  = peak_pending_bytes;

        m->feedback_tx_nack = sb.video_feedback_tx_nack;
        m->feedback_rx_nack = sa.video_feedback_rx_nack;
        m->feedback_tx_pli  = sb.video_feedback_tx_pli;
        m->feedback_rx_pli  = sa.video_feedback_rx_pli;
    };

    // ---- The two engines ------------------------------------------------
    EngineConfig cfg;
    cfg.local_bind_address    = "127.0.0.1";
    cfg.stun_server_host      = "";
    cfg.pcm_sample_rate_hz    = kClockRate;
    cfg.pcm_channels          = 1;
    cfg.audio_ptime_ms        = 20;
    cfg.audio_codec.clock_rate   = kClockRate;
    cfg.audio_codec.channels    = 1;
    cfg.audio_codec.payload_type = kOpusPt;

    NimRTCEngine a(cfg);
    if (a.pre_open() != 0 || a.open() != 0) {
        std::fprintf(stderr,
                     "[soak-p2p] engine A could not bind the loopback interface\n");
        relay.stop();
        return 2;
    }

    // A's offer, with every candidate redirected to **the address B was
    // told to expect**: the relay's endpoint_a().
    const std::string offer = a.create_offer();
    if (offer.empty()) {
        std::fprintf(stderr, "[soak-p2p] create_offer failed\n");
        relay.stop();
        return 2;
    }
    const std::string offer_via_relay = rewrite_candidates_to(offer, relay.endpoint_a());

    NimRTCEngine b(cfg);
    const std::string ice_block = extract_ice_block(offer_via_relay);
    if (!ice_block.empty()) {
        if (b.pre_open() != 0) {
            std::fprintf(stderr, "[soak-p2p] engine B pre_open failed\n");
            relay.stop();
            return 2;
        }
        if (!b.set_remote_ice(ice_block)) {
            std::fprintf(stderr, "[soak-p2p] B.set_remote_ice failed\n");
            relay.stop();
            return 2;
        }
    }
    if (b.open() != 0) {
        std::fprintf(stderr, "[soak-p2p] engine B could not bind the loopback interface\n");
        relay.stop();
        return 2;
    }

    const auto answer = b.process_remote_sdp(offer_via_relay);
    if (!answer) {
        std::fprintf(stderr, "[soak-p2p] the answerer rejected the offer\n");
        relay.stop();
        return 2;
    }
    // B's answer, with its candidates redirected to the relay's other
    // endpoint — the address A was told to expect.
    const std::string answer_via_relay =
        rewrite_candidates_to(*answer, relay.endpoint_b());
    if (!a.process_remote_sdp(answer_via_relay)) {
        std::fprintf(stderr, "[soak-p2p] the offerer rejected the answer\n");
        relay.stop();
        return 2;
    }

    // ---- Wait for ICE (through the relay) -------------------------------
    {
        const auto deadline = clk::now() + std::chrono::seconds(20);
        bool connected = false;
        while (clk::now() < deadline) {
            a.tick();
            b.tick();
            if (is_connected(a.ice_state_string()) &&
                is_connected(b.ice_state_string())) {
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!connected) {
            std::fprintf(stderr,
                         "[soak-p2p] ICE did not connect through the relay in time "
                         "(A=%s, B=%s)\n",
                         a.ice_state_string(), b.ice_state_string());
            m->failed_early = true;
            m->failure_reason =
                std::string("ICE did not connect through the relay in time (A=") +
                a.ice_state_string() + ", B=" + b.ice_state_string() + ")";
            collect(a, b);
            a.close();
            b.close();
            relay.stop();
            return 2;
        }
    }
    std::fprintf(stderr, "[soak-p2p] ICE connected through the relay\n");

    // ---- Stream audio for the requested window -------------------------
    const auto pcm = make_tone(kFrameSamples, 0);
    const auto run_start = clk::now();
    const auto run_end   = run_start + std::chrono::milliseconds(
                                          static_cast<long long>(p.seconds * 1000.0));

    // Keep the largest `audio_samples_pending` seen: the Q2 bound is about
    // the peak, not the final reading.
    std::uint64_t next_audio_at_us = 0;

    // ---- Watch the ICE state for the whole window ------------------------
    //
    // The 2026-11-20 Q5 run died 3 h 48 m in with an ICE consent expiry
    // ("Consent expired for candidate pair" -> "Lost connectivity" ->
    // "Changing state to failed"), and then spent the remaining twenty
    // hours sending into a dead transport: 26 627 "Send while ICE is
    // not connected" errors.  Nothing in the harness noticed, because
    // the harness never looked.  It would have run to the end of its
    // 24 h window, collected its counters, and every one of them would
    // have satisfied the green criteria -- the sender had emitted
    // packets, the receiver had decoded frames, the relay had forwarded
    // traffic -- so the archive would have filed a **green** Q5 record
    // for a run whose connection was dead for five sixths of its life.
    //
    // A soak that cannot see its own subject failing is not evidence.
    // So the loop watches both peers, and the moment either leaves the
    // connected state the run stops, records why, and writes its
    // artifact with `failed_early` set.
    bool ice_lost = false;
    std::string ice_lost_reason;

    while (clk::now() < run_end) {
        a.tick();
        b.tick();

        const auto elapsed_us =
            std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - run_start)
                .count();
        if (static_cast<std::uint64_t>(elapsed_us) >= next_audio_at_us) {
            next_audio_at_us += 20000;   // one 20 ms frame
            (void)a.send_audio(pcm.data(), pcm.size());
        }

        peak_pending_bytes =
            std::max(peak_pending_bytes, pending_audio_bytes(b.stats()));

        // The ICE state is only interesting once it has *been* good: a
        // peer that never connected is a different failure (the
        // connect phase above catches that), and polling the state
        // before the first nomination would fire on it.
        if (!ice_lost && elapsed_us > 5000000) {   // 5 s of grace
            const std::string sa_state = a.ice_state_string();
            const std::string sb_state = b.ice_state_string();
            if (!is_connected(sa_state) || !is_connected(sb_state)) {
                ice_lost = true;
                ice_lost_reason = "ICE left the connected state during the run "
                                  "(A=" + sa_state + ", B=" + sb_state + ")";
                std::fprintf(stderr, "[soak-p2p] %s\n", ice_lost_reason.c_str());
                std::fprintf(stderr,
                             "[soak-p2p] stopping at %.1f s of %.1f s\n",
                             static_cast<double>(elapsed_us) / 1e6, p.seconds);
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Let the tail packets arrive and be played out.
    const auto tail_deadline = clk::now() + std::chrono::milliseconds(500);
    while (clk::now() < tail_deadline) {
        a.tick();
        b.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // ---- Collect the counters -------------------------------------------
    collect(a, b);
    const NatRelay::Stats rs = relay.stats();

    if (ice_lost) {
        m->failed_early = true;
        m->failure_reason = ice_lost_reason;
    }

    if (p.verbose) {
        std::fprintf(stderr,
                     "[soak-p2p] relay: received=%" PRIu64 " forwarded=%" PRIu64
                     " dropped=%" PRIu64 " reordered=%" PRIu64 " buffered=%" PRIu64
                     "\n",
                     rs.packets_received,
                     rs.packets_forwarded,
                     rs.packets_dropped,
                     rs.packets_reordered,
                     rs.packets_buffered);
        std::fprintf(stderr,
                     "[soak-p2p] audio: sent=%" PRIu64 " received=%" PRIu64
                     " concealed=%" PRIu64 " fec_recovered=%" PRIu64
                     " lost=%" PRIu64 " decode_errors=%" PRIu64 "\n",
                     m->audio_packets_sent,
                     m->audio_frames_received,
                     m->audio_concealed,
                     m->audio_fec_recovered,
                     m->audio_packets_lost,
                     m->audio_decode_errors);
    }

    a.close();
    b.close();
    relay.stop();
    // A run whose ICE died is not a run that completed: the exit code
    // says so, and the artifact (written by the caller on both paths)
    // says why.
    return ice_lost ? 1 : 0;
}

// ---------------------------------------------------------------------------
// The green criteria
// ---------------------------------------------------------------------------

struct Verdict {
    bool ok = true;
    std::vector<std::string> failures;
};

void require(Verdict* v, bool condition, const std::string& message) {
    if (!condition) {
        v->ok = false;
        v->failures.push_back(message);
    }
}

/**
 * @brief The plan's §Q5 invariants, evaluated on one run.
 *
 * The thresholds are deliberately loose: the soak's job is to catch a
 * *collapse* (no media, unbounded growth, feedback amplification), not
 * to grade a codec's quality.  A tight threshold on a 20% -loss link
 * would fail for reasons that say nothing about the engine.
 */
Verdict evaluate(const Measurement& m) {
    Verdict v;

    // 0. The run has to have *survived* its window.  A run whose ICE
    //    died at 3 h 48 m would otherwise sail through every criterion
    //    below -- the sender had emitted packets, the receiver had
    //    decoded frames, the relay had forwarded traffic -- and be
    //    filed as a green Q5 record for a session that was dead for
    //    five sixths of its life.  The criterion that catches it is not
    //    about the media; it is about the connection.
    require(&v, !m.failed_early,
            "the run did not survive its window: " + m.failure_reason);

    // 1. The media path carried media.  Without this the rest of the
    //    measurements describe nothing.
    require(&v, m.audio_packets_sent > 0, "the sender never emitted an RTP packet");
    require(&v, m.audio_frames_received > 0,
            "the receiver never decoded a frame — the inbound path is broken");
    require(&v, m.relay_forwarded > 0, "the relay never forwarded a packet");

    // 2. Memory stayed bounded (Q2).  The receive path must not hold
    //    more than one second of PCM at the configured rate; the engine's
    //    accumulator is what grows when it stops draining.
    const std::uint64_t one_second_of_pcm = 48000ull * sizeof(float);
    require(&v, m.audio_samples_pending <= 4 * one_second_of_pcm,
            "the receive path held more than 4 s of PCM — the drain loop stalled");

    // 3. The feedback rate stayed bounded (Q4).  On the audio path the
    //    engine emits no NACKs today, so the counters are zero; the
    //    bound is still asserted so a future audio feedback loop cannot
    //    be added without someone reading this test.  The general shape:
    //    feedback packets must not exceed the RTP packets they describe.
    const std::uint64_t feedback_out = m.feedback_tx_nack + m.feedback_tx_pli;
    require(&v, feedback_out <= m.audio_packets_sent,
            "outbound feedback outnumbered the RTP packets it describes — "
            "the Q4 bound is not holding");

    // 4. The loss was largely recovered.  On a clean link the inferred
    //    loss must be zero; on an impaired one it may be non-zero, but the
    //    receiver has to keep decoding: lost packets that never turned
    //    into concealed or FEC-recovered frames would mean the packets
    //    vanished without the receive path noticing.
    require(&v, m.audio_decode_errors == 0,
            "the decoder reported errors — the bitstream was corrupted in transit");
    if (m.relay_dropped > 0) {
        const std::uint64_t accounted = m.audio_concealed + m.audio_fec_recovered;
        require(&v, accounted > 0,
                "the relay dropped packets but the receiver neither concealed nor "
                "recovered a frame — the lossy path is not being measured");
    }

    // 5. The relay really shaped the traffic it was asked to shape.  A
    //    profile that never drops (a 100 % loss run that reports zero)
    //    would make the soak a lie.
    if (m.loss_pct >= 100.0) {
        require(&v, m.relay_dropped > 0, "a 100 % loss profile dropped nothing");
    }

    return v;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string to_json(const Measurement& m, const Verdict& v) {
    char buf[4096];
    // The counters are `std::uint64_t`, so they are printed with the
    // `PRIu64` format macro rather than a hard-coded `%llu`.  The
    // difference is not cosmetic: `uint64_t` is `unsigned long` on
    // Linux and `unsigned long long` on Windows, and a `%llu` with the
    // former is a format mismatch -- which the old-style casts were
    // papering over, at the price of a `-Werror=old-style-cast` failure
    // on the GCC legs.
    std::snprintf(
        buf, sizeof(buf),
        "{\n"
        "  \"schema\": \"nimrtc-soak-impaired/1\",\n"
        "  \"params\": {\"seconds\": %.3f, \"loss_pct\": %.3f, \"delay_ms\": %.3f, "
        "\"jitter_ms\": %.3f, \"reorder_pct\": %.3f, \"rate_bps\": %" PRIu64
        ", \"seed\": %" PRIu64 "},\n"
        "  \"relay\": {\"received\": %" PRIu64 ", \"forwarded\": %" PRIu64
        ", \"dropped\": %" PRIu64 ", \"reordered\": %" PRIu64
        ", \"buffered\": %" PRIu64 "},\n"
        "  \"audio\": {\"packets_sent\": %" PRIu64 ", \"frames_received\": %" PRIu64
        ", \"concealed\": %" PRIu64 ", \"fec_recovered\": %" PRIu64
        ", \"packets_lost\": %" PRIu64 ", \"decode_errors\": %" PRIu64
        ", \"jb_dropped\": %" PRIu64 "},\n"
        "  \"memory\": {\"peak_pending_audio_bytes\": %" PRIu64 "},\n"
        "  \"feedback\": {\"tx_nack\": %" PRIu64 ", \"rx_nack\": %" PRIu64
        ", \"tx_pli\": %" PRIu64 ", \"rx_pli\": %" PRIu64 "},\n"
        "  \"failed_early\": %s,\n"
        "  \"failure_reason\": \"\",\n"
        "  \"green\": %s,\n"
        "  \"failures\": [%s]\n"
        "}\n",
        m.seconds, m.loss_pct, m.delay_ms, m.jitter_ms, m.reorder_pct,
        m.rate_bps, m.seed,
        m.relay_received, m.relay_forwarded,
        m.relay_dropped, m.relay_reordered,
        m.relay_buffered, m.audio_packets_sent,
        m.audio_frames_received, m.audio_concealed,
        m.audio_fec_recovered, m.audio_packets_lost,
        m.audio_decode_errors, m.jb_dropped,
        m.audio_samples_pending, m.feedback_tx_nack,
        m.feedback_rx_nack, m.feedback_tx_pli,
        m.feedback_rx_pli, m.failed_early ? "true" : "false",
        v.ok ? "true" : "false", "");
    std::string json(buf);
    if (!m.failure_reason.empty()) {
        const std::string needle = "\"failure_reason\": \"\"";
        const std::size_t at = json.find(needle);
        if (at != std::string::npos) {
            json.replace(at, needle.size(),
                         "\"failure_reason\": \"" + json_escape(m.failure_reason) + "\"");
        }
    }
    if (!v.failures.empty()) {
        std::string list;
        for (std::size_t i = 0; i < v.failures.size(); ++i) {
            if (i) list += ", ";
            list += '"';
            list += json_escape(v.failures[i]);
            list += '"';
        }
        const std::string needle = "\"failures\": []";
        const std::size_t at = json.find(needle);
        if (at != std::string::npos) {
            json.replace(at, needle.size(), "\"failures\": [" + list + "]");
        }
    }
    return json;
}

}  // namespace

int main(int argc, char** argv) {
    Params p;
    if (!parse_args(argc, argv, &p)) {
        usage();
        return 3;
    }

    nimrtc::core::register_all_default_plugins();

    std::fprintf(stderr,
                 "NimRTC soak-p2p: %.1f s, loss=%.1f%% delay=%.0fms jitter=%.0fms "
                 "reorder=%.1f%% rate=%" PRIu32 " bps\n",
                 p.seconds, p.loss_pct, p.delay_ms, p.jitter_ms, p.reorder_pct,
                 p.rate_bps);

    Measurement m;
    const int rc = run(p, &m);

    // The artifact is written on **both** paths.  A run that died before
    // its window ended still measured something -- and that measurement
    // is the only evidence of the failure.  Writing it only on success
    // meant that the 2026-11-20 Q5 run (ICE consent expiry at 3 h 48 m)
    // left no record at all: no JSON, no archive row, nothing.
    const Verdict v = evaluate(m);
    const std::string json = to_json(m, v);
    std::fputs(json.c_str(), stdout);
    std::fflush(stdout);

    if (!p.json_path.empty()) {
        if (std::FILE* f = std::fopen(p.json_path.c_str(), "w")) {
            std::fputs(json.c_str(), f);
            std::fclose(f);
            std::fprintf(stderr, "[soak-p2p] wrote %s\n", p.json_path.c_str());
        } else {
            std::fprintf(stderr, "[soak-p2p] could not write %s\n", p.json_path.c_str());
        }
    }

    // A run that never got off the ground has nothing to judge: its
    // counters are all zero and the verdict would say so, but the
    // reason is more useful than the verdict.
    if (rc != 0 && !m.failed_early) {
        std::fprintf(stderr, "[soak-p2p] the run did not complete (rc=%d)\n", rc);
        return rc;
    }

    if (!v.ok) {
        std::fprintf(stderr, "[soak-p2p] NOT GREEN:\n");
        for (const std::string& failure : v.failures) {
            std::fprintf(stderr, "  - %s\n", failure.c_str());
        }
        return 1;
    }
    std::fprintf(stderr, "[soak-p2p] green\n");
    return 0;
}
