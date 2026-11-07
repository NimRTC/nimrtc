/**
 * @file src/modules/audio_rx/src/receiver_plugin.cpp
 * @brief The `plugins::IAudioReceiver` adapter of the audio receive
 *        pipeline (v1.2.0, audio pipeline WP-3).
 *
 * The pipeline itself lives in `receiver.cpp`; this file is the seam
 * side of it:
 *
 *   * `Receiver` implements `plugins::IAudioReceiver` and forwards every
 *     call into the internal `Pipeline`;
 *   * `ReceiverFactory` implements `plugins::IAudioReceiverFactory` and
 *     registers under the id **"nimrtc_audio_rx"** — the id the
 *     specification's V-1 row "Registry slots" names;
 *   * `register_default_plugins()` is the module's entry point, which the
 *     core registrar table calls (PAL Slice 2).
 *
 * ## Why the factory returns `nullptr`
 *
 * The pipeline resolves its jitter buffer and its decoder through
 * `core::PluginRegistry` at open time.  When one of them is missing the
 * pipeline cannot work at all, and spec B-16 says there is **no fallback
 * receiver**: the caller gets a null factory product and turns it into a
 * hard error (`kAudioReceiverMissing`) that fires `on_error` exactly once
 * and disables the audio track.  Returning the null pointer from
 * `create()` is what makes that policy reachable.
 *
 * The callbacks are installed *after* `create()`, so the pipeline cannot
 * report the missing plugin through its own error callback at that point —
 * which is precisely why the factory turns the condition into a null
 * product instead of relying on the callback.
 */

#include "receiver_internal.hpp"

#include <new>
#include <utility>

#include <nimrtc/core/plugin_id.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::audio_rx {

// ---------------------------------------------------------------------------
// Receiver — the plugins::IAudioReceiver implementation
// ---------------------------------------------------------------------------

/**
 * @brief The audio receive pipeline behind `plugins::IAudioReceiver`.
 *
 * The class is declared (opaquely) in the module's public header; its
 * definition lives here because nothing outside the module may depend on
 * the pipeline's internals.
 */
class Receiver : public plugins::IAudioReceiver {
public:
    explicit Receiver(plugins::AudioReceiverConfig cfg) noexcept
        : cfg_(std::move(cfg))
        , pipeline_(cfg_) {}

    ~Receiver() override {
        close();
    }

    const char* name() const noexcept override {
        return "nimrtc::audio_rx::Receiver "
               "(audio pipeline behind plugins::IAudioReceiver)";
    }

    plugins::Status open() noexcept override {
        if (opened_) return plugins::kOk;
        if (!pipeline_.open()) return plugins::kErrNotReady;
        opened_ = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        pipeline_.close();
        opened_ = false;
    }

    void set_callbacks(FrameCallback on_frame,
                       ErrorCallback on_error) noexcept override {
        pipeline_.set_frame_callback(std::move(on_frame));
        pipeline_.set_error_callback(std::move(on_error));
    }

    void push(const std::uint8_t* payload, std::size_t payload_len,
              std::uint32_t rtp_timestamp, std::uint16_t seq,
              std::uint32_t ssrc,
              plugins::TimestampUs recv_time_us) noexcept override {
        pipeline_.push(payload, payload_len, rtp_timestamp, seq, ssrc, recv_time_us);
    }

    void tick(plugins::TimestampUs now_us) noexcept override {
        pipeline_.tick(now_us);
    }

    void reset_stream(std::uint32_t ssrc) noexcept override {
        pipeline_.reset_stream(ssrc);
    }

    Stats stats() const noexcept override {
        return pipeline_.stats();
    }

private:
    /** The configuration the caller asked for (kept for the factory's
     *  diagnostics and for the renegotiation path). */
    plugins::AudioReceiverConfig cfg_;
    /** The pipeline implementation. */
    Pipeline pipeline_;
    /** Whether the pipeline resolved its injected plugins. */
    bool opened_ = false;
};

// ---------------------------------------------------------------------------
// ReceiverFactory
// ---------------------------------------------------------------------------

namespace {

class ReceiverFactory : public plugins::IAudioReceiverFactory {
public:
    std::string_view id() const noexcept override {
        return NIMRTC_PLUGIN_ID("nimrtc_audio_rx");
    }

    std::string_view display_name() const noexcept override {
        return "NimRTC audio receive pipeline "
               "(injected jitter buffer + injected decoder)";
    }

    plugins::IAudioReceiver* create(
        const plugins::AudioReceiverConfig& cfg) const override {
        auto* receiver = new (std::nothrow) Receiver(cfg);
        if (!receiver) return nullptr;

        if (receiver->open() != plugins::kOk) {
            // The jitter buffer or the decoder plugin is not registered
            // (B-16): there is no fallback receiver, so the caller gets a
            // null product and reports the hard error itself.
            delete receiver;
            return nullptr;
        }
        return receiver;
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

namespace detail {

void do_register_audio_receiver_plugin() noexcept {
    static const struct Registrar {
        Registrar() {
            static nimrtc::audio_rx::ReceiverFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_audio_receiver(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

void register_default_plugins() noexcept {
    detail::do_register_audio_receiver_plugin();
}

} // namespace nimrtc::audio_rx
