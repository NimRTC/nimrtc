/**
 * @file src/engine/tests/test_engine_render_policy.cpp
 * @brief The v1-video-render-engine (D2) deterministic tests.
 *
 * ## What this file proves
 *
 * The slice has two behaviours that must be pinned with code:
 *
 *   1. **The render policy (D2 resolution).**  `detail::default_render_policy()`
 *      returns the per-platform table (Windows `d3d11` / macOS `metal` /
 *      Linux `egl`, fallback `"headless"`), and `resolve_auto_sink_backend()`
 *      resolves `"auto"` to the policy's preferred id only when that id is
 *      registered — otherwise it returns the empty string so the caller can
 *      degrade (a recommendation, not a requirement).
 *
 *   2. **The engine wiring.**  Driving a real `NimRTCEngine` (shell
 *      transport, no codec) with fake video sinks proves the public value
 *      table: the default `video_sink_name == "auto"`, `""` is headless
 *      verbatim, `"none"` installs no sink, `"headless"` is the explicit
 *      spelling, an unknown explicit id fails `open()` with
 *      `kVideoSinkPluginMissing`, an explicit id whose `open()` fails is a
 *      hard error (`kVideoSinkOpenFailed`), and — the heart of the slice —
 *      `"auto"` resolves to the platform renderer and *degrades* to
 *      `"headless"` (with `video_sink_backend_reason` set) instead of
 *      failing the session.  All of it is observable through the two new
 *      `Stats` fields.
 *
 * The private header `engine_render_policy.hpp` is included directly so the
 * rule and the test are the same rule.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_sink.hpp>

#include "engine_render_policy.hpp"

namespace {

namespace np = nimrtc::plugins;
namespace nc = nimrtc::core;
namespace nd = nimrtc::engine::detail;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// A video sink whose open() the test controls.
//
// The engine only ever reaches the sink through `plugins::IVideoSink`
// (Layout Invariant 4) — never through a concrete `nimrtc::video_sink`
// header — so the fake is seam-only, the same pattern the device-policy
// test uses for capture devices.
// ---------------------------------------------------------------------------

class FakeSink final : public np::IVideoSink {
public:
    FakeSink(np::VideoSinkConfig cfg, bool open_ok)
        : cfg_(std::move(cfg)), open_ok_(open_ok) {}

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "FakeSink"; }
    np::Status open() noexcept override {
        return open_ok_ ? np::kOk : np::kErrNotReady;
    }
    void close() noexcept override {}

    // ---- IVideoSink --------------------------------------------------------
    np::VideoSinkStats stats() const noexcept override { return np::VideoSinkStats{}; }
    np::VideoSinkConfig config() const noexcept override { return cfg_; }

private:
    np::VideoSinkConfig cfg_;
    bool open_ok_;
};

class FakeSinkFactory final : public np::IVideoSinkFactory {
public:
    FakeSinkFactory(std::string id, bool open_ok)
        : id_(std::move(id)), open_ok_(open_ok) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Fake video sink (render-policy test)";
    }
    np::IVideoSink* create(np::VideoSinkConfig cfg) const override {
        return new FakeSink(std::move(cfg), open_ok_);
    }

private:
    std::string id_;
    bool open_ok_;
};

// ---------------------------------------------------------------------------
// The engine fixture: register the default plugins, then register fake sink
// factories — INCLUDING under the policy's preferred backend id, so the
// "auto" tests resolve deterministically to the fakes on every platform
// instead of opening a real D3D11 / Metal / EGL device.
// ---------------------------------------------------------------------------

class EngineRenderPolicy : public ::testing::Test {
protected:
    static const FakeSinkFactory& platform_ok() {
        static const FakeSinkFactory f(
            std::string(nd::default_render_policy().preferred_backend),
            /*open_ok=*/true);
        return f;
    }
    static const FakeSinkFactory& platform_failing() {
        static const FakeSinkFactory f(
            std::string(nd::default_render_policy().preferred_backend),
            /*open_ok=*/false);
        return f;
    }
    static const FakeSinkFactory& explicit_ok() {
        static const FakeSinkFactory f("render-policy-explicit-ok", true);
        return f;
    }
    static const FakeSinkFactory& explicit_fail() {
        static const FakeSinkFactory f("render-policy-explicit-fail", false);
        return f;
    }

    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        // Registering under the policy's preferred id OVERWRITES the real
        // platform renderer for this process — the "auto" tests must not
        // open a real window / swap chain.  On a build with no platform
        // default (preferred id empty) there is nothing to overwrite.
        if (!platform_ok().id().empty()) {
            PluginRegistry::instance().register_video_sink(
                platform_ok().id(), &platform_ok());
        }
        PluginRegistry::instance().register_video_sink(
            explicit_ok().id(), &explicit_ok());
        PluginRegistry::instance().register_video_sink(
            explicit_fail().id(), &explicit_fail());
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_strategy_name = "shell";
        cfg.video_width  = 64;
        cfg.video_height = 48;
        cfg.datachannel_backend = "";
        cfg.bwe_name            = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }
};

// ===========================================================================
// 1. The default render policy (D2 resolution)
// ===========================================================================

TEST(RenderPolicy, DefaultPolicyMatchesPlatformTable) {
    const nd::RenderPolicy p = nd::default_render_policy();
#if defined(_WIN32)
    EXPECT_EQ(p.preferred_backend, "d3d11");
#elif defined(__APPLE__)
    EXPECT_EQ(p.preferred_backend, "metal");
#elif defined(__linux__)
    EXPECT_EQ(p.preferred_backend, "egl");
#else
    // A non-desktop build has no platform renderer; "auto" then degrades
    // (the engine's "no-platform-backend" case).
    EXPECT_EQ(p.preferred_backend, "");
#endif
    EXPECT_EQ(p.fallback_backend, "headless");
}

TEST(RenderPolicy, AutoResolvesToRegisteredPreferredBackend) {
    const nd::RenderPolicy policy = nd::default_render_policy();
    const auto registered = [](std::string_view) noexcept { return true; };
    EXPECT_EQ(nd::resolve_auto_sink_backend(policy, registered),
              policy.preferred_backend);
}

TEST(RenderPolicy, PreferredBackendNotRegisteredResolvesEmpty) {
    // D2-7: "auto" pointing at a backend this build does not register is
    // NOT an error — it resolves to the empty string and the caller
    // degrades (a recommendation, not a requirement).
    const nd::RenderPolicy policy = nd::default_render_policy();
    const auto unregistered = [](std::string_view) noexcept { return false; };
    EXPECT_EQ(nd::resolve_auto_sink_backend(policy, unregistered), "");
}

TEST(RenderPolicy, EmptyPreferredBackendResolvesNothing) {
    // D2-9: a policy with no platform default (preferred empty) can never
    // resolve — the caller degrades with "no-platform-backend".
    nd::RenderPolicy policy;  // preferred_backend stays ""
    const auto registered = [](std::string_view) noexcept { return true; };
    EXPECT_EQ(nd::resolve_auto_sink_backend(policy, registered), "");
}

// ===========================================================================
// 2. The value table, driven through a real engine
// ===========================================================================

TEST_F(EngineRenderPolicy, DefaultVideoSinkNameIsAuto) {
    // D2-1: the new default is "auto".
    EngineConfig cfg{};
    EXPECT_EQ(cfg.video_sink_name, "auto");
}

TEST_F(EngineRenderPolicy, EmptyNameIsHeadlessVerbatim) {
    // D2-2: "" is today's behaviour, verbatim — the headless sink.
    EngineConfig cfg = make_config();
    cfg.video_sink_name = "";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_NE(engine.video_sink(), nullptr);
    EXPECT_EQ(engine.stats().video_sink_backend, "headless");
    EXPECT_EQ(engine.stats().video_sink_backend_reason, "");
    engine.close();
}

TEST_F(EngineRenderPolicy, NoneInstallsNoSink) {
    // D2-3: "none" is an explicit opt-out — no sink instance at all.
    EngineConfig cfg = make_config();
    cfg.video_sink_name = "none";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.video_sink(), nullptr);
    EXPECT_EQ(engine.stats().video_sink_backend, "");
    EXPECT_EQ(engine.stats().video_sink_backend_reason, "");
    engine.close();
}

TEST_F(EngineRenderPolicy, HeadlessIsTheExplicitSpelling) {
    // D2-4: "headless" is the explicit spelling of the "" behaviour.
    EngineConfig cfg = make_config();
    cfg.video_sink_name = "headless";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_NE(engine.video_sink(), nullptr);
    EXPECT_EQ(engine.stats().video_sink_backend, "headless");
    EXPECT_EQ(engine.stats().video_sink_backend_reason, "");
    engine.close();
}

TEST_F(EngineRenderPolicy, UnknownExplicitIdFailsOpen) {
    // D2-5: an explicit id that is not in the registry is a hard error,
    // never a silent degrade.
    EngineConfig cfg = make_config();
    cfg.video_sink_name = "no-such-sink";
    NimRTCEngine engine(cfg);
    EXPECT_EQ(engine.open(), nc::kVideoSinkPluginMissing);
    engine.close();
}

TEST_F(EngineRenderPolicy, ExplicitOpenFailureIsHardError) {
    // D2-12: an explicit id whose open() fails is a hard error
    // (kVideoSinkOpenFailed) — the opposite of "auto", which degrades.
    EngineConfig cfg = make_config();
    cfg.video_sink_name = "render-policy-explicit-fail";
    NimRTCEngine engine(cfg);
    EXPECT_EQ(engine.open(), nc::kVideoSinkOpenFailed);
    engine.close();
}

// ===========================================================================
// 3. "auto": resolve to the platform renderer, or degrade
// ===========================================================================

TEST_F(EngineRenderPolicy, AutoResolvesToThePlatformBackend) {
    // D2-6 / D2-10: on a desktop build "auto" resolves to the policy's
    // preferred id, the sink is that backend, and the reason stays empty.
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    const nd::RenderPolicy policy = nd::default_render_policy();
    EngineConfig cfg = make_config();
    cfg.video_sink_name = "auto";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "\"auto\" must never fail open()";
    ASSERT_NE(engine.video_sink(), nullptr)
        << "\"auto\" must always install a sink (degrade keeps one)";
    // The installed sink is the fake registered under the policy's
    // preferred id — proof that "auto" resolved through the render policy
    // rather than hard-coding a backend.
    ASSERT_NE(dynamic_cast<FakeSink*>(engine.video_sink()), nullptr);
    EXPECT_EQ(engine.stats().video_sink_backend, policy.preferred_backend);
    EXPECT_EQ(engine.stats().video_sink_backend_reason, "");
    engine.close();
#endif
}

TEST_F(EngineRenderPolicy, AutoBackendOpenFailureDegradesToHeadless) {
    // D2-8: when the platform renderer's open() fails, "auto" degrades to
    // headless with a warn — open() succeeds and the reason is observable.
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    const std::string preferred =
        std::string(nd::default_render_policy().preferred_backend);
    // Swap the OK fake for a failing one under the same preferred id, then
    // restore it so the tests that run after this one see the OK fake.
    PluginRegistry::instance().register_video_sink(
        preferred, &platform_failing());

    EngineConfig cfg = make_config();
    cfg.video_sink_name = "auto";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "\"auto\" must never fail open()";
    ASSERT_NE(engine.video_sink(), nullptr)
        << "the degrade must still install the headless sink";
    EXPECT_EQ(engine.stats().video_sink_backend, "headless");
    EXPECT_EQ(engine.stats().video_sink_backend_reason, "backend-open-failed");
    engine.close();

    PluginRegistry::instance().register_video_sink(preferred, &platform_ok());
#endif
}

// ===========================================================================
// 4. Stats observability
// ===========================================================================

TEST_F(EngineRenderPolicy, StatsReportTheActualBackendId) {
    // D2-18: Stats::video_sink_backend reports the id actually installed,
    // including for an explicit non-platform id.
    EngineConfig cfg = make_config();
    cfg.video_sink_name = "render-policy-explicit-ok";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.stats().video_sink_backend, "render-policy-explicit-ok");
    EXPECT_EQ(engine.stats().video_sink_backend_reason, "")
        << "a non-degraded session must have an empty reason";
    engine.close();
}

}  // namespace
