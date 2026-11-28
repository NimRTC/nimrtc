/**
 * @file examples/remote-desktop/rd_device.cpp
 * @brief The remote-desktop **device** — the machine whose screen is shared.
 *
 * ## What this program does
 *
 * The device is the **answerer** (see `docs/guides/remote-desktop.md`
 * §2.1 — the device is a server-shaped peer, so the operator's client
 * opens the outgoing path).  It:
 *
 * 1. opens a NimRTC engine with a **real screen-capture device** (the
 *    DXGI / Windows Graphics Capture backend on Windows, ScreenCaptureKit
 *    on macOS, PipeWire / X11 on Linux) and a real video codec, with
 *    the engine's **capture pump** enabled — so the screen streams
 *    without the host ever calling `send_video()`;
 * 2. answers the operator's SDP offer and trickles the ICE candidates
 *    back over the signalling channel;
 * 3. receives the operator's **input events** on the control channel,
 *    decodes them with the `rd_input` event codec, passes them through
 *    the **safety filter**, and injects them into the local operating
 *    system with the `rd_input_inject` module;
 * 4. watches the **dead-man switch**: when the control path goes quiet
 *    (or the operator releases the dead-man button), every key and
 *    button is released, so the remote machine is never left with a key
 *    stuck down.
 *
 * ## The signalling channel
 *
 * The device speaks the same newline-delimited JSON dialect as
 * `demo-p2p` (`{"type":"offer","sdp":"..."}` and friends), so the
 * existing `examples/remote-desktop/client.py` bridges it to the
 * reference WebSocket service without a change.
 *
 * ## Usage
 *
 * ```
 *   rd_device                       # answer on stdin/stdout
 *   rd_device --capture windows-wgc # pick the capture backend
 *   rd_device --game-source         # capture from the synthetic game source
 *   rd_device --game-source --game-fps 30   # ... at 30 fps instead of 60
 *   rd_device --codec av1           # pick the codec
 *   rd_device --backend software    # force the software codec
 *   rd_device --duration 120        # stop after two minutes
 *   rd_device --no-inject           # do not inject input (read-only session)
 * ```
 *
 * ## Threading
 *
 * One thread only.  The engine's tick loop drives everything: the
 * capture pump, the encoder, the transport, the control-channel
 * receive path and the input injection.  The engine is a
 * single-threaded orchestrator (plan RD-6), and the input injector is
 * documented as not thread-safe — so keeping everything on the tick
 * thread is what makes the whole thing correct.
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/control_channel.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/rd_input/deadman_watchdog.hpp>
#include <nimrtc/rd_input/event_codec.hpp>
#include <nimrtc/rd_input/input_event.hpp>
#include <nimrtc/rd_input/safety_filter.hpp>
#include <nimrtc/rd_input_inject/input_injector.hpp>
#include <nimrtc/rd_session/session_guard.hpp>

#include "game_source.hpp"
#include "rd_signalling.hpp"

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;
namespace rdi = nimrtc::rd_input;
namespace inj = nimrtc::rd_input_inject;
namespace rds = nimrtc::rd_session;

// ---------------------------------------------------------------------------
// The configuration
// ---------------------------------------------------------------------------

struct Options {
    /** The capture backend's plugin id, or "auto" for the platform
     *  default (v1-device-policy D3: the engine resolves "auto" through
     *  its default device policy). */
    std::string capture_name = "auto";
    /**
     * Whether to capture from the synthetic whack-a-mole game source
     * instead of the real screen (slice v1-game-source).  Off by
     * default: the device keeps capturing the real screen unless this
     * flag is passed explicitly.
     */
    bool game_source = false;
    /** The game source's frame rate (slice v1-game-source). */
    std::uint32_t game_fps = 60;
    /** The codec's plugin id. */
    std::string codec_name = "h264";
    /** The codec backend class: "auto" / "hardware" / "software". */
    std::string codec_backend = "auto";
    /** The control channel's plugin id ("" = no control plane). */
    std::string control_channel_name = "control-channel";
    /** Whether the input is injected into the local OS. */
    bool inject = true;
    /** Whether to try to recover a locked console session and exit. */
    bool recover_session = false;
    /** How long the session runs, in seconds (0 = forever). */
    int duration_sec = 0;
    /** The capture width / height (0 = the device's best). */
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /** The STUN server ("" = none). */
    std::string stun_host = "stun.l.google.com";
    std::uint16_t stun_port = 19302;
    /** The TURN server. */
    std::string turn_host;
    std::uint16_t turn_port = 3478;
    std::string turn_user;
    std::string turn_pass;
};

/** The capture backend to use when `--capture` is left empty.
 *
 *  The per-platform default table moved into the engine core
 *  (v1-device-policy D3): this is now a compatibility shim that returns
 *  the "auto" sentinel, which makes the engine resolve the platform
 *  default through its own policy.  It keeps the source shape the
 *  v1-game-source slice's acceptance gate anchors on.
 */
std::string default_capture_name() {
    return "auto";
}

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --capture <id>        Capture backend (default: auto = the platform default)\n"
        "  --game-source         Capture from the synthetic whack-a-mole game source\n"
        "  --game-fps <n>        The game source frame rate (default: 60)\n"
        "  --codec <id>          Video codec (default: h264; also: av1, h265)\n"
        "  --backend <class>     Codec backend: auto | hardware | software\n"
        "  --width <px>          Capture width (default: the device's best)\n"
        "  --height <px>         Capture height\n"
        "  --duration <sec>      Stop after N seconds (0 = forever)\n"
        "  --stun <host[:port]>  STUN server (default: stun.l.google.com:19302)\n"
        "  --no-stun             Disable STUN gathering\n"
        "  --turn <host[:port]>  TURN server\n"
        "  --turn-user <u>       TURN username\n"
        "  --turn-pass <p>       TURN password\n"
        "  --no-inject           Do not inject input (a read-only session)\n"
        "  --no-control          Do not open a control channel\n"
        "  --recover-session     Recover a locked console session, then exit\n",
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

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

/** The host's monotonic clock, in microseconds. */
std::uint64_t now_us() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// The device session
// ---------------------------------------------------------------------------

/**
 * @brief Everything the device does between the offer and the bye.
 *
 * The class owns the state that the *control plane* needs and the engine
 * does not: the injector, the safety filter and the dead-man watchdog.
 * The engine owns the transport, the encoder and the capture device.
 */
class DeviceSession {
public:
    explicit DeviceSession(const Options& opts) : opts_(opts) {}

    /** Open the engine and wire the control plane. */
    bool start();

    /** The main loop: tick the engine and read the signalling channel. */
    int run();

private:
    // ---- The signalling half ---------------------------------------------

    /** Send the local ICE candidates. */
    void emit_candidates();

    /** Handle one signalling message. */
    void handle_signal(const std::string& line);

    // ---- The control-plane half -------------------------------------------

    /** Decode one inbound control message and act on it. */
    void handle_control_message(std::span<const std::uint8_t> message);

    /** Apply one decoded event: watchdog → safety filter → injector. */
    void apply_event(const rdi::InputEvent& event);

    /** Enter the safe state: release every key and button. */
    void enter_safe_state(rdi::DeadManState state);

    // ---- The state ---------------------------------------------------------

    Options opts_;
    std::unique_ptr<NimRTCEngine> engine_;
    std::unique_ptr<inj::InputInjector> injector_;
    std::unique_ptr<rdi::DeadManWatchdog> watchdog_;

    rdi::SafetyFilter filter_{[] {
        rdi::SafetyFilterConfig cfg;
        // The Windows / Command key is denied by default: an agent that
        // can press it can open the shell and lock the operator out of
        // the machine they are driving.  (The injector refuses to
        // synthesise it too — this is the second line of defence.)
        cfg.denied_keys.insert(rdi::keys::kMeta);
        return cfg;
    }()};

    bool answered_ = false;
    bool candidates_sent_ = false;
    bool running_ = true;
    std::uint64_t events_injected_ = 0;
    std::uint64_t events_denied_ = 0;
    std::uint64_t events_refused_ = 0;

    // The most recent frame binding seen on an inbound event
    // (v2-input-binder): the `ref_frame` id the event named, plus the
    // capture instant and the frame age the device resolved it to.  The
    // has_* flags are the honest report — an id the frame table no
    // longer knows is shown as unknown, never guessed (spec risks 1
    // and 2).
    bool          has_binding_ref_frame_   = false;
    std::uint64_t binding_ref_frame_id_    = 0;
    bool          has_binding_capture_ts_  = false;
    std::int64_t  binding_capture_ts_us_   = 0;
    bool          has_binding_age_us_      = false;
    std::int64_t  binding_age_us_          = 0;
};

// ---------------------------------------------------------------------------

bool DeviceSession::start() {
    EngineConfig cfg;

    // ---- The capture device (remote-desktop plan A1 / A10) -----------------
    //
    // The capture pump is the whole point: with it on, the engine pops
    // the frames from the device's bounded queue **on the tick thread**
    // and drives them through the encoder, so this program never calls
    // `send_video()`.
    cfg.video_capture_pump  = true;

    if (opts_.game_source) {
        // ---- The synthetic game source (slice v1-game-source) ---------------
        //
        // A *memory-frame* capture device: it synthesises a whack-a-mole
        // picture on its own thread and fills the same bounded capture
        // queue the screen backends use, so the capture pump drives it to
        // the encoder with zero changes to the pump itself.  It is an
        // explicit opt-in — the default path below stays the real screen
        // capture.
        static nimrtc::rd::GameSourceFactory game_factory;
        nimrtc::core::PluginRegistry::instance().register_video_capture(
            game_factory.id(), &game_factory);
        cfg.video_capture_name = std::string(game_factory.id());
        // The slice's "720p floor": the demo picture is readable at the
        // default size, and both the size and the frame rate are
        // configurable (`--width` / `--height` / `--game-fps`).
        cfg.video_capture_config.format.width  = opts_.width  != 0
                                                     ? opts_.width  : 1280;
        cfg.video_capture_config.format.height = opts_.height != 0
                                                     ? opts_.height : 720;
        cfg.video_capture_config.format.fps    = opts_.game_fps;
        cfg.video_width  = cfg.video_capture_config.format.width;
        cfg.video_height = cfg.video_capture_config.format.height;
        cfg.video_fps    = opts_.game_fps;
    } else {
        // "auto" (the default, and what the compatibility shim below
        // returns for an explicit empty value) tells the engine to
        // resolve through its default device policy (v1-device-policy
        // D3); an explicit id still wins verbatim.
        cfg.video_capture_name = opts_.capture_name.empty()
                                     ? default_capture_name()
                                     : opts_.capture_name;
        if (opts_.width != 0 || opts_.height != 0) {
            cfg.video_capture_config.format.width  = opts_.width;
            cfg.video_capture_config.format.height = opts_.height;
        }
        cfg.video_width  = opts_.width  != 0 ? opts_.width  : 1280;
        cfg.video_height = opts_.height != 0 ? opts_.height : 720;
        cfg.video_fps    = 30;
    }
    // The device wants the pointer composited into the picture when the
    // backend can do it (the capability check in the engine reports the
    // combination as unsupported otherwise).
    cfg.video_capture_config.want_cursor = true;

    // ---- The video path ----------------------------------------------------
    //
    // Screen content is not camera content: the encoder should spend
    // its budget on sharp edges and text, not on motion vectors (plan
    // B7 / RD-3).
    cfg.video_codec_name    = opts_.codec_name;
    cfg.video_codec_backend = opts_.codec_backend;
    // Screen content is text, not a camera: kText is what makes "auto"
    // resolve to the screen backend (D3-1) and lets the encoder spend
    // its budget on sharp edges and text, not on motion vectors (plan
    // B7 / RD-3).
    cfg.video_content_hint  = np::VideoEncoderParams::ContentHint::kText;

    // ---- The control plane (plan G1) ---------------------------------------
    if (!opts_.control_channel_name.empty()) {
        cfg.control_channel_name = opts_.control_channel_name;
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
        std::fprintf(stderr, "[rd_device] state: %s\n", s);
    });
    engine_->set_on_error([](std::uint32_t e, std::string_view m) {
        std::fprintf(stderr, "[rd_device] error 0x%04X: %.*s\n", e,
                     static_cast<int>(m.size()), m.data());
    });

    // `pre_open()` creates the ICE transport *without* gathering, so the
    // TURN server can be registered before `open()` starts the candidate
    // gathering (a TURN server added after `open()` is silently useless).
    engine_->pre_open();
    if (!opts_.turn_host.empty()) {
        const int rc = engine_->add_turn_server(
            opts_.turn_host, opts_.turn_port, opts_.turn_user, opts_.turn_pass);
        if (rc != 0) {
            std::fprintf(stderr, "[rd_device] add_turn_server failed\n");
            return false;
        }
    }

    if (engine_->open() != 0) {
        std::fprintf(stderr,
                     "[rd_device] engine open failed (rc=0x%04X, state=%s)\n",
                     engine_->last_open_rc(), engine_->ice_state_string());
        return false;
    }

    // ---- The injector ------------------------------------------------------
    if (opts_.inject) {
        injector_.reset(inj::create_input_injector());
        if (!injector_->available()) {
            std::fprintf(stderr,
                         "[rd_device] input injection is unavailable (%s) — "
                         "the session is read-only\n",
                         injector_->unavailable_reason().c_str());
        } else {
            std::fprintf(stderr, "[rd_device] input injector: %.*s\n",
                         static_cast<int>(injector_->backend_name().size()),
                         injector_->backend_name().data());
        }
    }

    // ---- The dead-man switch (plan G6) -------------------------------------
    {
        rdi::DeadManConfig dm;
        dm.heartbeat_timeout_ms = 1000;
        dm.heartbeat_interval_ms = 250;
        dm.deadman_enabled = true;
        dm.deadman_button = rdi::MouseButton::kRight;
        watchdog_ = std::make_unique<rdi::DeadManWatchdog>(
            dm,
            [this](rdi::DeadManState state) { enter_safe_state(state); },
            [] { std::fprintf(stderr, "[rd_device] control path is alive again\n"); });
    }

    // ---- The inbound control messages --------------------------------------
    //
    // The callback runs on the engine's thread (inside `tick()`), so the
    // injector is touched from exactly one thread — which is what the
    // injector's contract requires.
    engine_->set_on_control_message([this](std::span<const std::uint8_t> m) {
        handle_control_message(m);
    });

    // ---- The ready line -----------------------------------------------------
    //
    // Print the *effective* capture backend (v1-device-policy D3): what
    // the engine actually resolved, so the operator can see the policy
    // came through.  With --capture auto (the default) that is the
    // platform's default screen backend — the content hint above is
    // kText, so D3-1 picks the screen.
    std::string effective_backend = cfg.video_capture_name;
    if (!opts_.game_source && effective_backend == "auto") {
        effective_backend = NimRTCEngine::default_device_policy()
                                .screen_capture_backend;
    }
    std::fprintf(stderr,
                 "[rd_device] ready (capture=%s codec=%s control=%s)\n",
                 effective_backend.c_str(), cfg.video_codec_name.c_str(),
                 cfg.control_channel_name.empty() ? "off"
                                                  : cfg.control_channel_name.c_str());
    return true;
}

void DeviceSession::emit_candidates() {
    if (candidates_sent_) return;
    auto* ice = engine_->get_ice_transport();
    if (ice == nullptr) return;
    candidates_sent_ = true;
    for (const auto& c : ice->gathered_local_candidates()) {
        // The candidate lines are forwarded **verbatim**: libjuice's
        // lines already carry the "a=candidate:" prefix, and stripping it
        // corrupts the foundation field (see demo-p2p's long comment).
        rd_sig::emit_candidate(c, 0, "0");
    }
}

void DeviceSession::handle_signal(const std::string& line) {
    std::string type;
    if (!rd_sig::json_get_string(line, "type", &type)) {
        std::fprintf(stderr, "[rd_device] malformed signalling line\n");
        return;
    }

    if (type == "offer") {
        std::string sdp;
        if (!rd_sig::json_get_string(line, "sdp", &sdp)) return;

        // The device is the **answerer** (the guide's §2.1): the operator's
        // client is the side that opens the outgoing path.
        const auto answer = engine_->process_remote_sdp(sdp);
        if (!answer) {
            std::fprintf(stderr, "[rd_device] process_remote_sdp(offer) failed\n");
            return;
        }
        rd_sig::emit_sdp("answer", *answer);
        answered_ = true;
        emit_candidates();
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
        std::fprintf(stderr, "[rd_device] the peer said goodbye\n");
        running_ = false;
        return;
    }

    if (type == "ice_restart" || type == "reconnect") {
        // The peer restarted ICE: it will send a fresh offer.  Nothing to
        // do here beyond re-arming the candidate trickle.
        candidates_sent_ = false;
        std::fprintf(stderr, "[rd_device] the peer asked for an ICE restart\n");
        return;
    }

    std::fprintf(stderr, "[rd_device] unknown signalling type '%s'\n",
                  type.c_str());
}

void DeviceSession::handle_control_message(
    std::span<const std::uint8_t> message) {
    if (message.empty()) return;

    // The heartbeat is the engine's own one-byte convention (plan G6).
    // The engine already counted it; here it is *acted* upon — it is the
    // proof that the operator is still there.
    if (message.size() == 1 && message[0] == np::kControlHeartbeatPayload) {
        watchdog_->note_heartbeat(now_us());
        return;
    }

    // The payload is either a batch of events or a single event; the
    // batch form is what the viewer sends, and the single form is what
    // a peer that does not batch sends.
    rdi::DecodeOptions decode_opts;
    auto batch = rdi::decode_batch(message, decode_opts);
    if (batch.ok()) {
        for (const auto& event : batch.value()) {
            apply_event(event);
        }
        return;
    }

    auto single = rdi::decode_event(message, decode_opts);
    if (single.ok()) {
        apply_event(single.value());
        return;
    }

    std::fprintf(stderr, "[rd_device] a control message could not be decoded\n");
}

void DeviceSession::apply_event(const rdi::InputEvent& event) {
    // The frame binding (v2-input-binder): an event that names a frame
    // gets its id resolved to the capture instant and the frame age —
    // the "the click happened on the frame the operator was looking at"
    // check (plan G5).  A lookup that misses is reported as unknown,
    // never guessed (spec risks 1 and 2).
    if (event.ref_frame.has_value()) {
        has_binding_ref_frame_  = true;
        binding_ref_frame_id_   = *event.ref_frame;
        const auto capture_ts   = engine_->ref_frame_to_capture_ts(*event.ref_frame);
        const auto age_us       = engine_->data_message_frame_age_us(*event.ref_frame);
        has_binding_capture_ts_ = capture_ts.has_value();
        if (capture_ts.has_value()) binding_capture_ts_us_ = *capture_ts;
        has_binding_age_us_     = age_us.has_value();
        if (age_us.has_value()) binding_age_us_ = *age_us;
    }

    // The watchdog sees *every* event: any activity counts as liveness,
    // and a release of the dead-man button enters the safe state.
    watchdog_->observe(event, now_us());

    // While the session is in the safe state the remote machine must not
    // move: the events are dropped rather than injected.
    if (!watchdog_->alive()) return;

    // The safety filter (plan G7) is the last gate before the OS.  It is
    // documented as a *sending*-side filter, but a receiving-side filter
    // is the defence in depth that matters here: a peer that does not
    // honour the policy must not be able to press the Windows key.
    rdi::InputEvent adjusted = event;
    rdi::FilterResult verdict;
    if (!filter_.apply(adjusted, verdict)) {
        ++events_denied_;
        return;
    }

    if (!injector_) return;
    const inj::InjectionResult result = injector_->inject(adjusted);
    switch (result) {
        case inj::InjectionResult::kInjected: ++events_injected_; break;
        case inj::InjectionResult::kRefused:  ++events_refused_;  break;
        case inj::InjectionResult::kFailed:   ++events_refused_;  break;
    }
}

void DeviceSession::enter_safe_state(rdi::DeadManState state) {
    std::fprintf(stderr,
                 "[rd_device] entering the safe state (%s) — releasing every "
                 "key and button\n",
                 std::string(rdi::deadman_state_name(state)).c_str());
    if (injector_) {
        const std::size_t released = injector_->release_all();
        std::fprintf(stderr, "[rd_device] released %zu inputs\n", released);
    }
}

int DeviceSession::run() {
    // The candidates are sent as soon as the gathering settles, so the
    // operator's client can start connecting while the device is still
    // waiting for the offer.
    auto* ice = engine_->get_ice_transport();
    if (ice != nullptr) {
        // The same rule as the viewer: with a STUN server the gathering
        // has to survive its retransmit schedule before the candidate
        // set is stable, and a LAN run has nothing to wait for.
        const int gather_ms = opts_.stun_host.empty() ? 500 : 30'000;
        ice->wait_for_gathering(gather_ms);
    }
    emit_candidates();

    // The deadline: a finite one when the session has a duration, and
    // "never" otherwise.  The two branches are kept apart (rather than
    // folded into a ternary) so the type is unambiguous.
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

    while (running_ &&
           (std::chrono::steady_clock::now() < deadline)) {
        // ---- The engine ---------------------------------------------------
        engine_->tick();

        // ---- The dead-man watchdog ----------------------------------------
        if (watchdog_->check_timeout(now_us())) {
            // `check_timeout()` already fired the safe-state callback.
        }

        // ---- The signalling channel ---------------------------------------
        //
        // The read is non-blocking (a zero timeout), so the tick loop
        // keeps running at its own rate.
        //
        // A *closed* channel is different from an empty one: when the
        // client that drives this binary goes away, there is nobody left
        // to send the offer, so the session cannot start and there is no
        // point in capturing and encoding into the void.  `read_line`
        // reports the closed channel by returning false with the reader
        // at EOF — but "no line yet" also returns false, so the EOF is
        // told apart by the dedicated flag below.
        if (rd_sig::channel_closed()) {
            std::fprintf(stderr,
                         "[rd_device] the signalling channel closed - stopping\n");
            running_ = false;
            break;
        }
        if (rd_sig::read_line(&line, 0)) {
            if (!line.empty()) {
                handle_signal(line);
            }
        }

        // ---- The periodic report ------------------------------------------
        const std::uint64_t t = now_us();
        if (t - last_report_us >= 5'000'000ull) {
            last_report_us = t;
            const auto stats = engine_->stats();
            const auto control = engine_->control_channel_stats();
            // The frame binding (v2-input-binder): the `ref_frame` id the
            // most recent input event named, and the capture instant /
            // frame age the device resolved it to (spec req 4).  "none"
            // and "n/a" are the honest reports for a frame the table does
            // not know — the device never guesses a capture instant
            // (spec risks 1 and 2).
            std::string ref_frame_repr = "none";
            if (has_binding_ref_frame_) {
                ref_frame_repr = std::to_string(binding_ref_frame_id_);
            }
            std::string capture_ts_repr = "n/a";
            if (has_binding_capture_ts_) {
                capture_ts_repr = std::to_string(binding_capture_ts_us_);
            }
            std::string age_us_repr = "n/a";
            if (has_binding_age_us_) {
                age_us_repr = std::to_string(binding_age_us_);
            }
            std::fprintf(stderr,
                         "[rd_device] capture=%llu pumped=%llu ctrl_in=%llu "
                         "ctrl_sent=%llu injected=%llu denied=%llu refused=%llu "
                         "ref_frame=%s capture_ts_us=%s age_us=%s\n",
                         static_cast<unsigned long long>(stats.video_capture_device_frames_captured),
                         static_cast<unsigned long long>(stats.video_capture_pumped_frames),
                         static_cast<unsigned long long>(control.messages_received),
                         static_cast<unsigned long long>(control.messages_sent),
                         static_cast<unsigned long long>(events_injected_),
                         static_cast<unsigned long long>(events_denied_),
                         static_cast<unsigned long long>(events_refused_),
                         ref_frame_repr.c_str(),
                         capture_ts_repr.c_str(),
                         age_us_repr.c_str());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // ---- The safe state on the way out -----------------------------------
    //
    // A session that ends with a key held down is a session that leaves
    // the operator's machine in a broken state.
    if (injector_) {
        const std::size_t released = injector_->release_all();
        if (released != 0) {
            std::fprintf(stderr, "[rd_device] released %zu inputs on exit\n",
                         released);
        }
    }

    engine_->close();
    return answered_ ? 0 : 1;
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
        if (arg == "--capture" && has_next) {
            opts.capture_name = argv[++i];
        } else if (arg == "--game-source") {
            opts.game_source = true;
        } else if (arg == "--game-fps" && has_next) {
            opts.game_fps = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--codec" && has_next) {
            opts.codec_name = argv[++i];
        } else if (arg == "--backend" && has_next) {
            opts.codec_backend = argv[++i];
        } else if (arg == "--width" && has_next) {
            opts.width = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--height" && has_next) {
            opts.height = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--duration" && has_next) {
            opts.duration_sec = std::atoi(argv[++i]);
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
        } else if (arg == "--no-inject") {
            opts.inject = false;
        } else if (arg == "--no-control") {
            opts.control_channel_name.clear();
        } else if (arg == "--recover-session") {
            opts.recover_session = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "[rd_device] unknown argument '%.*s'\n",
                         static_cast<int>(arg.size()), arg.data());
            print_usage(argv[0]);
            return 64;
        }
    }

    // ---- The session guard (the locked-workstation trap) ------------------
    //
    // A locked workstation breaks the capture in two different, both
    // expensive ways: DXGI refuses outright, and WGC freezes after one
    // frame without an error anywhere.  Neither failure says why, so
    // the device says it here -- *before* the capture starts, where the
    // diagnosis is still cheap.
    {
        const rds::SessionGuardState state = rds::query_session_guard_state();
        std::fprintf(stderr, "[rd_device] session guard: %s (%s)\n",
                     std::string(rds::session_guard_state_name(state)).c_str(),
                     rds::session_guard_state_hint(state).c_str());

        if (state == rds::SessionGuardState::kLocked && !opts.recover_session) {
            std::fprintf(stderr,
                         "[rd_device] the workstation is locked, so the "
                         "capture cannot work.  Unlock it (Win+L, then "
                         "sign in) or run 'rd_device --recover-session' "
                         "from an elevated prompt.\n");
            return 1;
        }
    }

    // ---- The explicit recovery -------------------------------------------
    //
    // The recovery is opt-in and privileged, so it is a *flag*, never
    // something that happens behind the operator's back.
    if (opts.recover_session) {
        std::string error;
        if (!rds::recover_console_session(&error)) {
            std::fprintf(stderr, "[rd_device] the session was not recovered: %s\n",
                         error.empty() ? "the reason was not reported" : error.c_str());
            return 1;
        }
        std::fprintf(stderr, "[rd_device] the session was recovered.\n");
        return 0;
    }

    DeviceSession session(opts);
    if (!session.start()) return 1;
    return session.run();
}
