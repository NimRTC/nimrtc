// audio_device/src/resampler.cpp
// The explicit sample-rate converter — see
// include/nimrtc/audio_device/resampler.hpp for the contract.
//
// Two backends live here:
//
//   1. libsamplerate (the vendored, BSD-2-Clause production path).
//      Compiled in when NIMRTC_HAS_LIBSAMPLERATE=1, which the vendor
//      wrapper sets when NIMRTC_VENDORED_LIBSAMPLERATE=ON *and* the
//      submodule is populated.
//
//   2. A pure-C++ linear-interpolation fallback, used when the vendored
//      library is not compiled in.  It is honest about being a
//      fallback: `backend()` returns "linear" and `stats().converter`
//      says "linear".
//
// Both backends keep their state across calls, so a stream stays
// continuous, and both report the SAME counters.

#include <nimrtc/audio_device/resampler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#if defined(NIMRTC_HAS_LIBSAMPLERATE) && NIMRTC_HAS_LIBSAMPLERATE
    #define NIMRTC_RESAMPLER_USE_LIBSAMPLERATE 1
    #include <samplerate.h>
#else
    #define NIMRTC_RESAMPLER_USE_LIBSAMPLERATE 0
#endif

namespace nimrtc::audio_device {

namespace {

// libsamplerate's supported ratio range (see upstream's common.h:
// SRC_MAX_RATIO).  The same bound applies to the linear fallback, so
// an out-of-range ratio is rejected identically in both builds.
constexpr double kMaxRatio = 256.0;
constexpr double kMinRatio = 1.0 / kMaxRatio;

// The largest channel count libsamplerate's sinc converter supports
// (upstream's MAX_CHANNELS).  The linear fallback has no such limit,
// but the shared bound keeps the two backends' behaviour identical.
constexpr std::uint16_t kMaxChannels = 128;

// A safety bound on the scratch buffers so a hostile input frame count
// cannot ask for an unbounded allocation.
constexpr std::size_t kMaxScratchFrames = 192u * 60u * 60u;

const char* const kBackendLibsamplerate = "libsamplerate";
const char* const kBackendLinear        = "linear";

// The number of output frames the flush is allowed to produce.  A
// converter's tail is bounded by its filter's group delay, so a few
// thousand frames is already two orders of magnitude more than any
// real converter needs.
constexpr std::size_t kFlushCapacityFrames = 8192;

#if NIMRTC_RESAMPLER_USE_LIBSAMPLERATE

// Map a NimRTC quality hint onto a libsamplerate converter type.
//
// NOTE: the vendored build leaves upstream's SRC_SINC_BEST_QUALITY
// converter OUT (its coefficient table is a ~9 MB array of doubles).
// Asking for "best" therefore selects the MEDIUM converter, and the
// effective converter is reported through stats().converter so the
// host never believes it got something it did not.
int to_src_converter(ResamplerQuality quality) noexcept {
    switch (quality) {
        case ResamplerQuality::kFast:
            return SRC_SINC_FASTEST;
        case ResamplerQuality::kBest:
        case ResamplerQuality::kMedium:
        default:
            return SRC_SINC_MEDIUM_QUALITY;
    }
}

const char* converter_name(int converter_type) noexcept {
    const char* name = src_get_name(converter_type);
    return name != nullptr ? name : "unknown";
}

#endif // NIMRTC_RESAMPLER_USE_LIBSAMPLERATE

// Clamp a float sample into the int16 range and round it.
std::int16_t to_int16(double value) noexcept {
    if (value >= 32767.0) {
        return 32767;
    }
    if (value <= -32768.0) {
        return -32768;
    }
    return static_cast<std::int16_t>(std::lrint(value));
}

} // namespace

// ---------------------------------------------------------------------------
// Resampler::Impl
// ---------------------------------------------------------------------------

struct Resampler::Impl {
    std::uint32_t input_rate = 0;
    std::uint32_t output_rate = 0;
    std::uint16_t channels = 0;
    double ratio = 0.0;
    bool valid = false;
    bool flushed = false;

    // Counters (the DEC-3 "make the conversion visible" surface).
    std::uint64_t input_frames = 0;    // frames handed to the converter
    std::uint64_t output_frames = 0;   // frames produced by the converter

    const char* backend = kBackendLinear;
    std::string converter;

#if NIMRTC_RESAMPLER_USE_LIBSAMPLERATE
    SRC_STATE* state = nullptr;
    int src_converter = SRC_SINC_MEDIUM_QUALITY;

    // Scratch buffers.  libsamplerate works on float; the NimRTC
    // surface is int16.  The scratch is reused across calls so the hot
    // path does not allocate per block.
    std::vector<float> in_f;
    std::vector<float> out_f;

    // The input frames the converter did NOT consume in the previous
    // call.  libsamplerate copies the input into its filter buffer in
    // bounded chunks, so a large block can leave a tail behind; that
    // tail has to be re-offered (ahead of the next block) or it would
    // be silently dropped.
    std::vector<std::int16_t> pending_in;
#else
    // The pure-C++ fallback's state: the pending input tail and the
    // fractional read position within it.  The position is what makes
    // a block-wise conversion identical to a whole-stream conversion.
    std::vector<std::int16_t> pending;
    double position = 0.0;
#endif

    ~Impl() {
#if NIMRTC_RESAMPLER_USE_LIBSAMPLERATE
        if (state != nullptr) {
            src_delete(state);
            state = nullptr;
        }
#endif
    }

    // The number of output frames the converter still owes.
    std::size_t frames_available() const noexcept {
        if (ratio <= 0.0) {
            return 0;
        }
        // The uniform accounting: a stream of N input frames owes
        // N * ratio output frames.  Whatever has not been emitted yet
        // is still owed.
        //
        // For the libsamplerate backend this also covers the sinc
        // filter's group delay: the converter took the input frames in
        // but has not turned them into output yet, so they are owed.
        // For the linear backend the interpolator emits every frame
        // that has a right-hand neighbour, so the tail is at most one
        // frame.
        const double owed = static_cast<double>(input_frames) * ratio -
                            static_cast<double>(output_frames);
        return owed > 0.0 ? static_cast<std::size_t>(owed + 0.5) : 0u;
    }
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Resampler::Resampler(std::uint32_t input_rate_hz,
                     std::uint32_t output_rate_hz,
                     std::uint16_t channels,
                     ResamplerQuality quality) noexcept
    : impl_(std::make_unique<Impl>()) {
    impl_->input_rate  = input_rate_hz;
    impl_->output_rate = output_rate_hz;
    impl_->channels    = channels;

    // ---- Validate the configuration -------------------------------------
    // Every rejection path leaves the object in a valid-but-failed
    // state: no exception, no crash, no UB.
    if (input_rate_hz == 0 || output_rate_hz == 0 || channels == 0) {
        impl_->converter = "invalid";
        return;
    }
    if (channels > kMaxChannels) {
        impl_->converter = "invalid";
        return;
    }

    const double ratio = static_cast<double>(output_rate_hz) /
                         static_cast<double>(input_rate_hz);
    if (!(ratio >= kMinRatio && ratio <= kMaxRatio)) {
        impl_->converter = "invalid";
        return;
    }
    impl_->ratio = ratio;

#if NIMRTC_RESAMPLER_USE_LIBSAMPLERATE
    impl_->backend = kBackendLibsamplerate;
    impl_->src_converter = to_src_converter(quality);

    int error = 0;
    impl_->state = src_new(impl_->src_converter, static_cast<int>(channels), &error);
    if (impl_->state == nullptr) {
        impl_->converter = "invalid";
        return;
    }
    impl_->converter = converter_name(impl_->src_converter);
#else
    (void)quality;
    impl_->backend = kBackendLinear;
    impl_->converter = "linear";
#endif

    impl_->valid = true;
}

Resampler::~Resampler() = default;

Resampler::Resampler(Resampler&&) noexcept = default;

Resampler& Resampler::operator=(Resampler&&) noexcept = default;

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

ResampleStatus Resampler::resample(const std::int16_t* in,
                                    std::size_t in_frames,
                                    std::vector<std::int16_t>& out) {
    out.clear();

    if (impl_ == nullptr || !impl_->valid) {
        return ResampleStatus::kInvalidConfig;
    }
    if (impl_->flushed) {
        // The converter was flushed at end-of-stream; it needs a
        // reset() before it can accept more input.
        return ResampleStatus::kInternalError;
    }
    if (in == nullptr && in_frames > 0) {
        return ResampleStatus::kInvalidInput;
    }
    if (in_frames > kMaxScratchFrames) {
        return ResampleStatus::kInvalidInput;
    }
    if (in_frames == 0) {
        return ResampleStatus::kOk;
    }

#if NIMRTC_RESAMPLER_USE_LIBSAMPLERATE
    const std::size_t channels = impl_->channels;

    // ---- The 1:1 pass-through -------------------------------------------
    //
    // When the input and the output rate are the same there is nothing
    // to convert: running the sinc filter would only add its group
    // delay and its CPU cost for a signal that is already at the right
    // rate.  The pass-through keeps the stream bit-exact, which is what
    // a host expects when the device already runs at the codec rate.
    if (impl_->ratio == 1.0) {
        out.assign(in, in + in_frames * channels);
        impl_->input_frames  += in_frames;
        impl_->output_frames += in_frames;
        return ResampleStatus::kOk;
    }

    const std::size_t total_in = impl_->pending_in.size() / channels + in_frames;

    // The input the converter sees this call: the tail it did not
    // consume last time, followed by the new block.
    impl_->in_f.resize(total_in * channels);
    for (std::size_t i = 0; i < impl_->pending_in.size(); ++i) {
        impl_->in_f[i] = static_cast<float>(impl_->pending_in[i]) / 32768.0f;
    }
    for (std::size_t i = 0; i < in_frames * channels; ++i) {
        impl_->in_f[impl_->pending_in.size() + i] =
            static_cast<float>(in[i]) / 32768.0f;
    }

    // The output capacity: the ideal frame count plus a margin for the
    // converter's rounding.  The converter never produces more than
    // `consumed * ratio`, so the margin is never the binding limit.
    const std::size_t ideal =
        static_cast<std::size_t>(std::ceil(static_cast<double>(total_in) * impl_->ratio)) + 64;
    impl_->out_f.resize(ideal * channels);

    SRC_DATA data {};
    data.data_in       = impl_->in_f.data();
    data.input_frames  = static_cast<long>(total_in);
    data.data_out      = impl_->out_f.data();
    data.output_frames = static_cast<long>(ideal);
    data.end_of_input  = 0;
    data.src_ratio     = impl_->ratio;

    const int error = src_process(impl_->state, &data);
    if (error != 0) {
        return ResampleStatus::kInternalError;
    }

    const std::size_t consumed = static_cast<std::size_t>(data.input_frames_used);
    const std::size_t produced = static_cast<std::size_t>(data.output_frames_gen);

    // Whatever the converter did not consume stays pending, so the next
    // call re-offers it ahead of the new block.
    if (consumed < total_in) {
        impl_->pending_in.resize((total_in - consumed) * channels);
        for (std::size_t i = 0; i < impl_->pending_in.size(); ++i) {
            impl_->pending_in[i] = to_int16(
                static_cast<double>(impl_->in_f[consumed * channels + i]) * 32768.0);
        }
    } else {
        impl_->pending_in.clear();
    }

    out.resize(produced * channels);
    for (std::size_t i = 0; i < produced * channels; ++i) {
        out[i] = to_int16(static_cast<double>(impl_->out_f[i]) * 32768.0);
    }

    impl_->input_frames  += in_frames;
    impl_->output_frames += produced;
#else
    // ---- The pure-C++ linear-interpolation fallback ---------------------
    //
    // The interpolator reads the input stream at a fractional position
    // that advances by `1 / ratio` input frames per output frame.  The
    // state that must survive a call is (a) the pending input tail and
    // (b) the fractional position, so a stream split into blocks is
    // converted identically to the same stream in one block.
    const std::size_t channels = impl_->channels;

    // ---- The 1:1 pass-through -------------------------------------------
    //
    // When the input and the output rate are the same there is nothing
    // to convert: the signal is already at the right rate, so it is
    // copied through bit-exact.  This keeps the two backends'
    // behaviour identical, and it keeps the "the device already runs
    // at the codec rate" case free of any filter delay.
    if (impl_->ratio == 1.0) {
        out.assign(in, in + in_frames * channels);
        impl_->input_frames  += in_frames;
        impl_->output_frames += in_frames;
        return ResampleStatus::kOk;
    }

    impl_->pending.insert(impl_->pending.end(), in, in + in_frames * channels);

    const std::size_t pending_frames = impl_->pending.size() / channels;

    std::vector<std::int16_t> result;
    result.reserve((pending_frames + 1) * channels);

    // `position` is the fractional index of the NEXT output sample's
    // left-hand neighbour, relative to the pending buffer.
    double pos = impl_->position;
    while (true) {
        const std::size_t left = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(left);
        if (left + 1 >= pending_frames) {
            break;   // no right-hand neighbour yet
        }
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const double a = static_cast<double>(impl_->pending[left * channels + ch]);
            const double b = static_cast<double>(impl_->pending[(left + 1) * channels + ch]);
            result.push_back(to_int16(a + (b - a) * frac));
        }
        pos += 1.0 / impl_->ratio;
    }

    // Drop the input frames that have been fully consumed, keeping the
    // last one (it is the left-hand neighbour of the next output).
    const std::size_t consumed = static_cast<std::size_t>(pos);
    const std::size_t keep_from = consumed > 0 ? consumed - 1 : 0;
    if (keep_from >= pending_frames) {
        impl_->pending.clear();
        impl_->position = 0.0;
    } else {
        if (keep_from > 0) {
            impl_->pending.erase(impl_->pending.begin(),
                                 impl_->pending.begin() +
                                     static_cast<std::ptrdiff_t>(keep_from * channels));
        }
        impl_->position = pos - static_cast<double>(keep_from);
    }

    out = std::move(result);

    impl_->input_frames  += in_frames;
    impl_->output_frames += out.size() / channels;
#endif

    return ResampleStatus::kOk;
}

ResampleStatus Resampler::flush(std::vector<std::int16_t>& out) {
    out.clear();

    if (impl_ == nullptr || !impl_->valid) {
        return ResampleStatus::kInvalidConfig;
    }
    if (impl_->flushed) {
        return ResampleStatus::kOk;   // idempotent
    }

#if NIMRTC_RESAMPLER_USE_LIBSAMPLERATE
    // libsamplerate drains its filter when the *last* input block
    // arrives with `end_of_input` set: upstream then zero-pads the
    // filter buffer and marks the real end, so the remaining frames
    // come out.  The pending tail (the frames the converter did not
    // consume) is that last block.
    //
    // When there is no pending tail, a single zero frame is offered:
    // upstream's `prepare_data` returns early on a NULL input pointer,
    // so the end-of-input path needs a non-NULL buffer to reach.
    const std::size_t channels = impl_->channels;
    std::vector<float> flush_in;
    std::size_t flush_frames = 0;
    if (impl_->ratio == 1.0) {
        // The pass-through consumed everything: there is no tail.
        impl_->pending_in.clear();
        impl_->flushed = true;
        return ResampleStatus::kOk;
    }
    if (!impl_->pending_in.empty()) {
        flush_frames = impl_->pending_in.size() / channels;
        flush_in.resize(impl_->pending_in.size());
        for (std::size_t i = 0; i < impl_->pending_in.size(); ++i) {
            flush_in[i] = static_cast<float>(impl_->pending_in[i]) / 32768.0f;
        }
    } else {
        flush_frames = 1;
        flush_in.assign(channels, 0.0f);
    }
    impl_->pending_in.clear();

    impl_->out_f.resize(kFlushCapacityFrames * channels);

    SRC_DATA data {};
    data.data_in       = flush_in.data();
    data.input_frames  = static_cast<long>(flush_frames);
    data.data_out      = impl_->out_f.data();
    data.output_frames = static_cast<long>(kFlushCapacityFrames);
    data.end_of_input  = 1;
    data.src_ratio     = impl_->ratio;

    const int error = src_process(impl_->state, &data);
    if (error != 0) {
        return ResampleStatus::kInternalError;
    }

    const std::size_t produced = static_cast<std::size_t>(data.output_frames_gen);
    out.resize(produced * channels);
    for (std::size_t i = 0; i < produced * channels; ++i) {
        out[i] = to_int16(static_cast<double>(impl_->out_f[i]) * 32768.0);
    }
    impl_->output_frames += produced;
#else
    // The linear fallback's tail: the pending input frames that were
    // never interpolated (they had no right-hand neighbour).  The
    // honest flush is to hold the last value for them.
    const std::size_t channels = impl_->channels;
    const std::size_t pending_frames = impl_->pending.size() / channels;

    double pos = impl_->position;
    while (pending_frames > 0) {
        const std::size_t left = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(left);
        if (left + 1 >= pending_frames) {
            break;
        }
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const double a = static_cast<double>(impl_->pending[left * channels + ch]);
            const double b = static_cast<double>(impl_->pending[(left + 1) * channels + ch]);
            out.push_back(to_int16(a + (b - a) * frac));
        }
        pos += 1.0 / impl_->ratio;
    }

    // Hold the last input frame for whatever is still owed.
    const std::size_t produced = out.size() / channels;
    const std::size_t owed = impl_->frames_available();
    if (owed > produced && pending_frames > 0) {
        const std::size_t extra = owed - produced;
        const std::size_t last = (pending_frames - 1) * channels;
        for (std::size_t i = 0; i < extra; ++i) {
            for (std::size_t ch = 0; ch < channels; ++ch) {
                out.push_back(impl_->pending[last + ch]);
            }
        }
    }
    impl_->output_frames += out.size() / channels;
    impl_->pending.clear();
    impl_->position = 0.0;
#endif

    impl_->flushed = true;
    return ResampleStatus::kOk;
}

void Resampler::reset() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->flushed = false;
#if NIMRTC_RESAMPLER_USE_LIBSAMPLERATE
    if (impl_->state != nullptr) {
        src_reset(impl_->state);
    }
    impl_->pending_in.clear();
#else
    impl_->pending.clear();
    impl_->position = 0.0;
#endif
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool Resampler::valid() const noexcept {
    return impl_ != nullptr && impl_->valid;
}

std::uint32_t Resampler::input_rate_hz() const noexcept {
    return impl_ != nullptr ? impl_->input_rate : 0;
}

std::uint32_t Resampler::output_rate_hz() const noexcept {
    return impl_ != nullptr ? impl_->output_rate : 0;
}

std::uint16_t Resampler::channels() const noexcept {
    return impl_ != nullptr ? impl_->channels : 0;
}

double Resampler::ratio() const noexcept {
    return impl_ != nullptr ? impl_->ratio : 0.0;
}

std::string Resampler::backend() const {
    return impl_ != nullptr ? std::string(impl_->backend) : std::string(kBackendLinear);
}

ResamplerStats Resampler::stats() const {
    ResamplerStats s;
    if (impl_ == nullptr) {
        return s;
    }
    s.input_rate_hz  = impl_->input_rate;
    s.output_rate_hz = impl_->output_rate;
    s.ratio          = impl_->ratio;
    s.input_frames   = impl_->input_frames;
    s.output_frames  = impl_->output_frames;
    s.channels       = impl_->channels;
    s.conversion_performed = impl_->input_rate != impl_->output_rate;
    s.backend        = impl_->backend;
    s.converter      = impl_->converter;
    return s;
}

std::size_t Resampler::output_frames_available() const noexcept {
    return impl_ != nullptr ? impl_->frames_available() : 0u;
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

double resample_ratio(std::uint32_t input_rate, std::uint32_t output_rate) noexcept {
    if (input_rate == 0 || output_rate == 0) {
        return 0.0;
    }
    return static_cast<double>(output_rate) / static_cast<double>(input_rate);
}

ResamplerQuality default_resampler_quality() noexcept {
    return ResamplerQuality::kMedium;
}

bool resampler_available() noexcept {
    return NIMRTC_RESAMPLER_USE_LIBSAMPLERATE != 0;
}

std::string resampler_backend() noexcept {
    return NIMRTC_RESAMPLER_USE_LIBSAMPLERATE != 0 ? std::string(kBackendLibsamplerate)
                                                    : std::string(kBackendLinear);
}

std::string_view resample_status_name(ResampleStatus status) noexcept {
    switch (status) {
        case ResampleStatus::kOk:            return "ok";
        case ResampleStatus::kInvalidConfig: return "invalid configuration";
        case ResampleStatus::kInvalidInput:  return "invalid input";
        case ResampleStatus::kInternalError: return "internal error";
        default:                             return "unknown";
    }
}

} // namespace nimrtc::audio_device
