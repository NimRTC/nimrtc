/**
 * @file src/engine/src/engine_transport.cpp
 * @brief Engine Transport plane (v1.1 R1). Strategy/session init, BWE + scheduler init, DataChannel registry, DTLS handshake, inbound dispatch, DTLS/SRTP install + drain, tick(), ICE-flap rehandshake poll..
 *
 * v1.1 R1 split of engine.cpp - mechanical move,
 * behaviour unchanged. Shares engine_impl.hpp.
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {


void NimRTCEngine::init_bwe_scheduler() noexcept {
    // ---- BWE plugin -------------------------------------------------------
    // Created and opened here; injected into ice_t_ via set_bwe().
    // If bwe_name is empty, BWE is skipped (passthrough — "transport" profile).
    if (!config_.bwe_name.empty() && !bwe_) {
        // Failure policy: Warn (matches pre-Slice-1.5 behaviour — BWE is
        // optional, so a missing or broken plugin degrades to "no BWE"
        // with a diagnostic log line, not an on_error_ fire).
        (void)pal::make_plugin<&pal::resolve_bwe>(
            config_.bwe_name,
            bwe_,
            config_.bwe_config,
            {.err_open_failed = core::kEngineInternal,  // unused (Warn)
             .err_missing = core::kEngineInternal,      // unused (Warn)
             .open_fail_mode = pal::PluginFailureMode::Warn,
             .missing_mode = pal::PluginFailureMode::Warn,
             .on_error = on_error_});
    }

    // ---- Scheduler plugin ------------------------------------------------
    // Created and opened here; injected into ice_t_ via set_scheduler().
    // If scheduler_config.enabled is false, scheduler is skipped (direct ICE send).
    if (config_.scheduler_config.enabled && !scheduler_) {
        (void)pal::make_plugin<&pal::resolve_scheduler>(
            config_.scheduler_name,
            scheduler_,
            config_.scheduler_config,
            {.err_open_failed = core::kEngineInternal,
             .err_missing = core::kEngineInternal,
             .open_fail_mode = pal::PluginFailureMode::Warn,
             .missing_mode = pal::PluginFailureMode::Warn,
             .on_error = on_error_});
    }

    // ---- Inject into ICE transport ---------------------------------------
    // ice_t_ is set by the time init_bwe_scheduler() is called
    // (pre_open / open both create it first).
    if (ice_t_) {
        ice_t_->set_bwe(bwe_.get());
        ice_t_->set_scheduler(scheduler_.get());
    }
}


void NimRTCEngine::shutdown_bwe_scheduler() noexcept {
    auto close_plugin = [](auto& p) {
        if (p) p->close();
        p.reset();
    };
    close_plugin(bwe_);
    close_plugin(scheduler_);
    if (ice_t_) {
        ice_t_->set_bwe(nullptr);
        ice_t_->set_scheduler(nullptr);
    }
}


// ---------------------------------------------------------------------------
// Phase 1 — transport strategy validation + CapabilitySelector.
//
// Resolve the strategy name and apply the CapabilitySelector BEFORE any
// module is created.  This is the single place where the engine validates
// the strategy id and records the resolved (media_factory_id,
// control_factory_id) pair.
//
// Failure modes:
//   - Unknown strategy name (non-empty, not in the 5-id catalog)
//     → std::invalid_argument from `strategy_by_name()`.  We catch it
//     and convert to kEngineInvalidParam (0x1001) so callers see a
//     uniform non-zero return code (the engine API does not throw).
//   - Selector returning an empty media_stack (the CapabilitySelector
//     default ctor returns empty stacks when no resolver is injected) →
//     kEngineInternal.  This shouldn't happen with a well-formed
//     strategy but is caught defensively.
//
// On success, the resolved pair is stored in `impl_` and surfaced
// through the `transport_strategy_session()` accessor.
// ---------------------------------------------------------------------------
uint32_t NimRTCEngine::init_transport_strategy() noexcept {
    {
        // ==================================================================
        // Sentinel: `cfg.transport_strategy_name == "shell"` opts out of
        // real ICE/DTLS entirely and picks the Slice 8 shell factory
        // (id="default", whose accessors return references to
        // null-instance singletons whose methods are no-ops /
        // kErrNotReady).  This path is intended for:
        //   - in-process unit tests that exercise engine wiring
        //     (create_data_channel, plugin dispatch, SDP munging) but
        //     have no network and would otherwise fail ice_t_->open()
        //     because of port-bind restrictions in sandboxed CI runners
        //     or low-integrity processes (errno 10013 WSAEACCES on the
        //     Windows GitHub Actions runner, observed repeatedly for
        //     hardcoded test port ranges like [52000, 52100]);
        //   - SDK/demo binaries where transport is intentionally disabled
        //     (render-only / source-only demos).
        // The "shell" sentinel bypasses Selector validation by handling
        // the unmatched name here and never calling
        // `requirements_from_profile_input()` — the Sentinel is a
        // well-known reserved name documented in the public EngineConfig
        // header (see engine.hpp).
        // ==================================================================
        if (config_.transport_strategy_name == "shell") {
            impl_->resolved_strategy_name     = "shell";
            impl_->resolved_media_factory_id  = "default";
            impl_->resolved_control_factory_id = "";     // no control plane
            core::log::Logger::instance().info(
                "engine: transport strategy 'shell' — using Slice 8 "
                "shell stack factory (id=\"default\"); ICE/DTLS/SRTP "
                "are inactive by design");
            return 0;
        }

        // Resolve the strategy name.  `strategy_by_name("")` returns the
        // browser-call default, so empty `cfg.transport_strategy_name`
        // is handled silently.
        std::string_view resolved_name;
        try {
            // requirements_from_profile_input() throws on unknown names.
            // The base call below has no per-instance overrides — it
            // returns the strategy's canonical TransportRequirements.
            ProfileTransportInput in;
            in.strategy = config_.transport_strategy_name;
            (void)requirements_from_profile_input(in);
            resolved_name = config_.transport_strategy_name.empty()
                ? std::string_view{"browser-call"}
                : config_.transport_strategy_name;
        } catch (const std::invalid_argument& e) {
            impl_->last_open_rc = core::kEngineInvalidParam;
            const std::string msg = std::string{"transport_strategy_name: "} + e.what();
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kEngineInvalidParam, msg);
            return core::kEngineInvalidParam;
        }
        impl_->resolved_strategy_name = resolved_name;

        // Apply the Selector.  We construct a default CapabilitySelector
        // (no resolver hooks), which in "rule-only mode" returns empty
        // stacks but records the chosen factory ids in `last_trace()`.
        // Those ids are the §5.3-rule-derived factory names — exactly
        // what the engine's accessors need to expose.
        CapabilitySelector selector;
        TransportRequirements req;
        try {
            req = requirements_from_profile_input(
                ProfileTransportInput{resolved_name});
        } catch (const std::invalid_argument&) {
            // Unreachable — we just validated the name above.  Keep the
            // catch as a defensive belt-and-braces; if it ever fires
            // it indicates a logic bug, not a config error.
            impl_->last_open_rc = core::kEngineInternal;
            return core::kEngineInternal;
        }
        TransportSession session = selector.select(req);
        const auto trace = selector.last_trace();

        // The Selector's trace carries the §5.3-rule-derived factory
        // ids.  An empty trace.media_factory_id means the Selector
        // could not resolve a media stack (rule 3 / `webrtc-quic` is
        // not yet implemented; the `webrtc-quic-call` strategy lands
        // in v1.x per transport-strategies.md §6 #1).
        //
        // The "shell" sentinel (no real ICE/DTLS) is handled earlier
        // in `init_transport_strategy()` and never reaches this block.
        //
        // We copy the trace strings into impl_->resolved_*_factory_id
        // (owned `std::string` storage) because `trace` is a local that
        // goes out of scope at the end of this block; views into it
        // would dangle.
        if (trace.media_factory_id.empty()) {
            // Fall back to the legacy v0.10.x media-stack id
            // ("webrtc-classic") for v0.12.0 strategies.  This
            // preserves the engine's pre-PR3 media-stack path while
            // keeping the strategy recorded.  Slice 7.x 2nd-half work
            // (v0.13+) will replace this with the Selector's actual
            // choice.
            impl_->resolved_media_factory_id = "webrtc-classic";
        } else {
            impl_->resolved_media_factory_id = trace.media_factory_id;
        }
        impl_->resolved_control_factory_id = trace.control_factory_id;

        // (void)session — the rule-only Selector returns empty stacks.
        // We log the trace instead so diagnostics still see what the
        // Selector would have chosen in production.
        (void)session;
        core::log::Logger::instance().info(
            std::string{"transport: strategy '"} +
            impl_->resolved_strategy_name +
            "' resolved — media_factory_id='" +
            impl_->resolved_media_factory_id +
            "', control_factory_id='" +
            impl_->resolved_control_factory_id + "'");
    }
    return 0;
}


// ---------------------------------------------------------------------------
// Phase 2 — TransportSession build + ICE pre-open configuration.
//
// Slice 7.6 PR3 replaces the pre-PR3 manual ICE + DTLS construction
// path with a TransportSession that owns the media_stack and
// control_stack.  The factory lookups are expressed inline here (rather
// than via a helper in the transport module) to keep the engine's
// link surface unchanged for existing consumers — nimrtc::transport is
// still linked, but the PUBLIC link is sufficient; no new API
// boundary is required.
//
// StackConfig defaults mirror what the pre-PR3 engine set on each
// plugin individually.  The §5.3 factory ids (webrtc-classic /
// raw-udp-arq) are stored in impl_->resolved_media_factory_id /
// control_factory_id.
//
// Rule 2 caveat (teleop-ultra-low-latency):
//   §5.3 Rule 2 maps "no browser interop + high-freq control" to
//   media=raw-udp-arq.  That stack has no DTLS.  Audio falls back to
//   null-encoder; ICE/connectivity work via raw-UDP ARQ.
// ---------------------------------------------------------------------------
uint32_t NimRTCEngine::init_transport_session() noexcept {
    {
        StackConfig scfg;
        scfg.ice_id  = config_.transport_name;
        scfg.dtls_id = config_.dtls_name.empty()
                           ? std::string{"wolfssl"}
                           : config_.dtls_name;
        // The sctp_id and rtp_id use the factory defaults; leave them
        // empty so the factory uses its own defaults.

        // v1.1 Ts3: canonical entry point — replaces the hand-rolled
        // get_transport_stack + create two-step with a single call.
        impl_->session = nimrtc::transport::make_transport_instance(
            impl_->resolved_media_factory_id,
            impl_->resolved_control_factory_id, scfg);

        if (!impl_->session.media_stack) {
            impl_->last_open_rc = core::kEngineInternal;
            const std::string msg =
                std::string{"engine: media stack factory '"} +
                impl_->resolved_media_factory_id +
                "' not registered or returned null (did "
                "nimrtc::transport::register_default_plugins() run "
                "before engine construction?).";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kEngineInternal, msg);
            return core::kEngineInternal;
        }

        // Extract handles into the owned session.
        //   - ice_t_: raw pointer (out of v0.13.0 scope; still safe because
        //     the engine never outlives the session's media_stack — see
        //     ~NimRTCEngine's delete impl_ ordering).
        //   - impl_->dtls: weak_ptr (v0.13.0; auto-expires after close()).
        ice_t_      = &impl_->session.media_stack->ice();
        impl_->dtls = impl_->session.media_stack->dtls_weak();   // weak_ptr — auto-expires after close()

        // Rule 2 warning: raw-udp-arq media has no DTLS.
        if (!impl_->dtls.lock()) {
            core::log::Logger::instance().warn(
                std::string{"engine: strategy '"} +
                impl_->resolved_strategy_name +
                "' uses media_factory_id='" +
                impl_->resolved_media_factory_id +
                "' with no DTLS (Rule 2 — raw-udp-arq media). "
                "Audio pipeline uses null-encoder; "
                "DTLS-dependent features (SRTP, DataChannel) inactive.");
        }

        core::log::Logger::instance().info(
            std::string{"engine: TransportSession built — "
                        "media='"} +
            impl_->resolved_media_factory_id +
            "', control='" +
            impl_->resolved_control_factory_id +
            "'");
    }

    // ---- Apply pre-open ICE configuration ---------------------------------
    // set_bind_address / set_stun_server / set_local_port_range are safe
    // on null-instance ICE returns from RawUdpArqStack (they are no-ops).
    ice_t_->set_bind_address(config_.local_bind_address);
    ice_t_->set_stun_server(config_.stun_server_host, config_.stun_server_port);
    ice_t_->set_local_port_range(config_.local_port_range_begin,
                                  config_.local_port_range_end);
    // TURN servers are added via NimRTCEngine::add_turn_server() *before*
    // open() — they're applied to ice_t_ at that time, so no loop here.

    return 0;
}


// ---------------------------------------------------------------------------
// Phase 5 — DataChannel factory verification.
//
// Verify the IDataChannelFactory registered under
// `cfg.datachannel_name` resolves via PluginRegistry.  The engine
// does NOT auto-create any channel — callers explicitly invoke
// `create_data_channel(label)` after open() succeeds (mirrors the JS
// `peer.createDataChannel()` API).
//
// When `datachannel_name.empty()` the engine skips DataChannel
// entirely: `create_offer()` won't emit `m=application`, the answer
// side silently drops the peer's `m=application` line, and
// `create_data_channel()` returns nullptr.
//
// The factory lookup is intentionally lazy — we don't construct any
// IDataChannel at init time.  The default `SctpDataChannelFactory`
// registers at static-init time via `register_default_plugins()`,
// so by the time `init_modules_once()` runs, the factory is
// already in the registry.
//
// NOTE (v0.15.0): the WebTransport/QUIC data plane has landed.  The default
// backend is now "web_transport" (msquic-backed); the session is created lazily
// in create_data_channel() and shared by all channels of this engine.  The notes
// below describe the legacy "sctp" backend, retained for backward compatibility.
//
// (historical note: the v0.11 plan deferred the SCTP-over-DTLS wiring; the SCTP
// backend below is the retained legacy path.)
//
//
// The full DataChannel lifecycle requires plumbing the DTLS
// transport's outbound datagram path into the SCTP socket:
//   1. Once DTLS reaches `Connected`, derive the SRTP / DTLS-SRTP
//      keying material and feed it into the SCTP socket via the
//      `ISctpSocket::set_dtls_keys()` hook.
//   2. Hook the DTLS transport's outbound (encrypted) side to
//      the SCTP socket's `send_datagram()` -- that's where
//      SCTP-over-DTLS packets are emitted onto the wire.
//   3. Hook the DTLS transport's inbound (decrypted) datagrams
//      into `ISctpSocket::feed_inbound_datagram()` so usrsctp
//      can drain its receive queue.
//   This skeleton only reserves the (void*) transport pointer
// placeholder (`Impl::datachannel_transport_cookie`) -- the SCTP path reserves it;
// the WebTransport path owns `Impl::wt_session` instead.
// Returns `core::kEngineInternal` when `cfg.datachannel_backend` holds
// an unrecognised value (no silent fallback to "sctp"); a missing
// factory only logs a warning and `create_data_channel()` will return
// `nullptr` on the caller side.
// ---------------------------------------------------------------------------
uint32_t NimRTCEngine::init_datachannel_registry() noexcept {
    // ---- DataChannel factory dispatch (v0.14.0-rc.1) --------------------
    //
    // Algorithm:
    //   1. If `cfg.datachannel_backend` is empty, fall back to the legacy
    //      `cfg.datachannel_name` selector (backward compat for v0.x
    //      callers).  After this step the field is treated as the new
    //      `datachannel_backend` (might be "sctp", "web_transport", "...").
    //   2. Dispatch on the resolved backend id:
    //        "sctp"          — pick factory id "sctp" (or `datachannel_name`
    //                          override when non-empty).
    //        "web_transport" — pick factory id "web_transport" (the
    //                          `datachannel_name` override is ignored in
    //                          this mode); also verify the
    //                          `web_transport_name` session factory
    //                          resolves (readiness-only; physical
    //                          session creation follows in the next rc.1
    //                          PR that wires DTLS↔WebTransport).
    //        ""              — DataChannel disabled (resolved id stays
    //                          empty).
    //        <other>         — hard error: `kEngineInternal` is returned
    //                          so `open()` fails and surfaces the problem
    //                          via `on_error_`; the factory id is left
    //                          empty and `create_data_channel()` returns
    //                          nullptr.  No silent fallback to "sctp".
    //
    // A legacy `datachannel_name` holding a value outside the known
    // backend families is the v0.x power-user escape hatch: it resolves as
    // a direct `IDataChannelFactory` id rather than being rejected.
    //
    // The resolved factory id is stored in `resolved_dc_factory_id_` for
    // `create_data_channel()` to read.
    //
    // Returns 0 on every path except an unrecognised `datachannel_backend`
    // value: a missing DC factory just logs a warning, and a missing
    // WebTransport session factory is soft-warned at this phase (the engine
    // surfaces it without failing init so callers can still pre-register a
    // factory between construction and `open()`).

    // Step 1: backward-compat normalisation.
    std::string backend = config_.datachannel_backend;
    // Set when the legacy `datachannel_name` carried a value that is not a
    // known backend family.  Such a value is a direct `IDataChannelFactory`
    // id (the v0.x power-user escape hatch), so it resolves through the
    // registry directly instead of being rejected as an unknown backend.
    bool legacy_direct_factory_id = false;
    if (backend.empty() && !config_.datachannel_name.empty()) {
        // Legacy v0.x call: `datachannel_name` is set, `datachannel_backend`
        // is not.  Map the legacy value to the new dispatch:
        //   - "sctp" (the v0.x default) maps to "sctp" backend
        //     (factory id stays "sctp")
        //   - any other (non-empty) factory id is treated as a power-user
        //     factory override; the engine resolves it directly without
        //     dispatching through a backend family.
        if (config_.datachannel_name == "sctp") {
            backend = "sctp";   // explicit even though "sctp" is also the default
        } else {
            // Power-user factory override.  Treat the value as a direct
            // factory id rather than a backend family — flagging it keeps
            // the dispatch below from reporting it as an unrecognised
            // `datachannel_backend` value.
            backend              = config_.datachannel_name;
            legacy_direct_factory_id = true;
        }
    }

    // Step 2: dispatch.
    if (backend.empty()) {
        // Disabled — resolved id stays empty, no factory resolution.
        core::log::Logger::instance().debug(
            "engine: DataChannel disabled (datachannel_backend == \"\" "
            "&& datachannel_name.empty())");
        return 0;
    }

    if (legacy_direct_factory_id) {
        // v0.x power-user path: `datachannel_name` held a custom factory id.
        // Resolve it directly through the registry — no family dispatch, no
        // hard error (this is the pre-rc.1 contract).
        resolved_dc_factory_id_ = backend;
        const plugins::IDataChannelFactory* dcf =
            pal::resolve_datachannel(resolved_dc_factory_id_);
        if (dcf) {
            core::log::Logger::instance().info(
                std::string("engine: DataChannel plugin '")
                    .append(dcf->id())
                    .append("' ready (legacy datachannel_name resolved via "
                            "core::PluginRegistry::get_datachannel)"));
        } else {
            core::log::Logger::instance().warn(
                std::string("engine: DataChannel plugin '")
                    .append(resolved_dc_factory_id_)
                    .append("' not registered (legacy datachannel_name; "
                            "create_data_channel() will return nullptr)"));
        }
        return 0;
    }

    if (backend == "sctp") {
        // Legacy SCTP-over-DTLS path.  `datachannel_name` stays the
        // authoritative knob in this family: it is both the factory-id
        // override and the explicit-disable signal (empty = disabled),
        // which is the v0.x contract (`cfg.datachannel_name = ""` was how
        // callers turned DataChannel off).  The `EngineConfig` default is
        // already "sctp", so an empty value here is unambiguously an
        // intentional disable rather than an unset field.
        if (config_.datachannel_name.empty()) {
            core::log::Logger::instance().debug(
                "engine: DataChannel disabled (datachannel_name is empty)");
            return 0;
        }
        resolved_dc_factory_id_ = config_.datachannel_name;

        const plugins::IDataChannelFactory* dcf =
            pal::resolve_datachannel(resolved_dc_factory_id_);
        if (dcf) {
            core::log::Logger::instance().info(
                std::string("engine: DataChannel backend='sctp' ready "
                            "(factory '")
                    .append(dcf->id())
                    .append("' resolved via core::PluginRegistry::get_datachannel)"));
        } else {
            core::log::Logger::instance().warn(
                std::string("engine: DataChannel backend='sctp' but factory '")
                    .append(resolved_dc_factory_id_)
                    .append("' is not registered (create_data_channel() will return nullptr)"));
        }
        return 0;
    }

    if (backend == "web_transport") {
        // v1.0.0 production data plane.  Factory id is fixed to
        // "web_transport" (`datachannel_name` is intentionally ignored in
        // this mode so the family-name and the factory-id are not
        // duplicated knobs).
        resolved_dc_factory_id_ = "web_transport";

        const plugins::IDataChannelFactory* dcf =
            pal::resolve_datachannel(resolved_dc_factory_id_);
        if (dcf) {
            core::log::Logger::instance().info(
                std::string("engine: DataChannel backend='web_transport' "
                            "ready (factory '")
                    .append(dcf->id())
                    .append("' resolved via core::PluginRegistry::get_datachannel)"));
        } else {
            core::log::Logger::instance().warn(
                "engine: DataChannel backend='web_transport' but factory "
                "'web_transport' is not registered (was "
                "NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC=OFF at build time? "
                "create_data_channel() will return nullptr)");
        }

        // Also verify the WebTransport session factory (readiness only;
        // physical session creation is the v0.14.0-rc.1 follow-up PR that
        // wires DTLS↔WebTransport plumbing).
        if (config_.web_transport_name.empty()) {
            core::log::Logger::instance().warn(
                "engine: DataChannel backend='web_transport' but "
                "web_transport_name is empty (skipping session factory lookup)");
        } else {
            const web_transport::IWebTransportSessionFactory* wtf =
                pal::resolve_web_transport(config_.web_transport_name);
            if (wtf) {
                core::log::Logger::instance().info(
                    std::string("engine: WebTransport session factory '")
                        .append(wtf->id())
                        .append("' ready (factory resolved via "
                                "core::PluginRegistry::get_web_transport)"));
            } else {
                core::log::Logger::instance().warn(
                    std::string("engine: WebTransport session factory '")
                        .append(config_.web_transport_name)
                        .append("' not registered (the engine will not be "
                                "able to create the session; this becomes "
                                "fatal in the rc.1 PR that wires "
                                "DTLS↔WebTransport plumbing)"));
            }
        }
        return 0;
    }

    // Unknown backend — hard error.  `open()` propagates the return code
    // so the caller sees a non-zero rc; there is deliberately no silent
    // fallback to "sctp" (a typo'd backend id would otherwise mask an
    // integrator error).
    std::string msg = std::string("engine: datachannel_backend='") + backend
        + "' is not recognised (valid: \"sctp\", \"web_transport\", \"\"; "
          "see docs/plan/v0.14-plan.md §2.1 / docs/plan/v1.0-plan.md §2.1.1)";
    core::log::Logger::instance().error(msg);
    impl_->last_open_rc = core::kEngineInternal;
    if (on_error_) on_error_(core::kEngineInternal, msg);
    resolved_dc_factory_id_.clear();   // force nullptr path in create_data_channel
    return core::kEngineInternal;
}


// ---------------------------------------------------------------------------
// Phase 6 — SRTP context creation + DTLS open + ICE callbacks.
//
// Slice 7.6 PR3 (v0.12.0): DTLS session ownership moved from the
// engine (direct `reg.get_dtls_session()` + `factory->create()`) to
// `WebRtcClassicStackFactory` inside `make_transport_instance()`.
// `impl_->dtls` is a weak_ptr into `impl_->session.media_stack`
// (see the field comment on `Impl::dtls`).
//
// v0.13.0: changed from raw pointer to weak_ptr so `.lock()` returns
// nullptr automatically after close(), eliminating the UAF class.
//
// The factory's `create()` already built a default DtlsConfig with
// role=Server / srtp_profile=Aes128CmSha1_80 — matching what the
// pre-PR3 engine set here.  The DTLS role is still a placeholder;
// `process_remote_sdp()` resolves the real role from the peer's
// `a=setup` attribute (RFC 5763 §5) and calls `dtls->set_role()`
// before the handshake.  See P0#1 (DTLS role negotiation).
//
// Rule 2 caveat (teleop-ultra-low-latency):
//   `impl_->dtls` may be expired (weak_ptr, no lock) when the media stack is
//   raw-udp-arq (no DTLS in the media plane).  In that case we
//   skip DTLS open and surface a warning earlier in this function.
//   Note: weak_ptr::lock() == nullptr for both "never set" and "already destroyed".
//
// DTLS-missing-but-required gate (regression for test_dtls_seam
//   `cfg_dtls_name_unknown_factory_yields_open_failure`):
//   When the resolved media stack REQUIRES DTLS (webrtc-classic
//   for all 4 browser-interop strategies) but the user-supplied
//   `dtls_name` doesn't resolve to a registered factory, the
//   stack's factory returns the stack with dtls_=null (defensive
//   — see webrtc_classic_stack.cpp create()'s "DTLS factory not
//   found" branch).  The engine must NOT silently proceed without
//   DTLS in that case — it must surface kDtlsOpenFailed (the same
//   error the pre-PR3 path produced when `reg.get_dtls_session()`
//   returned null).  Rule 2 (raw-udp-arq media) is the only case
//   where null DTLS is OK; the strategy id is checked here to
//   distinguish "DTLS-less by design" from "DTLS-less by
//   misconfiguration".
// ---------------------------------------------------------------------------
uint32_t NimRTCEngine::init_dtls_handshake() noexcept {
    // ---- SRTP ----------------------------------------------------------
    impl_->srtp = std::make_unique<srtp::SrtpContext>();
    core::log::Logger::instance().debug(
        "engine: srtp_ created, about to open dtls_");

    // ---- DTLS ----------------------------------------------------------
    if (auto* d = impl_->dtls.lock().get()) {
        auto open_rc = d->open();
        if (!open_rc.ok()) {
            impl_->last_open_rc = core::kDtlsOpenFailed;
            core::log::Logger::instance().error(
                std::string("engine: dtls_->open() failed: ") +
                open_rc.error().message());
            if (on_error_) on_error_(core::kDtlsOpenFailed,
                "DTLS open failed: " + open_rc.error().message());
            return core::kDtlsOpenFailed;
        }
        core::log::Logger::instance().debug("engine: dtls_->open() returned ok");
    } else if (impl_->resolved_media_factory_id == "raw-udp-arq" ||
               impl_->resolved_media_factory_id == "default") {
        // No-DTLS-by-design media stack:
        //   - "raw-udp-arq"  — Rule 2 strategy (teleop-ultra-low-latency).
        //     Strategically chosen for high-freq control without browser
        //     interop, so DTLS is omitted from the media plane.
        //   - "default"      — Slice 8 shell stack used by the "shell"
        //     transport strategy (no ICE/DTLS/SRTP at all). The shell
        //     is intended for in-process engine wiring tests and
        //     render-only / source-only SDK demos that bypass
        //     networking entirely.
        // The strategy block above already warned (raw-udp-arq) or
        // info-logged (default) at this phase; here we just log at
        // debug level so the trace shows the session was built
        // without DTLS.
        core::log::Logger::instance().debug(
            "engine: dtls_->open() skipped — media stack id='" +
            impl_->resolved_media_factory_id +
            "' has no DTLS in its media path by design");
    } else {
        // Media stack requires DTLS (webrtc-classic) but dtls_ is
        // null.  This means the user-supplied cfg.dtls_name (or the
        // default "wolfssl") didn't resolve in core::PluginRegistry.
        // The pre-PR3 contract was to fail with kEngineInternal in
        // that case (test_dtls_seam regresses this).  Preserve that
        // contract: a misconfigured DTLS backend is a hard error,
        // not a silent degradation.  The error code matches the
        // pre-Slice-8 "transport plugin not found" path that the
        // test pins.
        impl_->last_open_rc = core::kEngineInternal;
        const std::string msg =
            std::string{"engine: DTLS plugin '"} +
            config_.dtls_name +
            "' not registered (required by media stack '" +
            impl_->resolved_media_factory_id +
            "' — no DTLS in the resolved stack). Check "
            "nimrtc::dtls::register_default_plugins() ran before "
            "engine construction.";
        core::log::Logger::instance().error(msg);
        if (on_error_) on_error_(core::kEngineInternal, msg);
        return core::kEngineInternal;
    }

    // ---- ICE recv / error callbacks ------------------------------------
    ice_t_->set_callbacks(
        [this](plugins::BufferView bv) { on_transport_recv(bv); },
        [this](plugins::Status err, std::string_view msg) {
            if (on_error_) on_error_(static_cast<std::uint32_t>(err), msg);
        });

    return 0;
}


int NimRTCEngine::tick() noexcept {
    if (!is_open()) return 0;

    // ---- WebTransport session state poll (v0.15.0 product route) --------
    // ` wt_session` is created lazily by create_data_channel(); poll it
    // here and surface kConnected/kFailed transitions via on_data_state_.
    if (impl_->wt_session) {
        const auto st = impl_->wt_session->state();
        const auto cur = static_cast<std::uint8_t>(st);
        if (cur != impl_->wt_last_state) {
            impl_->wt_last_state = cur;
            // WebTransportState: 0=kConnecting 1=kConnected 2=kClosing
            // 3=kClosed 4=kFailed (see web_transport_state.hpp).
            if (cur == 1) {
                if (on_data_state_) on_data_state_("open");
            } else if (cur == 4) {
                if (on_data_state_) on_data_state_("error");
            } else if (cur == 3) {
                if (on_data_state_) on_data_state_("closed");
            }
        }
    }

    // ---- Push BWE estimate into the scheduler (P2/P3 wiring) -------------
    // estimate() is cheap (reads an atomic snapshot in the AIMD impl) and
    // tick is on the engine thread, so no extra locks.  We skip the push
    // when target_bps == 0 so the scheduler keeps its previous budget
    // rather than reading "0 = total sender pause" on a transient BWE
    // startup glitch.  Both pointers may be null in the "transport" profile.
    if (bwe_ && scheduler_) {
        const std::uint32_t target_bps = bwe_->estimate().target_bitrate_bps;
        if (target_bps > 0) {
            scheduler_->on_bwe_update(target_bps);
        }
    }

    // ---- BWE-linked audio bitrate adaptation ----------------------------
    //
    // The audio encoder is the one outbound stream that must survive a
    // bandwidth collapse: when the estimate drops, the encoder's
    // bitrate follows it down (never below the configured floor), and
    // it climbs back towards the configured ceiling once the link
    // recovers.  The controller is rate-limited, so this is a no-op
    // between two steps (see maybe_update_audio_bitrate()).
    maybe_update_audio_bitrate();

    // ---- Drain the scheduler (if enabled) ---------------------------------
    // The scheduler holds outbound packets (audio / video / control) and
    // dispatches them in priority order.  drain_with() invokes our callback
    // for each packet; we send it through ICE.
    if (scheduler_) {
        scheduler_->drain_with(64, [this](plugins::Priority,
                                          core::ByteSpan data) noexcept -> bool {
            if (!ice_t_) return false;
            plugins::BufferView bv{data.data(), data.size()};
            ice_t_->send(bv, {});
            return true;
        });
    }

    // ---- Drive the audio receive pipeline (v1.2.0, audio pipeline WP-7) --
    //
    // The pipeline is clock-driven (spec A-1): the host's tick() is what
    // makes it play out.  Ticking it here — on the engine thread, next to
    // the other periodic work — is what makes the output cadence follow
    // the media time rather than the packet arrival (spec A-3 / A-4).
    if (audio_receiver_) {
        audio_receiver_->tick(plugins::TimestampUs{
            static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    core::SteadyClock::now().time_since_epoch()).count())});
    }

    // Only drain DTLS outbound once ICE has selected a candidate pair;
    // otherwise libjuice drops the UDP datagram ("Send while ICE is not
    // connected") and the handshake never starts.
    // v1.1 Dtls2: poll ICE flap before draining so a NAT-rebinding
    // disconnected->connected (2nd) transition triggers rehandshake().
    poll_ice_flap_for_rehandshake();
    if (is_ice_connected()) {
        drain_dtls();
        maybe_send_sender_report();
        // v1.1 Q3: put the pending video feedback (NACK / PLI) on the
        // wire.  It is sent from here — not from the receiver callback —
        // so the transport's send() is never re-entered from its recv().
        flush_video_feedback();
        // v1.2.0 WP-8 (spec B-12): the RTCP receiver report for the
        // inbound audio stream.  It goes out at least once per second
        // while audio is inbound — the browser's `inbound-rtp` loss and
        // jitter figures are derived from the reports it receives, so a
        // stream that never reports shows no statistics at all.
        maybe_send_receiver_report();
    }
    return ice_t_->recv();
}


void NimRTCEngine::poll_ice_flap_for_rehandshake() noexcept {
    // v1.1 Dtls2: NAT-rebinding recovery. Engine thread only (called from
    // tick()); impl_/ice_t_ lifetime is guaranteed by is_open() gate.
    if (!ice_t_ || !impl_) return;
    const plugins::IceState cur = ice_t_->state();
    if (cur == impl_->last_ice_state) return;
    const plugins::IceState prev = impl_->last_ice_state;
    impl_->last_ice_state = cur;
    const auto name = [](plugins::IceState s) {
        switch (s) {
            case plugins::IceState::Disconnected: return "disconnected";
            case plugins::IceState::Gathering:    return "gathering";
            case plugins::IceState::Connecting:   return "connecting";
            case plugins::IceState::Connected:    return "connected";
            case plugins::IceState::Completed:    return "completed";
            case plugins::IceState::Failed:       return "failed";
        }
        return "unknown";
    };
    const bool cur_up = (cur == plugins::IceState::Connected ||
                         cur == plugins::IceState::Completed);
    const bool prev_up = (prev == plugins::IceState::Connected ||
                          prev == plugins::IceState::Completed);
    if (cur_up && !prev_up) {
        if (!impl_->ice_connected_once) {
            impl_->ice_connected_once = true;
        } else if (impl_->ice_rehandshake_armed) {
            impl_->ice_rehandshake_armed = false;
            auto d = impl_->dtls.lock();
            if (d && d->state() == dtls::DtlsState::Connected) {
                core::log::Logger::instance().info(
                    "engine[dtls]: ICE flap disconnected->connected, "
                    "triggering DTLS rehandshake");
                const auto rc = d->rehandshake();
                if (rc == plugins::kOk) {
                    impl_->srtp_installed = false;
                } else {
                    core::log::Logger::instance().warn(
                        std::string("engine[dtls]: rehandshake rejected rc=") +
                        std::to_string(static_cast<int>(rc)));
                }
            } else {
                core::log::Logger::instance().info(
                    std::string("engine[dtls]: ICE flap ") + name(prev) +
                    "->" + name(cur) + ", no Connected DTLS session "
                    "(rule-2/raw-udp-arq or pre-handshake); skip rehandshake");
            }
        }
        return;
    }
    if (!cur_up && impl_->ice_connected_once) {
        impl_->ice_rehandshake_armed = true;
        core::log::Logger::instance().info(
            std::string("engine[ice]: connected->") + name(cur) +
            ", arming DTLS rehandshake on next reconnect");
    }
}


void NimRTCEngine::on_transport_recv(const plugins::BufferView& pkt) noexcept {
    // P0 (v0.12.0): guard against post-close use.  The ICE recv callback
    // can race with close(): a buffered packet may arrive on the transport
    // thread after close() has torn down the media_stack.  Without this
    // guard we'd hit a use-after-free in d->state() / feed_inbound()
    // below.  Mirrors the send_audio() pattern at line 2119.
    //
    // v0.13.0: impl_->dtls is a weak_ptr; .lock() returns nullptr
    // automatically after close(), so the inner if (auto d = ...) check
    // below catches the closed-engine case.  This is_open() guard
    // remains as a defence-in-depth short-circuit that avoids the
    // weak_ptr::lock() atomic increment on every non-DTLS packet.
    if (!is_open()) return;
    if (pkt.empty() || pkt.size() < 1) return;
    std::uint8_t b0 = pkt.data()[0];
    bool looks_like_dtls = (b0 == 22 || b0 == 23 || b0 == 21);
    if (looks_like_dtls) {
        auto d = impl_->dtls.lock();
        if (!d) return;  // weak_ptr expired — engine is closed; drop packet
        core::log::Logger::instance().info(
            std::string("engine[dtls]: rx dtls ct=") + std::to_string(b0) +
            " len=" + std::to_string(pkt.size()) +
            " state=" + std::string(nimrtc::dtls::DtlsSession::state_name(d->state())));
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        // The ITransport::RecvCallback interface carries only the payload, no
        // source address.  For DTLS over ICE there is only one peer at a time
        // (the selected ICE candidate pair), so we derive the peer address from
        // the ICE transport's current selected remote endpoint.  This is correct
        // for both the initial ClientHello and all subsequent handshake records.
        //
        // NOTE: `ice_t_->remote_addr()` returns a text-encoded "host:port"
        // string packed into a plugins::Addr structure.  The DtlsAddr uses
        // plain "host" + port fields — the parsing below extracts those fields.
        nimrtc::dtls::DtlsAddr from;
        if (ice_t_) {
            plugins::Addr raw = ice_t_->remote_addr();
            // Text encoding: "host\0port\0" packed in raw.data[0..len-1].
            std::string_view sv(reinterpret_cast<const char*>(raw.data), raw.len);
            std::size_t colon = sv.find(':');
            if (colon != std::string_view::npos) {
                from.host.assign(sv.data(), colon);
                auto port_str = sv.substr(colon + 1);
                if (!port_str.empty()) {
                    // NOLINTNEXTLINE(cert-env33-c): port is always numeric here
                    int p = std::atoi(std::string(port_str).c_str());
                    from.port = static_cast<std::uint16_t>(p);
                }
            }
        }
        d->feed_inbound(bytes, from);
        // Only flush outbound DTLS records once ICE has selected a pair,
        // mirroring the gate in tick().  Without this, the first inbound
        // ClientHello (which arrives as soon as the peer's ICE connectivity
        // check reaches us) enqueues a HelloVerifyRequest that libjuice
        // silently drops with "Send while ICE is not connected"; the peer
        // never sees our cookie and keeps retransmitting its cookie-less
        // ClientHello forever.
        if (is_ice_connected()) {
            drain_dtls();
        }
        return;
    }
    // CCS (content type 20) isn't recognised as DTLS above; process it so
    // the receiving side can complete its state machine.
    if (b0 == nimrtc::dtls::kDtlsChangeCipherSpec) {
        auto d = impl_->dtls.lock();
        if (!d) return;  // weak_ptr expired — engine is closed; drop packet
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        // Same peer-address derivation as the DTLS path above.
        nimrtc::dtls::DtlsAddr from;
        if (ice_t_) {
            plugins::Addr raw = ice_t_->remote_addr();
            std::string_view sv(reinterpret_cast<const char*>(raw.data), raw.len);
            std::size_t colon = sv.find(':');
            if (colon != std::string_view::npos) {
                from.host.assign(sv.data(), colon);
                auto port_str = sv.substr(colon + 1);
                if (!port_str.empty()) {
                    int p = std::atoi(std::string(port_str).c_str()); // NOLINT(cert-env33-c)
                    from.port = static_cast<std::uint16_t>(p);
                }
            }
        }
        d->feed_inbound(bytes, from);
        if (is_ice_connected()) {
            drain_dtls();
        }
        return;
    }
    std::vector<std::uint8_t> plaintext;
    core::ByteSpan view(pkt.data(), pkt.size());
    bool tried_srtp = false;
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(0, /*outgoing=*/false);
        if (sess) {
            // SRTP and SRTCP use *different* key derivations and independent
            // sequence-index spaces (RFC 3711 §3.3), so an SRTCP packet fed
            // to srtp_unprotect fails the auth tag and is reported as a
            // decryption error.  The two are indistinguishable on the wire
            // under rtcp-mux (RFC 5761 — same 5-tuple, no port to demux on),
            // so the only way to tell them apart is to try one and fall back
            // to the other.  Trying RTP first preserves the existing hot
            // path: a media packet succeeds on the first attempt and never
            // pays for the RTCP branch.
            auto r = sess->unprotect_rtp(view, nullptr, nullptr);
            if (r) {
                view = r.value();
                tried_srtp = true;
            } else {
                // Not an SRTP packet — try the RTCP context before giving up.
                auto rc = sess->unprotect_rtcp(view);
                if (rc) {
                    handle_rtcp(rc.value());
                    return;
                }
                ++impl_->srtp_stats_drop;
                return;
            }
        }
    }
    dispatch_media_inbound(view, tried_srtp);
}


void NimRTCEngine::dispatch_media_inbound(plugins::BufferView view,
                                          bool tried_srtp) noexcept {
    // ---- RTCP demux before RTP (ADR-017) ---------------------------------
    // RFC 3550 §6.1: an RTCP packet is identified by the second octet's
    // payload-type field falling in 192..223, which cannot collide with an
    // RTP payload type (0..127) or an RTCP packet riding the same socket
    // under rtcp-mux (RFC 5761).  Sender Reports are the anchor the
    // timeline seam needs, so they get parsed here rather than falling
    // through to the RTP parser (which would reject them) and being
    // dropped as "non-RTP".
    //
    // This catches the *unencrypted* case (SRTP not yet keyed, or a peer
    // that negotiated plaintext RTCP).  The SRTCP case is handled by the
    // caller (on_transport_recv) with the unprotect_rtcp fallback, where
    // the plaintext PT byte is only readable after decryption.
    if (view.size() >= 2) {
        const std::uint8_t pt2 = view[1];
        if (pt2 >= 192 && pt2 <= 223) {
            handle_rtcp(view);
            return;
        }
    }

    rtp::Parser rp;
    auto parsed = rp.parse(view);
    if (parsed) {
        handle_rtp(parsed.value());
        return;
    }
    if (!tried_srtp) {
        core::log::Logger::instance().debug(
            "engine: non-RTP and non-DTLS packet (likely STUN handled by libjuice)");
    }
}


void NimRTCEngine::feed_media_inbound(const std::uint8_t* pkt,
                                      std::size_t len) noexcept {
    if (!pkt || len == 0) return;
    // Same post-close guard as on_transport_recv(): a harness may deliver
    // a buffered packet after close() has torn down the state the
    // handlers below read.
    if (!is_open()) return;
    // tried_srtp = true: no SRTP attempt happened on this path, so bytes
    // that parse as neither RTP nor RTCP are a malformed packet rather than
    // "probably STUN handled by libjuice". Skip that misleading debug line.
    dispatch_media_inbound(plugins::BufferView(pkt, len), /*tried_srtp=*/true);
}


int NimRTCEngine::drain_dtls() noexcept {
    auto d = impl_->dtls.lock();
    if (!d) return 0;

    // Drive the wolfSSL retransmit timer (RFC 6347 §4.2.4).  Without
    // this pump, a stalled handshake (e.g. lost HelloVerifyRequest
    // cookie exchange) hangs forever.  tick() is a no-op once the
    // session reaches Connected/Failed/Closed.
    d->tick();

    auto recs = d->take_outbound();
    int sent = 0;
    for (auto& rec : recs) {
        plugins::BufferView bv{rec.bytes.data(), rec.bytes.size()};
        if (ice_t_->send(bv, {}) == plugins::kOk) ++sent;
    }

    // Trace DTLS state transitions for debuggability.  Uses a per-engine
    // member (not a static local) so two engines in the same process
    // (e.g. the loopback test) log their transitions independently.
    const auto new_state = d->state();
    if (new_state != impl_->last_dtls_state) {
        core::log::Logger::instance().info(
            "engine[dtls]: state transition " +
            std::string(nimrtc::dtls::DtlsSession::state_name(impl_->last_dtls_state)) + " → " +
            std::string(nimrtc::dtls::DtlsSession::state_name(new_state)));
        impl_->last_dtls_state = new_state;
    }

    if (new_state == nimrtc::dtls::DtlsState::Connected) {
        // SRTP key installation happens here — this is the critical
        // handshake boundary the P1 interop tests verify.
        core::log::Logger::instance().info(
            "engine[dtls→srtp]: DTLS reached Connected, "
            "calling maybe_install_srtp_keys()");
        maybe_install_srtp_keys();
    }
    return sent;
}


void NimRTCEngine::maybe_install_srtp_keys() noexcept {
    // Idempotency: only install once per DTLS connection.
    if (impl_->srtp_installed) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: already installed, skipping");
        return;
    }
    auto d = impl_->dtls.lock();
    if (!d) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: dtls_ is null, skipping");
        return;
    }
    if (!impl_->srtp) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: srtp_ is null, skipping");
        return;
    }

    // Pre-condition: DTLS MUST be Connected (RFC 5764 §4.2 — keying material
    // is only available after the Finished message).  Re-check inside this
    // function to avoid races between drain_dtls()'s state read and our own.
    const auto dtls_state = d->state();
    if (dtls_state != nimrtc::dtls::DtlsState::Connected) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: dtls_state=" +
            std::to_string(static_cast<int>(dtls_state)) +
            " (not Connected, skipping key install)");
        return;
    }

    auto km = d->srtp_keying_material();
    if (!km) {
        core::log::Logger::instance().warn(
            "engine[dtls→srtp]: dtls reports Connected but no keying material");
        return;
    }

    // RFC 5764 §4.2 — DTLS-SRTP key derivation produces:
    //   client_master_key || client_master_salt || server_master_key || server_master_salt
    // Each side uses its OWN direction's keys for outbound, the peer's for
    // inbound.  Which half is "ours" depends on the DTLS role we ended up
    // playing (RFC 5763 §5):
    //
    //   we are the DTLS **Client** → we send the ClientHello, so the
    //     *client* master key is ours: client_* for outbound, server_* for
    //     inbound.
    //   we are the DTLS **Server** → the peer sends the ClientHello, so the
    //     *server* master key is ours: server_* for outbound, client_* for
    //     inbound.
    //
    // The previous code hard-coded the server assumption ("NimRTC uses
    // client_master_key for inbound, we are the controlled/answerer"),
    // which silently produced undecryptable inbound traffic whenever the
    // negotiation made NimRTC the client — the case a NimRTC-to-NimRTC
    // session hits.
    auto& log = core::log::Logger::instance();
    log.info("engine[dtls→srtp]: derived keying material:");
    log.info("  - profile        = 0x" + std::to_string(static_cast<int>(km->profile)));
    log.info("  - key_len        = " + std::to_string(km->client_master_key.size()));
    log.info("  - salt_len       = " + std::to_string(km->client_master_salt.size()));
    log.info("  - lifetime       = " + std::to_string(km->lifetime));
    log.info("  - dtls role      = " +
             std::string(impl_->dtls_is_client ? "Client" : "Server"));

    // The keying material is a set of fixed-size std::array members, so
    // the four views below are *spans into the optional's storage* — the
    // `std::optional` local outlives every use of them.  (A ternary of
    // two arrays would yield a prvalue copy whose lifetime the span
    // cannot extend; binding each branch explicitly avoids that.)
    const std::span<const std::uint8_t> own_key =
        impl_->dtls_is_client
            ? std::span<const std::uint8_t>(km->client_master_key)
            : std::span<const std::uint8_t>(km->server_master_key);
    const std::span<const std::uint8_t> own_salt =
        impl_->dtls_is_client
            ? std::span<const std::uint8_t>(km->client_master_salt)
            : std::span<const std::uint8_t>(km->server_master_salt);
    const std::span<const std::uint8_t> peer_key =
        impl_->dtls_is_client
            ? std::span<const std::uint8_t>(km->server_master_key)
            : std::span<const std::uint8_t>(km->client_master_key);
    const std::span<const std::uint8_t> peer_salt =
        impl_->dtls_is_client
            ? std::span<const std::uint8_t>(km->server_master_salt)
            : std::span<const std::uint8_t>(km->client_master_salt);

    if (!own_key.empty()) {
        log.info("  - own master_key[0..7] = " + [&]{
            std::string s;
            for (std::size_t i = 0; i < std::min<std::size_t>(8, own_key.size()); ++i) {
                char buf[8]; std::snprintf(buf, sizeof(buf), "%02x", own_key[i]);
                s += buf;
                if (i + 1 < std::min<std::size_t>(8, own_key.size())) s += ":";
            }
            return s;
        }());
    }

    // Install the PEER's keys for INBOUND (decrypting the peer's packets).
    impl_->srtp->derive_keys_for_remote(peer_key, peer_salt,
                                        srtp::CryptoSuite::Aes128CmSha1_80);

    // Install our OWN keys for OUTBOUND (encrypting our packets).
    if (!own_key.empty() && !own_salt.empty()) {
        impl_->srtp->derive_keys_for_local(own_key, own_salt,
                                            srtp::CryptoSuite::Aes128CmSha1_80);
    } else {
        core::log::Logger::instance().warn(
            "engine[dtls→srtp]: own master_key/salt missing — outbound SRTP will fail");
    }

    // Mark installed; subsequent calls will short-circuit.
    impl_->srtp_installed = true;

    log.info("engine[dtls→srtp]: SRTP keys INSTALLED ✓ "
             "(state=" + std::string(state_ == State::kOpen ? "Open" : "Other") +
             ") — RTP from peer will now be unprotectable");
}


uint32_t NimRTCEngine::feed_srtp_inbound(const std::uint8_t* srtp_packet, std::size_t len) noexcept {
    if (!srtp_packet || len == 0 || !impl_->srtp) return core::kEngineInvalidParam;
    auto sess = impl_->srtp->get_session(0, /*outgoing=*/false);
    if (!sess) return 0;
    core::ByteSpan span(srtp_packet, len);
    auto un = sess->unprotect_rtp(span, nullptr, nullptr);
    return un ? 0 : 0xDEAD;
}

}  // namespace nimrtc::engine
