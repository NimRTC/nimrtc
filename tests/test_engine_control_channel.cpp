/**
 * @file tests/test_engine_control_channel.cpp
 * @brief The engine-level control channel (remote-desktop plan **G1** /
 *        **G2**).
 *
 * ## What this test proves
 *
 * The module-level test (`src/modules/rd_input/tests/test_control_channel.cpp`)
 * proves the *framing*: that a message split across transport records is
 * reassembled, that a malformed frame is refused cleanly, and that the
 * counters move.  That test is deliberately transport-free.
 *
 * This test proves the *wiring*: that the engine
 *
 *   1. resolves the control channel through the **registry seam** (so the
 *      engine never includes the `rd_input` module's header — Layout
 *      Invariant 4 / ADR-025);
 *   2. sends and receives **whole messages** over the DTLS
 *      application-data seam (plan G1) with **no DataChannel backend**
 *      configured at all — the DataChannel path is deliberately *not*
 *      wired here, so the G1 path cannot be passing by accident;
 *   3. carries an **input event** (encoded with the `rd_input` codec)
 *      from one engine to the other and decodes it correctly on the far
 *      side — the payload is opaque to the engine, and this is what
 *      proves that the *opaque bytes* really do survive the round trip;
 *   4. accounts the **heartbeat** (plan G6) on both sides.
 *
 * ## The shape of the fake
 *
 * The two engines share one **app-data bus**: a fake DTLS session's
 * `send_app_data()` hands the bytes to the *other* session's inbox, and
 * `take_app_data()` drains it.  That is exactly the seam the wolfSSL
 * backend implements (`wolfSSL_write` into the record layer, `wolfSSL_read`
 * out of it), with the record layer replaced by a vector — so the test
 * exercises the engine's real `take_app_data()` pump and its real
 * `send_app_data()` call site without a socket.
 *
 * The ICE transport is fake too, but only so the engine's ICE-connected
 * gate opens (the engine drains DTLS only once ICE has selected a pair).
 * No RTP, no SRTP and no handshake is involved: the control plane does
 * not need any of them, and that is the point of plan G1.
 *
 * ## What this test does NOT prove
 *
 * The **G2** half (the control plane on its own transport) is proved
 * *structurally* — the engine selects the control stack's raw-UDP path
 * when the host asks for it — and not behaviourally: that a saturated
 * video path cannot starve the control path is a **benchmark**
 * (`tests/bench/`), not a unit test.  See the comments in
 * `src/modules/rd_input/include/nimrtc/rd_input/control_channel.hpp`.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/rd_input/event_codec.hpp>
#include <nimrtc/rd_input/input_event.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;
namespace ri = nimrtc::rd_input;
namespace core = nimrtc::core;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// The app-data bus — the two fake DTLS sessions talk through it
// ---------------------------------------------------------------------------

/**
 * @brief The bus that stands in for the DTLS record layer.
 *
 * `send_app_data()` on one session puts the bytes into the *other*
 * session's inbox; `take_app_data()` drains that inbox.  One record in,
 * one record out — the record boundary is preserved, exactly as the
 * wolfSSL backend's `take_app_data()` promises.
 */
class AppDataBus {
public:
    /** Attach a session to the bus; returns its endpoint index. */
    int attach() {
        std::lock_guard<std::mutex> lock(mutex_);
        inboxes_.emplace_back();
        return static_cast<int>(inboxes_.size()) - 1;
    }

    /**
     * @brief Forget every buffered record.
     *
     * The bus is shared by the whole fixture (the DTLS factory has to
     * outlive the individual tests, otherwise the engine would call
     * into a dangling pointer), so each test starts from a clean
     * world.  The *endpoints* stay: an endpoint index is bound to the
     * DTLS session that claimed it, and that session is still alive
     * (or closed) — dropping the index would only make the numbers
     * confusing.
     */
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& inbox : inboxes_) inbox.clear();
    }

    /**
     * @brief Hand a record to every *other* endpoint.
     *
     * Broadcasting (rather than pairing the endpoints explicitly)
     * keeps the fixture robust: the number of DTLS sessions a test
     * creates varies (some tests open one engine, some open two),
     * and a pairing scheme would have to guess which sessions belong
     * to the same test.  Broadcasting needs no such guess — a test
     * that opens two engines has exactly one sender and one receiver,
     * so the record reaches the receiver and nobody else reads it.
     */
    void deliver(int from, std::span<const std::uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < inboxes_.size(); ++i) {
            if (static_cast<int>(i) == from) continue;
            inboxes_[i].emplace_back(bytes.begin(), bytes.end());
        }
    }

    /** Drain this endpoint's inbox. */
    std::vector<std::vector<std::uint8_t>> take(int index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || static_cast<std::size_t>(index) >= inboxes_.size()) {
            return {};
        }
        std::vector<std::vector<std::uint8_t>> out;
        out.swap(inboxes_[static_cast<std::size_t>(index)]);
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<std::vector<std::vector<std::uint8_t>>> inboxes_;
};

// ---------------------------------------------------------------------------
// The fake DTLS session — connected, with a working app-data seam
// ---------------------------------------------------------------------------

/**
 * @brief A DTLS session that reports "connected" and carries app data.
 *
 * The engine asks three things of the DTLS session in this test:
 *
 *   - "is the handshake done?" — yes, so the engine installs (well, tries
 *     to install) the SRTP keys.  The fake exports no keying material, so
 *     SRTP stays down — which is fine: the control plane does not use
 *     SRTP (it rides the DTLS record layer itself).
 *   - "send these bytes" — `send_app_data()` hands them to the peer
 *     session through the bus.
 *   - "give me the bytes that arrived" — `take_app_data()` drains the
 *     inbox.
 */
class FakeAppDataDtlsSession final : public nd::IDtlsSession {
public:
    FakeAppDataDtlsSession(AppDataBus* bus, int index)
        : bus_(bus), index_(index) {}

    // ---- The seam-only methods -------------------------------------------
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    // ---- The engine-facing methods ---------------------------------------
    nimrtc::core::Result<void> open() noexcept override {
        return nimrtc::core::Result<void>::make_ok();
    }
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>,
                             const nd::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}
    nd::DtlsState state() const noexcept override { return nd::DtlsState::Connected; }
    bool is_connected() const noexcept override { return true; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

    // ---- The app-data seam (plan G1) -------------------------------------
    np::Status send_app_data(std::span<const std::uint8_t> bytes) noexcept override {
        if (bus_ == nullptr) return np::kErrNotReady;
        bus_->deliver(index_, bytes);
        return np::kOk;
    }

    std::vector<std::vector<std::uint8_t>> take_app_data() noexcept override {
        if (bus_ == nullptr) return {};
        return bus_->take(index_);
    }

private:
    AppDataBus* bus_ = nullptr;
    int index_ = 0;
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

/**
 * @brief The factory that hands out the fake sessions.
 *
 * Every `create()` call attaches a new endpoint to the bus, so the two
 * engines of this test end up on opposite ends of the same bus.
 */
class FakeAppDataDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    explicit FakeAppDataDtlsFactory(AppDataBus* bus) : bus_(bus) {}

    std::string_view id() const noexcept override { return "ctrl-fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session with an app-data seam (control channel test)";
    }

    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeAppDataDtlsSession>(
            bus_, bus_ != nullptr ? bus_->attach() : 0);
    }

private:
    AppDataBus* bus_ = nullptr;
};

// ---------------------------------------------------------------------------
// The fake ICE transport — "connected", and nothing else
// ---------------------------------------------------------------------------

/**
 * @brief A fake ICE transport that reports "connected".
 *
 * The engine only drains its DTLS session once ICE has selected a pair,
 * so the fake has to say "connected" for the app-data pump to run.  It
 * carries no packets: the control plane does not travel over ICE in this
 * test — it travels over the DTLS record layer the fake session owns.
 */
class FakeConnectedIce final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "ctrl-fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_ = std::move(on_recv);
        on_error_ = std::move(on_error);
    }
    np::Status send(np::BufferView, np::Addr = {}) noexcept override {
        return np::kOk;
    }
    int recv() noexcept override { return 0; }
    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "ctrlufrag"; }
    std::string local_password() const noexcept override { return "ctrlpassword"; }
    bool wait_for_gathering(int) noexcept override { return true; }
    std::vector<std::string> gathered_local_candidates() const noexcept override {
        return {};
    }
    np::Status set_remote_description(std::string_view) noexcept override {
        return np::kOk;
    }
    np::Status add_remote_candidate(std::string_view) noexcept override {
        return np::kOk;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t, std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

private:
    np::RecvCallback  on_recv_;
    np::ErrorCallback on_error_;
};

class FakeConnectedIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "ctrl-fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE (always connected, control channel test)";
    }
    np::IICETransport* create_ice() const override { return new FakeConnectedIce(); }
};

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineControlChannel : public ::testing::Test {
protected:
    /**
     * @brief The shared app-data bus.
     *
     * It is a **static** member on purpose: the DTLS factory is
     * registered once for the whole suite (the registry keeps the
     * pointer for the lifetime of the process), so the bus it points
     * at has to outlive every individual test.
     */
    static AppDataBus bus;

    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeConnectedIceFactory ice;
        PluginRegistry::instance().register_ice_transport(ice.id(), &ice);

        static FakeAppDataDtlsFactory dtls(&bus);
        PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);
    }

    void SetUp() override { bus.reset(); }

    /**
     * @brief The engine configuration both engines use.
     *
     * Note what is **not** here: `datachannel_backend` stays empty and
     * `control_channel_transport` stays at its default
     * (`kDtlsAppData`).  The whole point of the test is that the G1 path
     * works with **no DataChannel backend configured at all**.
     */
    static EngineConfig make_config() {
        EngineConfig cfg;
        cfg.transport_name  = "ctrl-fake-ice";
        cfg.dtls_name       = "ctrl-fake-dtls";
        cfg.stun_server_host.clear();
        // The control plane is what this test is about; everything else
        // stays off so the test exercises the control path and nothing
        // else.
        cfg.control_channel_name = "control-channel";
        cfg.control_channel_transport =
            np::ControlChannelTransport::kDtlsAppData;
        cfg.video_capture_name.clear();
        cfg.video_capture_pump = false;
        return cfg;
    }
};

AppDataBus EngineControlChannel::bus;

// ---------------------------------------------------------------------------
// The registry seam
// ---------------------------------------------------------------------------

TEST_F(EngineControlChannel, TheChannelIsResolvedThroughTheRegistry) {
    // The built-in channel registers under "control-channel" through the
    // `rd_input` module's registrar — the engine never names the module.
    ASSERT_NE(PluginRegistry::instance().get_control_channel("control-channel"),
              nullptr);

    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The channel is live: its counters are readable and start at zero.
    const auto stats = engine.control_channel_stats();
    EXPECT_EQ(stats.messages_sent, 0u);
    EXPECT_EQ(stats.messages_received, 0u);

    engine.close();
}

TEST_F(EngineControlChannel, AnUnknownChannelIdIsAHardError) {
    EngineConfig cfg = make_config();
    cfg.control_channel_name = "no-such-control-channel";

    NimRTCEngine engine(cfg);
    std::uint32_t err = 0;
    engine.set_on_error([&err](std::uint32_t code, std::string_view) {
        err = code;
    });

    EXPECT_NE(engine.open(), 0u)
        << "an unknown control-channel id must not silently fall back to "
           "another backend";
    EXPECT_NE(err, 0u);
}

// ---------------------------------------------------------------------------
// The G1 round trip
// ---------------------------------------------------------------------------

TEST_F(EngineControlChannel, TwoEnginesExchangeControlMessages) {
    NimRTCEngine a(make_config());
    NimRTCEngine b(make_config());
    ASSERT_EQ(a.open(), 0u) << "the first engine must open";
    ASSERT_EQ(b.open(), 0u) << "the second engine must open";

    // ---- The receiver's callback ----------------------------------------
    //
    // The callback fires on the engine's thread with one **whole**
    // message; the test copies the bytes out (the span is only valid for
    // the duration of the call).
    std::vector<std::vector<std::uint8_t>> received;
    b.set_on_control_message(
        [&received](std::span<const std::uint8_t> message) {
            received.emplace_back(message.begin(), message.end());
        });

    // ---- Send one input event -------------------------------------------
    //
    // The payload is the `rd_input` codec's output — the engine treats
    // it as opaque bytes, and the far side decodes it with the same
    // codec.
    ri::InputEvent event = ri::make_mouse_button(ri::MouseButton::kLeft,
                                                  ri::ButtonAction::kDown,
                                                  120, 240);
    event.timestamp_us = 987654u;
    event.ref_frame = 4242u;

    ri::EncodeOptions encode_options;
    encode_options.include_timestamp = true;
    encode_options.include_ref_frame = true;

    const auto encoded = ri::encode_event(event, encode_options);
    ASSERT_TRUE(encoded.ok()) << encoded.error().message();

    ASSERT_EQ(a.send_control_message(
                  std::span<const std::uint8_t>(encoded.value().data(),
                                                encoded.value().size())),
              np::kOk);

    // ---- Deliver it ------------------------------------------------------
    //
    // The inbound path is the engine's own tick: it drains the DTLS
    // session's app data, feeds the control channel (which reassembles
    // the message) and delivers it to the host's callback.
    for (int i = 0; i < 5; ++i) b.tick();

    ASSERT_EQ(received.size(), 1u)
        << "the control message must arrive at the far engine as ONE whole "
           "message";

    // ---- Decode it -------------------------------------------------------
    const auto decoded = ri::decode_event(
        core::ByteSpan{received[0].data(), received[0].size()});
    ASSERT_TRUE(decoded.ok()) << decoded.error().message();

    EXPECT_EQ(decoded.value().type, ri::InputEventType::kMouseButton);
    EXPECT_EQ(decoded.value().button, ri::MouseButton::kLeft);
    EXPECT_EQ(decoded.value().button_action, ri::ButtonAction::kDown);
    EXPECT_EQ(decoded.value().x, 120);
    EXPECT_EQ(decoded.value().y, 240);
    EXPECT_EQ(decoded.value().timestamp_us, 987654u);
    // The G5 binding survived the round trip.
    ASSERT_TRUE(decoded.value().ref_frame.has_value());
    EXPECT_EQ(*decoded.value().ref_frame, 4242u);

    // ---- The counters ----------------------------------------------------
    const auto a_stats = a.control_channel_stats();
    EXPECT_EQ(a_stats.messages_sent, 1u);
    EXPECT_EQ(a_stats.bytes_sent, encoded.value().size());

    const auto b_stats = b.control_channel_stats();
    EXPECT_EQ(b_stats.messages_received, 1u);
    EXPECT_EQ(b_stats.bytes_received, encoded.value().size());
    EXPECT_EQ(b_stats.malformed_received, 0u);
    EXPECT_EQ(b_stats.oversized_received, 0u);

    a.close();
    b.close();
}

TEST_F(EngineControlChannel, SeveralMessagesInOneRecordAreAllDelivered) {
    NimRTCEngine a(make_config());
    NimRTCEngine b(make_config());
    ASSERT_EQ(a.open(), 0u);
    ASSERT_EQ(b.open(), 0u);

    std::vector<std::vector<std::uint8_t>> received;
    b.set_on_control_message(
        [&received](std::span<const std::uint8_t> message) {
            received.emplace_back(message.begin(), message.end());
        });

    // Three messages back to back.  The DTLS record layer may pack them
    // into one record; the channel's framing is what keeps them
    // separate.
    for (int i = 0; i < 3; ++i) {
        ri::InputEvent event = ri::make_mouse_move(i + 1, -(i + 1));
        const auto encoded = ri::encode_event(event);
        ASSERT_TRUE(encoded.ok());
        ASSERT_EQ(a.send_control_message(
                      std::span<const std::uint8_t>(encoded.value().data(),
                                                    encoded.value().size())),
                  np::kOk);
    }

    for (int i = 0; i < 5; ++i) b.tick();

    ASSERT_EQ(received.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        const auto decoded = ri::decode_event(
            core::ByteSpan{received[static_cast<std::size_t>(i)].data(),
                           received[static_cast<std::size_t>(i)].size()});
        ASSERT_TRUE(decoded.ok()) << decoded.error().message();
        EXPECT_EQ(decoded.value().dx, i + 1);
        EXPECT_EQ(decoded.value().dy, -(i + 1));
    }

    a.close();
    b.close();
}

// ---------------------------------------------------------------------------
// G6 — the heartbeat
// ---------------------------------------------------------------------------

TEST_F(EngineControlChannel, TheHeartbeatIsCountedOnBothSides) {
    NimRTCEngine a(make_config());
    NimRTCEngine b(make_config());
    ASSERT_EQ(a.open(), 0u);
    ASSERT_EQ(b.open(), 0u);

    ASSERT_EQ(a.send_control_heartbeat(), np::kOk);
    for (int i = 0; i < 5; ++i) b.tick();

    // The engine counts the heartbeats it sent and the ones it received,
    // and publishes the instant of the last inbound heartbeat — the
    // dead-man watchdog (plan G6) consumes exactly that.
    const auto a_stats = a.control_channel_stats();
    EXPECT_EQ(a_stats.heartbeats_sent, 1u);

    const auto b_stats = b.control_channel_stats();
    EXPECT_EQ(b_stats.heartbeats_received, 1u);
    EXPECT_TRUE(b.last_control_heartbeat_us().has_value())
        << "the receiver must publish the instant of the last heartbeat";

    a.close();
    b.close();
}

// ---------------------------------------------------------------------------
// The disabled plane
// ---------------------------------------------------------------------------

TEST_F(EngineControlChannel, AnEmptyChannelNameDisablesTheControlPlane) {
    EngineConfig cfg = make_config();
    cfg.control_channel_name.clear();

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The plane is off, so the API reports "not ready" rather than
    // silently dropping the message.
    const std::uint8_t payload[] = {0x01, 0x02};
    EXPECT_EQ(engine.send_control_message(
                  std::span<const std::uint8_t>(payload, sizeof(payload))),
              np::kErrNotReady);

    engine.close();
}

// ---------------------------------------------------------------------------
// G2 — the control plane on its own transport
// ---------------------------------------------------------------------------

TEST_F(EngineControlChannel, TheRawUdpControlTransportIsSelectedWhenTheSessionHasOne) {
    // The "agent-gateway" strategy is the shape the remote-desktop
    // profile uses: browser interop for the media plane **plus** a
    // high-frequency control plane, which the CapabilitySelector turns
    // into a *separate* `raw-udp-arq` control stack (Selector §5.3
    // rule 1b).  That stack is what gives the control plane its own
    // 5-tuple and therefore its own congestion-control domain.
    EngineConfig cfg = make_config();
    cfg.transport_strategy_name = "agent-gateway";
    cfg.control_channel_transport = np::ControlChannelTransport::kRawUdp;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "the session must open with a control stack";

    // The session really did build the separate control stack.
    const auto view = engine.transport_strategy_session();
    EXPECT_EQ(view.control_factory_id, "raw-udp-arq")
        << "the control plane must be on its own stack";

    // And the control channel resolved onto the raw-UDP path — not onto
    // the media stack's DTLS session.
    EXPECT_EQ(engine.control_channel_transport(),
              np::ControlChannelTransport::kRawUdp);

    engine.close();
}

TEST_F(EngineControlChannel, ARawUdpRequestWithoutAControlStackIsAHardError) {
    // The "browser-call" strategy has **no** control stack (Selector
    // rule 1), so asking for the raw-UDP control transport cannot be
    // honoured.  That is a hard error, not a silent downgrade to the
    // DTLS app-data seam: a host that asked for G2's separate
    // congestion-control domain must be told when it did not get one.
    EngineConfig cfg = make_config();
    cfg.transport_strategy_name = "browser-call";
    cfg.control_channel_transport = np::ControlChannelTransport::kRawUdp;

    NimRTCEngine engine(cfg);
    std::uint32_t err = 0;
    engine.set_on_error([&err](std::uint32_t code, std::string_view) {
        err = code;
    });

    EXPECT_NE(engine.open(), 0u);
    EXPECT_NE(err, 0u);
}

// ---------------------------------------------------------------------------
// G5 — the engine's half of the input <-> frame binding
// ---------------------------------------------------------------------------

TEST_F(EngineControlChannel, TheRefFrameAccessorIsEmptyUntilAFrameIsShown) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // Nothing has been displayed yet, so there is no frame for an input
    // event to name.  The binder treats this as "leave the event
    // un-stamped", not as an error.
    EXPECT_FALSE(engine.last_displayed_ref_frame_id().has_value());

    engine.close();
}

}  // namespace
