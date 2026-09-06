/**
 * @file nimrtc/video_source/video_source_plugin.hpp
 * @brief PluginAdapter — wraps a concrete `nimrtc::video_source::IVideoSource`
 *        so it can be looked up by string ID through `core::PluginRegistry`.
 *
 * Implements `plugins::IVideoSource` by delegating to an owned concrete impl
 * (currently `MemoryVideoSource` produced by `create_memory_source()`).
 *
 * ## Adapter vs direct implementation
 *
 * The concrete `video_source::IVideoSource` API uses a refcounted
 * `video_frame::VideoFrameBuffer` + `VideoFrameInfo` callback. The plugin
 * API uses a POD view (`plugins::VideoSourceFrame`). A small adapter class
 * keeps the two APIs decoupled without forcing concrete modules to emit
 * plugin-shaped callbacks they don't natively produce.
 *
 * ## Registration
 *
 * `MemoryPluginFactory` is registered under id `"memory"`. When a real
 * camera / screen / file source ships, a separate factory should register
 * under a different id (`"camera_v4l2"`, `"screen"`, ...); users then pick
 * by setting `EngineConfig::video_source_name` accordingly.
 *
 * ## Important: explicit-registration required (MSVC quirk)
 *
 * MSVC's static linker strips anonymous-namespace `static` global
 * initializers from a .lib when no symbol from that .obj is ODR-used by
 * the consumer .obj. Callers must therefore explicitly invoke
 * `nimrtc::video_source::register_default_plugins()` once at startup
 * before any `core::PluginRegistry::instance()` lookup of video sources.
 * (Same MSVC quirk as `audio3a::register_default_plugins()` —
 * documented in audio3a_plugin.hpp's preamble.)
 *
 * @note P1 (R2-Batch2). Adapter only — engine refactor (drop direct
 *       concrete includes) is a separate step.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/video_source/video_source.hpp>     // concrete IVideoSource / SourceConfig
#include <nimrtc/plugins/video_source.hpp>          // plugins::IVideoSource / VideoSourceConfig

namespace nimrtc::video_source {

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

/**
 * @brief Wraps a concrete `video_source::IVideoSource` so it can be
 *        exposed via the `plugins::IVideoSource` interface and looked up
 *        by string ID through `core::PluginRegistry`.
 *
 * Ownership: takes ownership of the concrete impl pointer (passed via
 * set_impl() before open()); deletes it on destruction. If no impl is
 * provided, a `MemoryVideoSource` is created lazily on demand.
 *
 * Thread safety of the concrete impl is the same as `video_source::IVideoSource`:
 * caller must serialize `start` / `stop` / `produce_one` calls. The
 * adapter's own state (callback storage) is single-threaded.
 */
class PluginAdapter : public plugins::IVideoSource {
public:
    /** Construct adapter wrapping an existing concrete impl.
     *  The adapter takes ownership. Pass nullptr to delay creation. */
    explicit PluginAdapter(std::unique_ptr<video_source::IVideoSource> impl) noexcept;
    ~PluginAdapter() override;

    PluginAdapter(const PluginAdapter&)            = delete;
    PluginAdapter& operator=(const PluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------
    const char*       name()  const noexcept override;
    plugins::Status   open()        noexcept override;
    void              close()       noexcept override;

    // ---- plugins::IVideoSource --------------------------------------------
    plugins::Status   start() noexcept override;
    void              stop() noexcept override;
    bool              running() const noexcept override;
    void              produce_one() noexcept override;
    plugins::VideoSourceConfig config() const noexcept override;
    void              set_pattern(plugins::VideoSourcePattern p) noexcept override;
    void              set_callback(plugins::VideoSourceFrameCallback cb) noexcept override;
    plugins::IVideoSource::Stats stats() const noexcept override;

private:
    std::unique_ptr<video_source::IVideoSource>   concrete_;
    plugins::VideoSourceFrameCallback             user_cb_;
    bool                                          registered_with_concrete_ = false;
    bool                                          opened_                    = false;
};

// ---------------------------------------------------------------------------
// MemoryPluginFactory
// ---------------------------------------------------------------------------

/** Factory for the in-memory test-pattern source. Registers under id
 *  `"memory"` (the default for `EngineConfig::video_source_name`). */
class MemoryPluginFactory : public plugins::IVideoSourceFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;

    /** Create an adapter owning a fresh MemoryVideoSource. The adapter is
     *  in kConstructed state — caller must call `open()` then either
     *  `set_callback()` followed by `start()` (cadence-driven) or
     *  `produce_one()` (step-driven). */
    plugins::IVideoSource*
        create(plugins::VideoSourceConfig cfg) const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

/** Register the default video_source plugin(s) with core::PluginRegistry.
 *  Must be called once at program startup before any video-source lookup.
 *  The first invocation registers; subsequent invocations are no-ops
 *  (idempotent via a Meyer's-singleton latch). */
void register_default_plugins() noexcept;

} // namespace nimrtc::video_source
