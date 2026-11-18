/**
 * @file examples/remote-desktop/rd_viewer.cpp
 * @brief The remote-desktop **viewer** — the operator's machine.
 *
 * ## What this program does
 *
 * The viewer is the **offerer** (see `docs/guides/remote-desktop.md`
 * §2.1 — the operator's client opens the outgoing path, because the
 * device is a server-shaped peer behind the operator's NAT).  It:
 *
 * 1. creates the SDP offer and trickles the ICE candidates to the
 *    device;
 * 2. receives the device's screen: the engine's video receiver
 *    depacketises, the codec decodes, and the **video sink** presents
 *    the picture (the D3D11 renderer on Windows, the headless sink
 *    when the operator only wants to drive the machine without looking
 *    at it);
 * 3. captures the operator's **local** mouse and keyboard, coalesces
 *    the pointer motion to the display rate (plan G4), and sends the
 *    events over the control channel (plan G3 / G7);
 * 4. sends the **liveness heartbeat** (plan G6) — the device's
 *    dead-man watchdog counts the beats, and when they stop the device
 *    enters its safe state and releases every key it is holding.
 *
 * ## The dead-man switch
 *
 * The operator holds the **right mouse button** while they drive the
 * remote machine.  Releasing it tells the device to stop.  The button
 * is reported as an ordinary button event, and the device's watchdog
 * gives it the meaning (plan G6).
 *
 * ## Usage
 *
 * ```
 *   rd_viewer                       # offer on stdin/stdout
 *   rd_viewer --sink d3d11          # pick the renderer
 *   rd_viewer --codec av1           # pick the codec
 *   rd_viewer --duration 120        # stop after two minutes
 *   rd_viewer --no-input            # watch only; do not send any input
 *   rd_viewer --bind-ref-frame      # bind the events to the displayed frame
 * ```
 *
 * ## Threading
 *
 * One thread, exactly like the device: the engine's tick loop drives
 * the transport, the decoder, the renderer and the heartbeat, and the
 * local capture is polled from the same loop.
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/control_channel.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/rd_input/coalescer.hpp>
#include <nimrtc/rd_input/deadman_watchdog.hpp>
#include <nimrtc/rd_input/event_codec.hpp>
#include <nimrtc/rd_input/input_event.hpp>
#include <nimrtc/rd_input/ref_frame_binder.hpp>
#include <nimrtc/rd_input/safety_filter.hpp>
#include <nimrtc/rd_qoe/qoe_metrics.hpp>

#include "rd_local_input.hpp"
#include "rd_signalling.hpp"

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;
namespace rdi = nimrtc::rd_input;
namespace rq = nimrtc::rd_qoe;

// ---------------------------------------------------------------------------
// The configuration
// ---------------------------------------------------------------------------

struct Options {
    /** The video sink's plugin id. */
    std::string sink_name = "headless";
    /** The codec's plugin id. */
    std::string codec_name = "h264";
    /** The codec backend class: "auto" / "hardware" / "software". */
    std::string codec_backend = "auto";
    /** The control channel's plugin id ("" = no control plane). */
    std::string control_channel_name = "control-channel";
    /** Whether the operator's input is captured and sent. */
    bool send_input = true;
    /** Whether the events carry the ref-frame id (plan G5). */
    bool bind_ref_frame = false;
    /** Whether the QoE metrics are collected (plan J3 / J6). */
    bool qoe = true;
    /** How long the session runs, in seconds (0 = forever). */
    int duration_sec = 0;
    /** The STUN server ("" = none). */
    std::string stun_host = "stun.l.google.com";
    std::uint16_t stun_port = 19302;
    /** The TURN server. */
    std::string turn_host;
    std::uint16_t turn_port = 3478;
    std::string turn_user;
    std::string turn_pass;
    /** The display rate the pointer is coalesced to (plan G4). */
    std::uint32_t display_hz = 60;
};

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --sink <id>           Video sink: headless | d3d11\n"
        "  --codec <id>          Video codec (default: h264; also: av1, h265)\n"
        "  --backend <class>     Codec backend: auto | hardware | software\n"
        "  --duration <sec>      Stop after N seconds (0 = forever)\n"
        "  --display-hz <hz>     The display rate the pointer is coalesced to\n"
        "  --bind-ref-frame      Bind the input events to the displayed frame\n"
        "  --no-qoe              Do not collect the QoE metrics\n"
        "  --stun <host[:port]>  STUN server (default: stun.l.google.com:19302)\n"
        "  --no-stun             Disable STUN gathering\n"
        "  --turn <host[:port]>  TURN server\n"
        "  --turn-user <u>       TURN username\n"
        "  --turn-pass <p>       TURN password\n"
        "  --no-input            Watch only; do not send any input\n"
        "  --no-control          Do not open a control channel\n",
        prog);
}

/** Parse "host" or "host:port". */
void split_host_port(std::string_view text, std::string* host,
                     std::uint16_t* port) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        host->assign(text);
        return;
    }
    host->assign(text.substr(0, colon));
    *port = static_cast<std::uint16_t>(
        std::atoi(std::string(text.substr(colon + 1)).c_str()));
}

/** The host's monotonic clock, in microseconds. */
std::uint64_t now_us() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// The viewer session
// ---------------------------------------------------------------------------

class ViewerSession {
public:
    explicit ViewerSession(const Options& opts) : opts_(opts) {}

    bool start();
    int run();

private:
    void emit_candidates();
    void handle_signal(const std::string& line);

    /** Send the events the coalescer produced. */
    void flush_input(std::uint64_t now_us_value);

    /** Feed one displayed frame to the QoE collector (plan J3 / J6). */
    void feed_presented_frame(const nimrtc::engine::DisplayedFrameInfo& info);

    Options opts_;
    std::unique_ptr<NimRTCEngine> engine_;
    std::unique_ptr<rd_local::LocalInputCapture> capture_;
    std::unique_ptr<rdi::InputCoalescer> coalescer_;
    std::unique_ptr<rdi::RefFrameBinder> binder_;
    std::unique_ptr<rdi::HeartbeatEmitter> heartbeat_;
    std::unique_ptr<rdi::SafetyFilter> filter_;

    // ---- The QoE feed (plan J3 / J6) ----------------------------------------
    //
    // The collector is fed, never pulls: the demo owns one and pushes the
    // observations itself (the engine is not wired to a collector name).
    // The pending deque mirrors the engine QoeSeam's bookkeeping — an
    // input event waits until the frame that reflects it is presented.
    std::unique_ptr<rq::QoeCollector> qoe_;

    /** One input event the control plane accepted, still waiting for the
     *  frame that reflects it. */
    struct PendingInput {
        std::int64_t  sent_ts_us;
        std::uint64_t event_id;
    };
    std::deque<PendingInput> pending_inputs_;

    /** The `frame_seq` of the last presented frame the QoE feed saw. */
    std::uint64_t last_frame_seq_ = 0;
    bool          have_frame_seq_ = false;

    bool offered_ = false;
    bool candidates_sent_ = false;
    bool running_ = true;
    std::uint64_t events_sent_ = 0;
    std::uint64_t events_denied_ = 0;
    std::uint64_t frames_received_ = 0;
};

// ---------------------------------------------------------------------------

bool ViewerSession::start() {
    EngineConfig cfg;

    // ---- The receive path -------------------------------------------------
    //
    // The viewer *receives* the screen: the device sends it.  The sink is
    // the renderer; "headless" decodes without presenting anything, which
    // is what an operator who only drives the machine wants.
    cfg.video_sink_name     = opts_.sink_name;
    cfg.video_receiver_name = "reference";
    cfg.video_codec_name    = opts_.codec_name;
    cfg.video_codec_backend = opts_.codec_backend;

    // The device encodes screen content, so the viewer expects the same
    // kind of picture (plan B7 / RD-3).  The hint is symmetric: it also
    // reaches the peer through the SDP's `a=content-hint` attribute.
    cfg.video_content_hint = np::VideoEncoderParams::ContentHint::kDetail;

    // ---- The control plane (plan G1) ---------------------------------------
    if (!opts_.control_channel_name.empty()) {
        cfg.control_channel_name = opts_.control_channel_name;
    }

    // ---- The QoE metrics (plan J3 / J6) ------------------------------------
    //
    // The collector is **fed** — it never pulls from the engine.  This
    // program owns one and pushes the observations itself (see `run()`);
    // the engine is deliberately left without a collector name, so the
    // two paths cannot double-count.  The engine is only there for the
    // raw events (`last_displayed_frame()`, the control-channel
    // acceptance).
    if (opts_.qoe) {
        qoe_ = std::make_unique<rq::QoeCollector>();
    }

    // ---- The transport -----------------------------------------------------
    if (opts_.stun_host.empty()) {
        cfg.stun_server_host.clear();
        cfg.stun_server_port = 0;
    } else {
        cfg.stun_server_host = opts_.stun_host;
        cfg.stun_server_port = opts_.stun_port;
    }

    engine_ = std::make_unique<NimRTCEngine>(cfg);

    engine_->set_on_state_change([](const char* s) {
        std::fprintf(stderr, "[rd_viewer] state: %s\n", s);
    });
    engine_->set_on_error([](std::uint32_t e, std::string_view m) {
        std::fprintf(stderr, "[rd_viewer] error 0x%04X: %.*s\n", e,
                     static_cast<int>(m.size()), m.data());
    });

    engine_->pre_open();
    if (!opts_.turn_host.empty()) {
        const int rc = engine_->add_turn_server(
            opts_.turn_host, opts_.turn_port, opts_.turn_user, opts_.turn_pass);
        if (rc != 0) {
            std::fprintf(stderr, "[rd_viewer] add_turn_server failed\n");
            return false;
        }
    }

    if (engine_->open() != 0) {
        std::fprintf(stderr,
                     "[rd_viewer] engine open failed (rc=0x%04X, state=%s)\n",
                     engine_->last_open_rc(), engine_->ice_state_string());
        return false;
    }

    // ---- The frame counter -------------------------------------------------
    //
    // The callback fires on the NAL level (before the decode), so it
    // counts the *frames* that arrived, which is what the operator wants
    // to know: "is the screen still coming?"
    engine_->set_on_video_frame([this](const std::uint8_t*, std::size_t, bool) {
        ++frames_received_;
    });

    // ---- The heartbeat (plan G6) --------------------------------------------
    //
    // The beat is the device's proof that the operator is still there.
    // It is a one-byte message the engine recognises (and counts) by
    // itself — the host only has to send it on time.  The emitter is
    // created even when the input capture is off: a watch-only session
    // still has to keep the device's watchdog quiet.
    heartbeat_ = std::make_unique<rdi::HeartbeatEmitter>(250);

    // ---- The local input capture -------------------------------------------
    if (opts_.send_input) {
        capture_ = std::make_unique<rd_local::LocalInputCapture>();
        if (!rd_local::LocalInputCapture::supported()) {
            std::fprintf(stderr,
                         "[rd_viewer] this platform has no local input "
                         "capture — the session is watch-only\n");
        } else {
            std::fprintf(stderr, "[rd_viewer] local input capture: %s\n",
                         rd_local::LocalInputCapture::backend_name().c_str());
        }

        // The coalescer (plan G4): the pointer is coalesced to the
        // display rate, and the key events are never dropped.
        rdi::CoalescerConfig cc;
        cc.display_hz = opts_.display_hz;
        cc.max_pointer_events_per_second = opts_.display_hz * 2;
        coalescer_ = std::make_unique<rdi::InputCoalescer>(cc);

        // The frame binding (plan G5): every outgoing event names the
        // frame the operator was looking at when they produced it.
        binder_ = std::make_unique<rdi::RefFrameBinder>(
            [this] { return engine_->last_displayed_ref_frame_id(); });

        // The safety filter (plan G7): the *sending* side is where the
        // filter belongs, so a denied key never reaches the device at all.
        rdi::SafetyFilterConfig fc;
        fc.denied_keys.insert(rdi::keys::kMeta);
        filter_ = std::make_unique<rdi::SafetyFilter>(fc);
    }

    std::fprintf(stderr, "[rd_viewer] ready (sink=%s codec=%s control=%s)\n",
                 cfg.video_sink_name.c_str(), cfg.video_codec_name.c_str(),
                 cfg.control_channel_name.empty() ? "off"
                                                  : cfg.control_channel_name.c_str());
    return true;
}

void ViewerSession::emit_candidates() {
    if (candidates_sent_) return;
    auto* ice = engine_->get_ice_transport();
    if (ice == nullptr) return;
    candidates_sent_ = true;
    for (const auto& c : ice->gathered_local_candidates()) {
        // The candidate lines are forwarded verbatim (see demo-p2p).
        rd_sig::emit_candidate(c, 0, "0");
    }
}

void ViewerSession::handle_signal(const std::string& line) {
    std::string type;
    if (!rd_sig::json_get_string(line, "type", &type)) {
        std::fprintf(stderr, "[rd_viewer] malformed signalling line\n");
        return;
    }

    if (type == "answer") {
        std::string sdp;
        if (!rd_sig::json_get_string(line, "sdp", &sdp)) return;

        // The answer commits the DTLS roles, so the engine adopts the
        // role the answer implies and produces no SDP of its own — the
        // returned optional holds an *empty* string in that case, which
        // is still a success.
        const auto result = engine_->process_remote_sdp(sdp);
        if (!result.has_value()) {
            std::fprintf(stderr, "[rd_viewer] process_remote_sdp(answer) failed\n");
            return;
        }
        std::fprintf(stderr, "[rd_viewer] the device answered\n");
        return;
    }

    if (type == "candidate") {
        std::string cand;
        if (!rd_sig::json_get_string(line, "candidate", &cand)) return;
        auto* ice = engine_->get_ice_transport();
        if (ice == nullptr) return;
        // `add_remote_candidate()` accepts both the "a=candidate:" and the
        // bare "candidate:" forms.
        ice->add_remote_candidate(cand);
        return;
    }

    if (type == "bye") {
        std::fprintf(stderr, "[rd_viewer] the device said goodbye\n");
        running_ = false;
        return;
    }

    if (type == "offer") {
        // The device re-offered: an ICE restart.  The viewer answers it
        // (the engine returns the answer string) and re-arms the
        // candidate trickle.
        std::string sdp;
        if (!rd_sig::json_get_string(line, "sdp", &sdp)) return;
        const auto answer = engine_->process_remote_sdp(sdp);
        if (answer.has_value() && !answer->empty()) {
            rd_sig::emit_sdp("answer", *answer);
        }
        candidates_sent_ = false;
        emit_candidates();
        return;
    }

    std::fprintf(stderr, "[rd_viewer] unknown signalling type '%s'\n",
                  type.c_str());
}

void ViewerSession::flush_input(std::uint64_t now_us_value) {
    if (!coalescer_ || !capture_ || !filter_ || !binder_) return;

    // ---- The capture ------------------------------------------------------
    const auto captured = capture_->poll(now_us_value);

    // ---- The safety filter ------------------------------------------------
    //
    // The filter runs *before* the coalescer, so a denied event never
    // even reaches the wire.  `apply()` clamps the axes in place and
    // reports the verdict.
    for (auto event : captured) {
        rdi::FilterResult verdict;
        if (!filter_->apply(event, verdict)) {
            ++events_denied_;
            continue;
        }
        coalescer_->push(event, now_us_value);
    }

    // ---- The frame binding (plan G5) ---------------------------------------
    //
    // The binding is applied to the *outgoing* event, so the id names the
    // frame that was on the operator's screen when they produced it.
    const auto drained = coalescer_->drain(now_us_value);
    if (drained.empty()) return;

    std::vector<rdi::InputEvent> bound;
    bound.reserve(drained.size());
    for (const auto& event : drained) {
        // `stamped()` takes the event by value and returns the stamped
        // copy, so the drained batch stays untouched.
        bound.push_back(binder_->stamped(event));
    }

    rdi::EncodeOptions encode_opts;
    encode_opts.include_timestamp = false;

    // The ref-frame id is only put on the wire when the operator asked
    // for it.  The event codec's version-1 rules are explicit about it:
    // an *old* decoder cannot skip a field it does not know, so a peer
    // that predates the extension must not be sent an event that
    // carries the id.  The two NimRTC ends agree, so the flag is safe
    // here — but it is opt-in rather than the default.
    encode_opts.include_ref_frame = opts_.bind_ref_frame;

    const auto encoded = rdi::encode_batch(bound, encode_opts);
    if (!encoded.ok()) {
        std::fprintf(stderr, "[rd_viewer] the input batch could not be encoded\n");
        return;
    }
    const std::vector<std::uint8_t>& bytes = encoded.value();

    // ---- The send ----------------------------------------------------------
    //
    // `send_input_event()` is `send_control_message()` plus the QoE
    // stamp: the engine pairs the event with the frame that reflects it,
    // which is the input-to-photon latency (plan J6).  Without a
    // collector it is exactly the plain send.
    //
    // The event id is the ref-frame id the operator acted on (0 when the
    // event is not bound to a frame).
    std::uint64_t event_id = 0;
    if (!bound.empty() && bound.front().ref_frame.has_value()) {
        event_id = *bound.front().ref_frame;
    }

    const np::Status rc = engine_->send_input_event(
        std::span<const std::uint8_t>(bytes.data(), bytes.size()), event_id);
    if (rc != np::kOk) {
        std::fprintf(stderr,
                     "[rd_viewer] the control message was refused (0x%04X)\n",
                     rc);
        return;
    }
    events_sent_ += bound.size();

    // ---- The QoE feed (plan J6) --------------------------------------------
    //
    // The event was **accepted by the control plane** — not merely
    // constructed — so this is the honest instant the input-to-photon
    // latency is measured from (spec requirement 2: the queueing and the
    // wire delay are included in the number).  The event is remembered
    // until the frame that reflects it is presented (see `run()`).
    // `event_id` is 0 for an event that was not bound to a frame.
    if (qoe_) {
        const auto sent_ts_us = static_cast<std::int64_t>(now_us_value);
        qoe_->on_input_event_sent(sent_ts_us, event_id);
        pending_inputs_.push_back(PendingInput{sent_ts_us, event_id});
    }
}

void ViewerSession::feed_presented_frame(
    const nimrtc::engine::DisplayedFrameInfo& info) {
    // ---- The end-to-end latency and the presented frame rate --------------
    //
    // The capture timestamp is the ADR-017 value the engine resolved for
    // this frame (spec requirement 1: real capture and render instants,
    // never a fake 0).  When the pipeline did not carry one the engine
    // reports `capture_ts_us == 0`, and the collector counts the frame as
    // presented but feeds **no** latency sample for it (spec requirement
    // 6) — a frame without a capture instant has no honest latency.
    rq::FrameObservation observation;
    observation.capture_ts_us = info.capture_ts_us;
    observation.render_ts_us  = info.rendered_at_us;
    observation.frame_id      = info.ref_frame_id;
    qoe_->on_frame_presented(observation);

    // ---- The input-to-photon pairing (plan item J6) -----------------------
    //
    // This mirrors the engine QoeSeam's rule, so the demo's numbers agree
    // with the engine's own: an input event is reflected by the first
    // presented frame whose ref-frame id is **strictly greater** than the
    // event's (the operator acted on the frame they were looking at, so
    // the frame that reflects the action is a later one).  An event that
    // was not bound to a frame (id 0) pairs with the next presented frame,
    // whatever its id.  The pairings resolve in send order.
    const std::int64_t render_ts_us = info.rendered_at_us;
    for (auto it = pending_inputs_.begin(); it != pending_inputs_.end();) {
        const bool reflects =
            it->event_id == 0 || info.ref_frame_id > it->event_id;
        if (!reflects) {
            ++it;
            continue;
        }
        qoe_->on_frame_presented_for_input(render_ts_us, it->event_id);
        it = pending_inputs_.erase(it);
    }
}

int ViewerSession::run() {
    auto* ice = engine_->get_ice_transport();
    if (ice != nullptr) {
        // How long to wait for the candidate gathering to settle before
        // the offer goes out.
        //
        // With a STUN server the gathering has to survive the server's
        // retransmit schedule (libjuice does three attempts eight
        // seconds apart), so two seconds is not enough: the offer would
        // carry a candidate set that is still changing, and the peer
        // would answer against a stale one.  A LAN run has no STUN at
        // all, so the host candidates are ready almost immediately and
        // a long wait would only delay the session.
        const int gather_ms = opts_.stun_host.empty() ? 500 : 30'000;
        ice->wait_for_gathering(gather_ms);
    }

    // ---- The offer ---------------------------------------------------------
    //
    // The viewer is the offerer (the guide's §2.1): the operator's client
    // is the side that opens the outgoing path.
    const std::string offer = engine_->create_offer();
    if (offer.empty()) {
        std::fprintf(stderr, "[rd_viewer] create_offer failed\n");
        return 2;
    }
    rd_sig::emit_sdp("offer", offer);
    offered_ = true;
    emit_candidates();

    const bool finite_session = opts_.duration_sec > 0;
    // `(expr::max)()` — the parentheses dodge the `max` macro that
    // <windows.h> defines (rd_signalling.hpp pulls it in on Windows).
    std::chrono::steady_clock::time_point deadline =
        (std::chrono::steady_clock::time_point::max)();
    if (finite_session) {
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::seconds(static_cast<long long>(opts_.duration_sec));
    }

    std::string line;
    std::uint64_t last_report_us = now_us();

    while (running_ && (std::chrono::steady_clock::now() < deadline)) {
        // ---- The engine ---------------------------------------------------
        engine_->tick();

        // ---- The QoE feed (plan J3 / J6) ------------------------------------
        //
        // The frame-presented events are **polled**, not pushed: the
        // engine records the last displayed frame (plan E9), and this loop
        // reads it once per tick.  The `frame_seq` gate feeds each
        // presented frame exactly once — the collector sees every frame the
        // viewer saw, with the real capture and render instants (spec
        // requirement 1), and the frames that reflect a pending input event
        // are paired (spec requirement 3).  The feed runs *before* the
        // local input flush, so an event sent in this tick pairs with a
        // frame presented in a later tick, never with a frame that was
        // already on the screen when the event went out.
        if (qoe_) {
            const auto displayed = engine_->last_displayed_frame();
            if (displayed.has_value() &&
                (!have_frame_seq_ || displayed->frame_seq != last_frame_seq_)) {
                have_frame_seq_ = true;
                last_frame_seq_ = displayed->frame_seq;
                feed_presented_frame(*displayed);
            }
        }

        // ---- The local input ------------------------------------------------
        const std::uint64_t t = now_us();
        flush_input(t);

        // ---- The heartbeat (plan G6) ----------------------------------------
        //
        // The beat is the device's proof that the operator is still there.
        // It is a one-byte message the engine recognises (and counts) by
        // itself — the host only has to send it on time.
        if (heartbeat_ && heartbeat_->due(t)) {
            if (engine_->send_control_heartbeat() == np::kOk) {
                heartbeat_->on_sent(t);
            }
        }

        // ---- The signalling channel -----------------------------------------
        //
        // The read is non-blocking (a zero timeout), so the tick loop
        // keeps running at its own rate.
        //
        // A *closed* channel is different from an empty one: once the
        // client that drives this binary is gone there is nobody left to
        // send the offer, so the session can never come up and there is
        // no point in decoding into the void.
        if (rd_sig::channel_closed()) {
            std::fprintf(stderr,
                         "[rd_viewer] the signalling channel closed - stopping\n");
            running_ = false;
            break;
        }
        if (rd_sig::read_line(&line, 0)) {
            if (!line.empty()) {
                handle_signal(line);
            }
        }

        // ---- The periodic report ---------------------------------------------
        if (t - last_report_us >= 5'000'000ull) {
            last_report_us = t;
            const auto stats = engine_->stats();
            const auto control = engine_->control_channel_stats();
            std::fprintf(stderr,
                         "[rd_viewer] frames=%llu decoded=%llu rendered=%llu "
                         "dropped=%llu events_sent=%llu denied=%llu beats=%llu "
                         "ctrl_out=%llu\n",
                         static_cast<unsigned long long>(frames_received_),
                         static_cast<unsigned long long>(stats.video_frames_decoded),
                         static_cast<unsigned long long>(stats.video_frames_rendered),
                         static_cast<unsigned long long>(
                             stats.video_frames_dropped_decoder),
                         static_cast<unsigned long long>(events_sent_),
                         static_cast<unsigned long long>(events_denied_),
                         static_cast<unsigned long long>(
                             heartbeat_ ? heartbeat_->beats_sent() : 0),
                         static_cast<unsigned long long>(control.messages_sent));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    engine_->close();
    return offered_ ? 0 : 1;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

} // namespace

int main(int argc, char** argv) {
    Options opts;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = (i + 1 < argc);
        if (arg == "--sink" && has_next) {
            opts.sink_name = argv[++i];
        } else if (arg == "--codec" && has_next) {
            opts.codec_name = argv[++i];
        } else if (arg == "--backend" && has_next) {
            opts.codec_backend = argv[++i];
        } else if (arg == "--duration" && has_next) {
            opts.duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--display-hz" && has_next) {
            opts.display_hz = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--bind-ref-frame") {
            opts.bind_ref_frame = true;
        } else if (arg == "--no-qoe") {
            opts.qoe = false;
        } else if (arg == "--stun" && has_next) {
            split_host_port(argv[++i], &opts.stun_host, &opts.stun_port);
        } else if (arg == "--no-stun") {
            opts.stun_host.clear();
            opts.stun_port = 0;
        } else if (arg == "--turn" && has_next) {
            split_host_port(argv[++i], &opts.turn_host, &opts.turn_port);
        } else if (arg == "--turn-user" && has_next) {
            opts.turn_user = argv[++i];
        } else if (arg == "--turn-pass" && has_next) {
            opts.turn_pass = argv[++i];
        } else if (arg == "--no-input") {
            opts.send_input = false;
        } else if (arg == "--no-control") {
            opts.control_channel_name.clear();
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "[rd_viewer] unknown argument '%.*s'\n",
                         static_cast<int>(arg.size()), arg.data());
            print_usage(argv[0]);
            return 64;
        }
    }

    ViewerSession session(opts);
    if (!session.start()) return 1;
    return session.run();
}
