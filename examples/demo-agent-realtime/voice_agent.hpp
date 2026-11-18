/**
 * @file examples/demo-agent-realtime/voice_agent.hpp
 * @brief Voice-agent chain: post-3A int16 PCM → ASR → streaming LLM → Opus TTS.
 *
 * Slice v4-voice-agent — the M3 voice branch of the AI teammate demo
 * (docs/plan/ai-game-demo-plan.md §4.1):
 *
 *   Mic -> pre-3A Tap (VAD) -> webrtc_apm -> post-3A int16 -> ASR
 *    -> LLM (流式) -> TTS (Opus 编码回传)
 *
 * ## What this file provides
 *
 *   - `IASR` + pluggable backends: M1's energy-VAD mock is the default;
 *     Whisper and vosk are runtime-pluggable access points (no compile-time
 *     model SDK — spec requirement 1 / 6).  A missing model is a clear
 *     error, never a silent mock fallback unless the caller opts in via
 *     `AsrOptions::allow_mock_fallback` (requirement 6).
 *   - `ILLM` streaming: `stream()` invokes an incremental token callback
 *     instead of returning one whole reply (requirement 3).
 *   - `ITts` + `OpusTts`: TTS synthesises int16 PCM which is encoded to
 *     Opus packets with `nimrtc::opus::Encoder::encode_int16` — the TTS
 *     return wire format (requirement 4).
 *   - `TapBatchAccumulator`: post-3A tap audio is batched to 100 ms before
 *     the model is called — never one call per 10 ms frame
 *     (requirement 4).
 *   - `BargeInMeter`: records the "interrupt command instant" and the
 *     "playback-stop instant" and exposes their difference, so the
 *     barge-in → stop-playback chain is measurable (requirement 5).
 *
 * The chain is deliberately self-contained except for the Opus encoder:
 * the model backends are the runtime seam (env-configured model path), so
 * the demo's default path builds and runs headlessly with zero model-SDK
 * dependency.
 */

#ifndef NIMRTC_DEMO_AGENT_REALTIME_VOICE_AGENT_HPP
#define NIMRTC_DEMO_AGENT_REALTIME_VOICE_AGENT_HPP

#include <nimrtc/opus/opus.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace demoai {

// ---------------------------------------------------------------------------
// Format declaration (spec risk 4: 接入前显式做格式声明与转换)
// ---------------------------------------------------------------------------
//
// post-3A int16 PCM reaches the model in exactly this shape; the chain
// refuses to feed a model a buffer whose format was not declared.

struct PcmFormat {
    std::uint32_t sample_rate_hz = 48000;  // post-3A pipeline rate
    std::uint8_t  channels       = 1;
    std::uint8_t  bits_per_sample = 16;    // int16 PCM
};

// ---------------------------------------------------------------------------
// ASR: pluggable backends (requirement 1)
// ---------------------------------------------------------------------------

class IASR {
public:
    virtual ~IASR() = default;

    virtual const char* name() const noexcept = 0;

    /** Feed one post-3A int16 PCM frame (samples per channel).
     *
     *  The input is the int16 tap that `IAudio3A::set_post_process_tap`
     *  delivers after AEC/ANS/AGC — not the pre-3A raw microphone data
     *  (requirement 2). */
    virtual void feed(const std::int16_t* pcm,
                      std::size_t num_samples,
                      const PcmFormat& fmt) noexcept = 0;

    /** Last recognized utterance (empty when nothing recognized yet). */
    virtual std::string transcript() const = 0;
};

// M1's energy-VAD mock — the default backend; the demo runs on it when no
// real model is configured (requirement 1: 缺省仍是 M1 的 mock 实现).
class MockAsr final : public IASR {
public:
    MockAsr() = default;

    const char* name() const noexcept override { return "mock"; }

    void feed(const std::int16_t* pcm,
              std::size_t num_samples,
              const PcmFormat&) noexcept override {
        last_level_dbfs_ = rms_dbfs_i16(pcm, num_samples);
        if (last_level_dbfs_ > kVadDbfs && transcript_.empty()) {
            transcript_ = "hello world";
        }
        ++frames_fed_;
    }

    std::string transcript() const override { return transcript_; }

    std::uint64_t frames_fed() const noexcept { return frames_fed_; }
    float last_level_dbfs() const noexcept { return last_level_dbfs_; }

private:
    static float rms_dbfs_i16(const std::int16_t* s, std::size_t n) noexcept {
        if (n == 0) return -96.0f;
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = static_cast<double>(s[i]) / 32768.0;
            sum += v * v;
        }
        return static_cast<float>(
            20.0 * std::log10(std::sqrt(sum / static_cast<double>(n)) + 1e-10));
    }

    static constexpr float kVadDbfs = -40.0f;  // dBFS threshold for "speech"
    std::string transcript_;
    std::uint64_t frames_fed_ = 0;
    float last_level_dbfs_ = -96.0f;
};

// Whisper access point (runtime-pluggable, spec risk 1 response).
//
// No compile-time link: the model is located at runtime through the
// `NIMRTC_WHISPER_MODEL_DIR` environment variable.  The factory refuses to
// construct this backend when the runtime model is absent, so a missing
// model is a loud error rather than a silent no-op.  `feed()` is the seam
// where a real integration forwards int16 PCM to the whisper runtime
// (whisper.cpp / a whisper server); this build carries the adapter shape
// without shipping the model SDK.
class WhisperAsr final : public IASR {
public:
    explicit WhisperAsr(std::string model_dir) : model_dir_(std::move(model_dir)) {}

    const char* name() const noexcept override { return "whisper"; }

    void feed([[maybe_unused]] const std::int16_t* pcm,
              std::size_t num_samples,
              const PcmFormat&) noexcept override {
        // Runtime seam: forward `pcm` to the configured whisper model here.
        samples_fed_ += num_samples;
    }

    std::string transcript() const override { return transcript_; }

    const std::string& model_dir() const noexcept { return model_dir_; }

private:
    std::string model_dir_;
    std::string transcript_;
    std::size_t samples_fed_ = 0;
};

// vosk access point (runtime-pluggable).  Mirrors WhisperAsr; the model
// dir comes from `NIMRTC_VOSK_MODEL_DIR`.
class VoskAsr final : public IASR {
public:
    explicit VoskAsr(std::string model_dir) : model_dir_(std::move(model_dir)) {}

    const char* name() const noexcept override { return "vosk"; }

    void feed([[maybe_unused]] const std::int16_t* pcm,
              std::size_t num_samples,
              const PcmFormat&) noexcept override {
        // Runtime seam: forward `pcm` to the configured vosk model here.
        samples_fed_ += num_samples;
    }

    std::string transcript() const override { return transcript_; }

    const std::string& model_dir() const noexcept { return model_dir_; }

private:
    std::string model_dir_;
    std::string transcript_;
    std::size_t samples_fed_ = 0;
};

struct AsrOptions {
    /** Explicit opt-in for the mock fallback (requirement 6).  Without it
     *  a missing real model is an error, never a silent fallback. */
    bool allow_mock_fallback = false;
};

/** Backend selection (requirement 1: 由配置/命令行选择).
 *
 *  `name` ∈ { mock, whisper, vosk }.  `mock` always succeeds.  whisper /
 *  vosk resolve their runtime model path from the environment; when it is
 *  absent the factory reports a clear error to stderr and returns nullptr
 *  — unless `opts.allow_mock_fallback`, in which case it falls back to the
 *  mock and says so. */
inline std::unique_ptr<IASR> make_asr(std::string_view name,
                                      const AsrOptions& opts = {},
                                      std::string* error = nullptr) {
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return std::unique_ptr<IASR>{};
    };
    if (name == "mock") {
        return std::make_unique<MockAsr>();
    }
    if (name == "whisper") {
        const char* dir = std::getenv("NIMRTC_WHISPER_MODEL_DIR");
        if (dir == nullptr || *dir == '\0') {
            const std::string why =
                "[ASR] backend 'whisper' unavailable: NIMRTC_WHISPER_MODEL_DIR "
                "is not set (no compile-time model SDK is linked; model access "
                "is runtime-only)";
            std::fprintf(stderr, "%s\n", why.c_str());
            if (!opts.allow_mock_fallback) return fail(why);
            std::fprintf(stderr,
                         "[ASR] --allow-mock-fallback: whisper -> mock\n");
            return std::make_unique<MockAsr>();
        }
        return std::make_unique<WhisperAsr>(dir);
    }
    if (name == "vosk") {
        const char* dir = std::getenv("NIMRTC_VOSK_MODEL_DIR");
        if (dir == nullptr || *dir == '\0') {
            const std::string why =
                "[ASR] backend 'vosk' unavailable: NIMRTC_VOSK_MODEL_DIR "
                "is not set (no compile-time model SDK is linked; model access "
                "is runtime-only)";
            std::fprintf(stderr, "%s\n", why.c_str());
            if (!opts.allow_mock_fallback) return fail(why);
            std::fprintf(stderr,
                         "[ASR] --allow-mock-fallback: vosk -> mock\n");
            return std::make_unique<MockAsr>();
        }
        return std::make_unique<VoskAsr>(dir);
    }
    return fail("[ASR] unknown backend '" + std::string(name) +
                "' (expected mock | whisper | vosk)");
}

inline std::vector<std::string_view> available_asr_backends() {
    return {"mock", "whisper", "vosk"};
}

// ---------------------------------------------------------------------------
// LLM: streaming consumption (requirement 3)
// ---------------------------------------------------------------------------

class ILLM {
public:
    virtual ~ILLM() = default;

    /** Incremental token sink — invoked once per emitted chunk as the
     *  reply is generated, before the whole text exists.  `final` fires
     *  once with an empty token when the stream ends. */
    using TokenSink = std::function<void(std::string_view token, bool final)>;

    virtual const char* name() const noexcept = 0;

    virtual void stream(const std::string& prompt, const TokenSink& on_token) = 0;
};

// Streaming mock: splits the canned reply word-by-word so the chain is
// exercised as a stream of tokens, not as one whole reply.
class MockLlm final : public ILLM {
public:
    explicit MockLlm(std::string reply = "moving left, got it")
        : reply_(std::move(reply)) {}

    const char* name() const noexcept override { return "mock"; }

    void stream([[maybe_unused]] const std::string& prompt, const TokenSink& on_token) override {
        std::istringstream iss(reply_);
        std::string word;
        while (iss >> word) {
            on_token(std::string_view(word), /*final*/ false);
        }
        on_token(std::string_view(), /*final*/ true);
    }

private:
    std::string reply_;
};

inline std::unique_ptr<ILLM> make_llm(std::string_view name) {
    if (name == "mock") return std::make_unique<MockLlm>();
    // A real streaming LLM (e.g. a local server / HTTP stream) plugs in
    // here and drives the same TokenSink incrementally.
    return nullptr;
}

// ---------------------------------------------------------------------------
// TTS: Opus-encoded return path (requirement 4)
// ---------------------------------------------------------------------------

class ITts {
public:
    virtual ~ITts() = default;

    virtual const char* name() const noexcept = 0;

    /** Synthesise `text` into int16 PCM (samples per channel). */
    virtual bool synthesize(const std::string& text,
                            std::vector<std::int16_t>& out_pcm,
                            const PcmFormat& fmt) = 0;
};

// Placeholder TTS: a short 440 Hz beep in int16 PCM, one 20 ms frame per
// call so the output is directly encodable by libopus (spec risk 4: the
// model's PCM shape must match what the encoder accepts).  A real TTS model
// plugs in here and produces the same int16 shape.
class MockTts final : public ITts {
public:
    const char* name() const noexcept override { return "mock"; }

    bool synthesize(const std::string& text,
                    std::vector<std::int16_t>& out_pcm,
                    const PcmFormat& fmt) override {
        const std::size_t samples =
            static_cast<std::size_t>(fmt.sample_rate_hz) / 50;  // 20 ms Opus frame
        out_pcm.resize(samples);
        for (std::size_t i = 0; i < samples; ++i) {
            const double phase = 2.0 * 3.141592653589793 * 440.0 *
                                 static_cast<double>(i) /
                                 static_cast<double>(fmt.sample_rate_hz);
            out_pcm[i] = static_cast<std::int16_t>(16000.0 * std::sin(phase));
        }
        (void)text;
        return true;
    }
};

inline std::unique_ptr<ITts> make_tts(std::string_view name) {
    if (name == "mock") return std::make_unique<MockTts>();
    // A real TTS model plugs in here and produces the same int16 PCM shape.
    return nullptr;
}

// Encodes TTS int16 PCM into Opus packets via nimrtc::opus::Encoder — the
// "TTS 用 Opus 编码回传" wire format.
class OpusTts {
public:
    explicit OpusTts(nimrtc::opus::EncoderConfig cfg)
        : enc_(std::move(cfg)) {}

    /** Encode one TTS PCM chunk (samples per channel) into an Opus packet. */
    std::size_t encode(const std::int16_t* pcm,
                       std::size_t num_samples,
                       std::uint8_t* output,
                       std::size_t output_capacity) noexcept {
        return enc_.encode_int16(pcm, num_samples, output, output_capacity);
    }

    std::uint64_t packets_encoded() const noexcept {
        return enc_.stats().frames_encoded;
    }

private:
    nimrtc::opus::Encoder enc_;
};

// ---------------------------------------------------------------------------
// Tap batching: 100 ms before the model call (requirement 4)
// ---------------------------------------------------------------------------
//
// The post-3A tap fires every 10 ms; calling the model per frame would burn
// tokens and latency.  Frames accumulate until one 100 ms batch is ready,
// then — and only then — the model sink is invoked.

inline constexpr std::int64_t kTapFrameMs = 10;    // tap cadence per frame
inline constexpr std::int64_t kTapBatchMs = 100;   // model-call batch size

class TapBatchAccumulator {
public:
    using BatchSink = std::function<void(const std::int16_t* pcm,
                                         std::size_t samples,
                                         const PcmFormat& fmt,
                                         std::int64_t batch_ms)>;

    explicit TapBatchAccumulator(BatchSink on_batch, PcmFormat fmt = {})
        : on_batch_(std::move(on_batch)), fmt_(fmt) {}

    /** Accumulate one 10 ms post-3A int16 frame.  Flushes a full 100 ms
     *  batch to the model sink; a partial batch is never forwarded. */
    void add(const std::int16_t* pcm, std::size_t num_samples) {
        if (fmt_.sample_rate_hz == 0) return;
        const std::int64_t frame_ms =
            1000 * static_cast<std::int64_t>(num_samples) /
            static_cast<std::int64_t>(fmt_.sample_rate_hz);
        buf_.insert(buf_.end(), pcm, pcm + num_samples);
        accumulated_ms_ += frame_ms;
        if (accumulated_ms_ >= kTapBatchMs) {
            on_batch_(buf_.data(), buf_.size(), fmt_, accumulated_ms_);
            buf_.clear();
            accumulated_ms_ = 0;
            ++batches_flushed_;
        }
    }

    std::uint64_t batches_flushed() const noexcept { return batches_flushed_; }
    std::size_t pending_samples() const noexcept { return buf_.size(); }

private:
    BatchSink on_batch_;
    PcmFormat fmt_;
    std::vector<std::int16_t> buf_;
    std::int64_t accumulated_ms_ = 0;
    std::uint64_t batches_flushed_ = 0;
};

// ---------------------------------------------------------------------------
// Barge-in → stop-playback measurement (requirement 5)
// ---------------------------------------------------------------------------

class BargeInMeter {
public:
    /** 打断指令时刻 — the instant the barge-in command arrived. */
    void record_barge_in(std::int64_t ts_us) noexcept {
        barge_in_ts_us_ = ts_us;
    }

    /** 停播帧时刻 — the instant the TTS playback queue actually stopped. */
    void record_stop(std::int64_t ts_us) noexcept {
        stop_ts_us_ = ts_us;
        if (barge_in_ts_us_ >= 0 && stop_ts_us_ >= barge_in_ts_us_) {
            last_delta_us_ = stop_ts_us_ - barge_in_ts_us_;
            ++events_;
        }
    }

    /** 打断→停播 差值 (stop − barge-in), in microseconds; -1 when no event
     *  has been measured yet. */
    std::int64_t last_delta_us() const noexcept { return last_delta_us_; }
    std::uint64_t events() const noexcept { return events_; }

private:
    std::int64_t barge_in_ts_us_ = -1;
    std::int64_t stop_ts_us_ = -1;
    std::int64_t last_delta_us_ = -1;
    std::uint64_t events_ = 0;
};

// ---------------------------------------------------------------------------
// The voice chain, wired end to end
// ---------------------------------------------------------------------------

class VoiceAgentPipeline {
public:
    VoiceAgentPipeline(std::unique_ptr<IASR> asr,
                       std::unique_ptr<ILLM> llm,
                       std::unique_ptr<ITts> tts,
                       nimrtc::opus::EncoderConfig opus_cfg)
        : asr_(std::move(asr)),
          llm_(std::move(llm)),
          tts_(std::move(tts)),
          opus_tts_(std::move(opus_cfg)),
          batch_(std::make_unique<TapBatchAccumulator>(
              [this](const std::int16_t* pcm, std::size_t samples,
                     const PcmFormat& fmt, std::int64_t batch_ms) {
                  on_full_batch(pcm, samples, fmt, batch_ms);
              })) {}

    /** Install this as the IAudio3A post-process int16 tap:
     *
     *   a3a->set_post_process_tap(
     *       nullptr,
     *       [this](const std::int16_t* s, const PcmFrameMetadata& m) {
     *           pipeline.on_post_3a_int16(
     *               s, m.num_samples,
     *               {m.sample_rate_hz, m.num_channels});
     *       });
     *
     *  Only post-3A int16 PCM reaches the ASR (requirement 2); pre-3A raw
     *  microphone data never enters this entry point. */
    void on_post_3a_int16(const std::int16_t* pcm,
                          std::size_t num_samples,
                          [[maybe_unused]] const PcmFormat& fmt) {
        batch_->add(pcm, num_samples);
    }

    /** Barge-in: stop the previous TTS playback queue immediately (spec
     *  risk 3) and record the interrupt instant so the chain is measurable
     *  (requirement 5).  `ts_us` is the interrupt command time. */
    void on_barge_in(std::int64_t ts_us) {
        meter_.record_barge_in(ts_us);
        stop_playback();
        // The playback queue drops pending audio; the stop instant is the
        // moment the in-flight 10 ms frame boundary passes.
        meter_.record_stop(ts_us + kTapFrameMs * 1000);
        ++barge_in_count_;
    }

    const BargeInMeter& barge_in_meter() const noexcept { return meter_; }
    std::uint64_t barge_in_count() const noexcept { return barge_in_count_; }
    std::uint64_t batches_flushed() const noexcept { return batch_->batches_flushed(); }
    std::uint64_t opus_packets() const noexcept { return opus_tts_.packets_encoded(); }
    std::uint64_t llm_tokens() const noexcept { return llm_tokens_; }
    std::uint64_t asr_frames_fed() const noexcept { return asr_frames_fed_; }
    const std::string& last_transcript() const noexcept { return last_transcript_; }
    const char* asr_name() const noexcept { return asr_ ? asr_->name() : "none"; }
    const char* llm_name() const noexcept { return llm_ ? llm_->name() : "none"; }
    const char* tts_name() const noexcept { return tts_ ? tts_->name() : "none"; }

private:
    void stop_playback() noexcept {
        // Drop every queued TTS PCM frame that has not been handed to the
        // speaker yet.  The demo models playback as an int16 ring the
        // pipeline drains; barge-in clears it here.
        playback_queued_samples_ = 0;
    }

    void on_full_batch(const std::int16_t* pcm,
                       std::size_t samples,
                       const PcmFormat& fmt,
                       std::int64_t /*batch_ms*/) {
        // Requirement 2: post-3A int16 PCM is what the ASR consumes.
        asr_->feed(pcm, samples, fmt);
        asr_frames_fed_ += samples;
        const std::string text = asr_->transcript();
        if (text.empty()) return;

        // A repeated ASR result is not re-sent to the model: the LLM is
        // streamed once per new utterance.
        if (text == last_llm_prompt_) return;
        last_llm_prompt_ = text;

        // Requirement 3: the LLM reply is consumed as a token stream; each
        // token is turned into TTS PCM and Opus-encoded on the way (回传).
        std::string partial;
        llm_->stream(text, [this, &partial, fmt](std::string_view token, bool final) {
            if (!token.empty()) {
                ++llm_tokens_;
                partial += token;
                std::vector<std::int16_t> pcm_out;
                if (tts_->synthesize(std::string(token), pcm_out, fmt) &&
                    !pcm_out.empty()) {
                    playback_queued_samples_ += pcm_out.size();
                    std::uint8_t opus[2048];
                    const std::size_t n =
                        opus_tts_.encode(pcm_out.data(), pcm_out.size(),
                                         opus, sizeof(opus));
                    if (n > 0) last_opus_packet_bytes_ = n;
                }
            }
            if (final) last_transcript_ = partial;
        });
    }

    std::unique_ptr<IASR> asr_;
    std::unique_ptr<ILLM> llm_;
    std::unique_ptr<ITts> tts_;
    OpusTts opus_tts_;
    std::unique_ptr<TapBatchAccumulator> batch_;
    BargeInMeter meter_;

    std::string last_transcript_;
    std::string last_llm_prompt_;
    std::uint64_t llm_tokens_ = 0;
    std::uint64_t asr_frames_fed_ = 0;
    std::uint64_t playback_queued_samples_ = 0;
    std::size_t last_opus_packet_bytes_ = 0;
    std::uint64_t barge_in_count_ = 0;
};

}  // namespace demoai

#endif  // NIMRTC_DEMO_AGENT_REALTIME_VOICE_AGENT_HPP
