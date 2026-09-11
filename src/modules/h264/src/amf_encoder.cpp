/**
 * @file amf_encoder.cpp
 * @brief AMD AMF H.264 encoder/decoder plugin implementation
 * @details Wraps AMD's Advanced Media Framework (AMF) SDK for hardware-accelerated
 *          H.264 encoding and decoding on Windows. Falls back to stubs on other platforms.
 */
#include <nimrtc/h264/hw_backends.hpp>

#if defined(NIMRTC_PLUGINS_AMF_ON) && defined(_WIN32)

#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/core/Surface.h>
#include <AMF/core/Buffer.h>
#include <AMF/components/VideoEncoder.h>
#include <AMF/components/VideoDecoderUVD.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <limits>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include "hw_backend_base.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace nimrtc::h264::amf_backend {

namespace {

// AMF error checking macro
#define AMF_RAISE_ERROR(x, msg) \
    do { \
        amf::AMF_RESULT _err_ = (x); \
        if (_err_ != AMF_OK) { \
            NIMRTC_LOG_ERROR(msg << ": AMF error 0x" << std::hex << static_cast<uint32_t>(_err_)); \
            return plugins::VideoCodecResult::ERROR; \
        } \
    } while (false)

// Convert frame rate to AMF rate
inline AMFRate MakeAmfRate(uint32_t fps_numerator, uint32_t fps_denominator) {
    if (fps_denominator == 0) {
        fps_denominator = 1;
    }
    if (fps_numerator == 0) {
        fps_numerator = 30;
    }
    AMFRate rate;
    rate.num = fps_numerator;
    rate.den = fps_denominator;
    return rate;
}

// Convert VideoCodecConfig profile to AMF profile
amf_int64 ConvertProfileToAmf(plugins::VideoCodecProfile profile) {
    switch (profile) {
        case plugins::VideoCodecProfile::BASELINE:
            return AMF_VIDEO_ENCODER_PROFILE_BASELINE;
        case plugins::VideoCodecProfile::MAIN:
            return AMF_VIDEO_ENCODER_PROFILE_MAIN;
        case plugins::VideoCodecProfile::HIGH:
        default:
            return AMF_VIDEO_ENCODER_PROFILE_HIGH;
    }
}

// Convert VideoCodecConfig level to AMF level
amf_int64 ConvertLevelToAmf(int level) {
    switch (level) {
        case 10: return AMF_LEVEL_1_0;
        case 11: return AMF_LEVEL_1_1;
        case 12: return AMF_LEVEL_1_2;
        case 13: return AMF_LEVEL_1_3;
        case 20: return AMF_LEVEL_2_0;
        case 21: return AMF_LEVEL_2_1;
        case 22: return AMF_LEVEL_2_2;
        case 30: return AMF_LEVEL_3_0;
        case 31: return AMF_LEVEL_3_1;
        case 32: return AMF_LEVEL_3_2;
        case 40: return AMF_LEVEL_4_0;
        case 41: return AMF_LEVEL_4_1;
        case 42: return AMF_LEVEL_4_2;
        case 50: return AMF_LEVEL_5_0;
        case 51: return AMF_LEVEL_5_1;
        case 52: return AMF_LEVEL_5_2;
        default:  return AMF_LEVEL_4_1;
    }
}

// Convert I420 to NV12 layout
// I420: Y (w*h) + U (w*h/4) + V (w*h/4) - planar
// NV12: Y (w*h) + UV interleaved (w*h/2) - semi-planar
void ConvertI420ToNv12(const uint8_t* y_src,
                       const uint8_t* u_src,
                       const uint8_t* v_src,
                       uint8_t* y_dst,
                       uint8_t* uv_dst,
                       int width,
                       int height) {
    // Copy Y plane directly
    int y_size = width * height;
    std::memcpy(y_dst, y_src, static_cast<size_t>(y_size));

    // Convert U/V planar to UV interleaved
    int uv_plane_size = y_size / 4;  // Each of U and V is y_size/4
    int uv_stride = width;            // UV plane width (same as Y, but represents UV pairs)
    int uv_height = height / 2;

    for (int row = 0; row < uv_height; ++row) {
        for (int col = 0; col < width / 2; ++col) {
            int u_idx = row * (width / 2) + col;
            int v_idx = row * (width / 2) + col;
            int uv_idx = row * uv_stride + col * 2;
            uv_dst[uv_idx] = u_src[u_idx];
            uv_dst[uv_idx + 1] = v_src[v_idx];
        }
    }
}

} // anonymous namespace

/**
 * @brief AMF H.264 Encoder implementation
 * @details Hardware-accelerated H.264 encoder using AMD AMF SDK
 */
class AmfEncoder : public plugins::IVideoCodec {
public:
    AmfEncoder()
        : opened_(false)
        , width_(0)
        , height_(0)
        , frame_count_(0)
        , encoder_(nullptr) {
    }

    ~AmfEncoder() override {
        Close();
    }

    plugins::VideoCodecResult Open(const plugins::VideoCodecConfig& config) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (opened_) {
            NIMRTC_LOG_ERROR("AMF encoder already opened");
            return plugins::VideoCodecResult::ERROR;
        }

        cfg_ = config;
        width_ = static_cast<int>(config.width);
        height_ = static_cast<int>(config.height);

        NIMRTC_LOG_INFO("Opening AMF encoder: " << width_ << "x" << height_
                     << " @" << config.framerate_numerator << "/"
                     << config.framerate_denominator
                     << " bitrate=" << config.target_bitrate);

        // Initialize factory
        AMF_RESULT res = AMFInit(AMF_MEMORY_TYPE_DX9EX, &factory_);
        if (res != AMF_OK) {
            // Try DX11 as fallback
            res = AMFInit(AMF_MEMORY_TYPE_DX11, &factory_);
        }
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("AMFInit failed: 0x" << std::hex << static_cast<uint32_t>(res));
            return plugins::VideoCodecResult::ERROR;
        }

        // Create context
        res = factory_->CreateContext(&context_);
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to create AMF context");
            factory_.Reset();
            return plugins::VideoCodecResult::ERROR;
        }

        // Create encoder component
        res = context_->CreateComponent(AMFVideoEncoderUVD_EncodeH264_GUID, &encoder_);
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to create H.264 encoder component");
            context_.Reset();
            factory_.Reset();
            return plugins::VideoCodecResult::ERROR;
        }

        // Configure encoder properties
        AMF_RAISE_ERROR(encoder_->SetProperty(AMF_VIDEO_ENCODER_PROFILE,
            ConvertProfileToAmf(config.profile)),
            "Failed to set encoder profile");

        AMFRate frame_rate = MakeAmfRate(config.framerate_numerator, config.framerate_denominator);
        AMF_RAISE_ERROR(encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, frame_rate),
            "Failed to set encoder framerate");

        // No B-frames for simplicity and compatibility
        AMF_RAISE_ERROR(encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0),
            "Failed to set B-frame pattern");

        // IDR interval (0 = only first frame is IDR, 1 = every frame is IDR, etc.)
        amf_int64 idr_interval = (config.keyframe_interval > 0) ?
            static_cast<amf_int64>(config.keyframe_interval) : 60;
        AMF_RAISE_ERROR(encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, idr_interval),
            "Failed to set IDR period");

        // Bitrate
        AMF_RAISE_ERROR(encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE,
            static_cast<amf_int64>(config.target_bitrate)),
            "Failed to set target bitrate");

        // Rate control method - use CBR for real-time
        AMF_RAISE_ERROR(encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR),
            "Failed to set rate control method");

        // Output format - Annex B for WebRTC compatibility
        AMF_RAISE_ERROR(encoder_->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_BIT_STREAM_TYPE,
            AMF_VIDEO_ENCODER_OUTPUT_BIT_STREAM_TYPE_ANNEXB),
            "Failed to set output bitstream type");

        // Quality preset if specified
        if (config.codec_specific && config.codec_specific->has_quality) {
            AMF_RAISE_ERROR(encoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                static_cast<amf_int64>(config.codec_specific->quality)),
                "Failed to set quality preset");
        }

        // Initialize encoder
        res = encoder_->Init(nullptr, width_, height_);
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to initialize encoder: 0x" << std::hex << static_cast<uint32_t>(res));
            encoder_.Reset();
            context_.Reset();
            factory_.Reset();
            return plugins::VideoCodecResult::ERROR;
        }

        opened_ = true;
        frame_count_ = 0;
        stats_ = plugins::VideoCodecStats{};

        NIMRTC_LOG_INFO("AMF encoder opened successfully");
        return plugins::VideoCodecResult::OK;
    }

    plugins::VideoCodecResult Encode(const plugins::VideoFrame& frame,
                                     plugins::EncodedFrame* encoded) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!opened_) {
            NIMRTC_LOG_ERROR("AMF encoder not opened");
            return plugins::VideoCodecResult::ERROR;
        }

        if (!frame.raw || !encoded) {
            return plugins::VideoCodecResult::BAD_PARAM;
        }

        // Get frame dimensions
        int frame_width = static_cast<int>(frame.raw->width());
        int frame_height = static_cast<int>(frame.raw->height());

        if (frame_width != width_ || frame_height != height_) {
            NIMRTC_LOG_ERROR("Frame dimensions mismatch: expected "
                           << width_ << "x" << height_ << ", got "
                           << frame_width << "x" << frame_height);
            return plugins::VideoCodecResult::BAD_PARAM;
        }

        // Check frame type
        bool is_key = frame.is_keyframe || (frame_count_ == 0);
        if (is_key) {
            ++stats_.num_key_frames;
        }
        ++frame_count_;
        ++stats_.num_frames;

        // Create AMF surface from I420 buffer
        AMFSurfacePtr surface;
        AMF_RESULT res = context_->CreateSurface(AMF_MEMORY_TYPE_HOST,
                                                  AMF_SURFACE_NV12,
                                                  width_,
                                                  height_,
                                                  &surface);
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to create AMF surface: 0x" << std::hex << static_cast<uint32_t>(res));
            return plugins::VideoCodecResult::ERROR;
        }

        // Lock the surface plane for writing
        AMFPlanePtr plane_y, plane_uv;
        res = surface->GetPlane(AMF_PLANE_NV12, &plane_y);
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to get NV12 plane");
            return plugins::VideoCodecResult::ERROR;
        }

        // Get I420 data from raw frame
        plugins::I420Buffer* i420 = frame.raw->AsI420Buffer();
        if (!i420) {
            NIMRTC_LOG_ERROR("Frame is not I420 format");
            return plugins::VideoCodecResult::BAD_PARAM;
        }

        const uint8_t* y_data = i420->DataY();
        const uint8_t* u_data = i420->DataU();
        const uint8_t* v_data = i420->DataV();

        // Get plane pointers and copy data
        amf_int32 y_stride, uv_stride;
        amf_uint8* y_plane = plane_y->GetNative();
        amf_uint8* uv_plane = plane_y->GetNative() + (plane_y->GetRowCount() * plane_y->GetPitch());

        y_stride = plane_y->GetPitch();
        uv_stride = y_stride;  // UV has same stride as Y in NV12

        // Convert I420 to NV12
        for (int row = 0; row < height_; ++row) {
            std::memcpy(y_plane + row * y_stride,
                        y_data + row * i420->StrideY(),
                        static_cast<size_t>(width_));
        }

        // Copy U and V into interleaved UV plane
        int uv_height = height_ / 2;
        int y_plane_size = width_ * height_;
        int uv_plane_size = width_ * uv_height;

        for (int row = 0; row < uv_height; ++row) {
            const uint8_t* u_row = u_data + row * i420->StrideU();
            const uint8_t* v_row = v_data + row * i420->StrideV();
            uint8_t* uv_row = uv_plane + row * uv_stride;

            for (int col = 0; col < width_ / 2; ++col) {
                uv_row[col * 2] = u_row[col];
                uv_row[col * 2 + 1] = v_row[col];
            }
        }

        surface->SetTimeStamp(frame.timestamp_us);

        // Force keyframe if requested
        if (is_key) {
            encoder_->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
        }

        // Submit input
        res = encoder_->SubmitInput(surface);
        if (res == AMF_NEED_MORE_INPUT) {
            // Encoder needs more input, this is normal
        } else if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to submit encoder input: 0x" << std::hex << static_cast<uint32_t>(res));
            return plugins::VideoCodecResult::ERROR;
        }

        // Query output
        AMFPacketPtr packet;
        res = encoder_->QueryOutput(&packet);
        if (res == AMF_REPEAT) {
            // No output ready yet
            return plugins::VideoCodecResult::OK;
        } else if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to query encoder output: 0x" << std::hex << static_cast<uint32_t>(res));
            return plugins::VideoCodecResult::ERROR;
        }

        if (!packet) {
            return plugins::VideoCodecResult::OK;
        }

        // Get encoded data
        AMFBufferPtr buffer;
        res = packet->GetBuffer(&buffer);
        if (res != AMF_OK || !buffer) {
            NIMRTC_LOG_ERROR("Failed to get buffer from packet");
            return plugins::VideoCodecResult::ERROR;
        }

        amf_uint8* data = static_cast<amf_uint8*>(buffer->GetNative());
        amf_size size = buffer->GetSize();

        if (!data || size == 0) {
            NIMRTC_LOG_ERROR("Empty encoded data");
            return plugins::VideoCodecResult::ERROR;
        }

        // Copy to output
        encoded->SetData(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(size));
        encoded->SetTimestamp(frame.timestamp_us);
        encoded->SetDuration(frame.duration_us);
        encoded->Setis_keyframe(is_key);

        // Check for codec info (SPS/PPS)
        amf_int64 bitstream_type = 0;
        if (packet->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_BIT_STREAM_TYPE, &bitstream_type) == AMF_OK) {
            // Bitstream type tells us if it's Annex B
        }

        // Update stats
        stats_.encoded_bytes += size;
        if (is_key) {
            stats_.num_key_frames++;
        }

        return plugins::VideoCodecResult::OK;
    }

    plugins::VideoCodecResult Close() override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!opened_) {
            return plugins::VideoCodecResult::OK;
        }

        // Drain encoder - submit null input
        if (encoder_) {
            // Flush remaining frames
            AMF_RESULT res = encoder_->Drain();
            if (res != AMF_OK) {
                NIMRTC_LOG_ERROR("Encoder drain failed: 0x" << std::hex << static_cast<uint32_t>(res));
            }

            encoder_->Terminate();
            encoder_.Reset();
        }

        if (context_) {
            context_.Reset();
        }

        if (factory_) {
            factory_.Reset();
        }

        opened_ = false;
        NIMRTC_LOG_INFO("AMF encoder closed");
        return plugins::VideoCodecResult::OK;
    }

    plugins::VideoCodecStats GetStats() const override {
        return stats_;
    }

    void ResetStats() override {
        stats_ = plugins::VideoCodecStats{};
        frame_count_ = 0;
    }

private:
    AMFFactoryPtr factory_;
    AMFContextPtr context_;
    AMFComponentPtr encoder_;

    std::mutex mutex_;
    plugins::VideoCodecConfig cfg_;
    plugins::VideoCodecStats stats_;

    bool opened_;
    int width_;
    int height_;
    int64_t frame_count_;
};

/**
 * @brief AMF H.264 Decoder implementation
 * @details Hardware-accelerated H.264 decoder using AMD AMF SDK
 */
class AmfDecoder : public plugins::IVideoCodec {
public:
    AmfDecoder()
        : opened_(false)
        , width_(0)
        , height_(0)
        , frame_count_(0)
        , decoder_(nullptr) {
    }

    ~AmfDecoder() override {
        Close();
    }

    plugins::VideoCodecResult Open(const plugins::VideoCodecConfig& config) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (opened_) {
            NIMRTC_LOG_ERROR("AMF decoder already opened");
            return plugins::VideoCodecResult::ERROR;
        }

        cfg_ = config;
        width_ = static_cast<int>(config.width);
        height_ = static_cast<int>(config.height);

        NIMRTC_LOG_INFO("Opening AMF decoder: " << width_ << "x" << height_);

        // Initialize factory
        AMF_RESULT res = AMFInit(AMF_MEMORY_TYPE_DX9EX, &factory_);
        if (res != AMF_OK) {
            res = AMFInit(AMF_MEMORY_TYPE_DX11, &factory_);
        }
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("AMFInit failed: 0x" << std::hex << static_cast<uint32_t>(res));
            return plugins::VideoCodecResult::ERROR;
        }

        // Create context
        res = factory_->CreateContext(&context_);
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to create AMF context");
            factory_.Reset();
            return plugins::VideoCodecResult::ERROR;
        }

        // Create decoder component
        res = context_->CreateComponent(AMFVideoDecoderUVD_H264_GUID, &decoder_);
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to create H.264 decoder component");
            context_.Reset();
            factory_.Reset();
            return plugins::VideoCodecResult::ERROR;
        }

        // Configure decoder properties
        AMF_RAISE_ERROR(decoder_->SetProperty(AMF_VIDEO_DECODER_SURFACE_TYPE,
            AMF_MEMORY_TYPE_HOST),
            "Failed to set decoder surface type");

        // Initialize decoder
        res = decoder_->Init(AMF_SURFACE_NV12, width_, height_);
        if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to initialize decoder: 0x" << std::hex << static_cast<uint32_t>(res));
            decoder_.Reset();
            context_.Reset();
            factory_.Reset();
            return plugins::VideoCodecResult::ERROR;
        }

        opened_ = true;
        frame_count_ = 0;
        stats_ = plugins::VideoCodecStats{};

        NIMRTC_LOG_INFO("AMF decoder opened successfully");
        return plugins::VideoCodecResult::OK;
    }

    plugins::VideoCodecResult Decode(const plugins::EncodedFrame& encoded,
                                     plugins::VideoFrame* frame) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!opened_) {
            NIMRTC_LOG_ERROR("AMF decoder not opened");
            return plugins::VideoCodecResult::ERROR;
        }

        if (!frame) {
            return plugins::VideoCodecResult::BAD_PARAM;
        }

        ++stats_.num_frames;

        // Create buffer from encoded data
        AMFBufferPtr buffer;
        AMF_RESULT res = context_->CreateBuffer(AMF_MEMORY_TYPE_HOST,
                                                encoded.data_size(),
                                                &buffer);
        if (res != AMF_OK || !buffer) {
            NIMRTC_LOG_ERROR("Failed to create buffer for decoder input");
            return plugins::VideoCodecResult::ERROR;
        }

        // Copy encoded data to buffer
        void* dst = buffer->GetNative();
        std::memcpy(dst, encoded.data(), encoded.data_size());
        buffer->SetSize(encoded.data_size());

        // Create packet
        AMFPacketPtr packet;
        res = context_->CreatePacket(buffer, &packet);
        if (res != AMF_OK || !packet) {
            NIMRTC_LOG_ERROR("Failed to create decoder packet");
            return plugins::VideoCodecResult::ERROR;
        }

        packet->SetTimeStamp(encoded.timestamp_us());
        packet->SetDuration(encoded.duration_us());

        // Set properties for keyframe
        if (encoded.is_keyframe()) {
            amf::AMFVariant val(true);
            packet->SetProperty(AMF_VIDEO_DECODER_FULL_SURFACE_MODE, val);
        }

        // Submit to decoder
        res = decoder_->SubmitInput(packet);
        if (res != AMF_OK && res != AMF_NEED_MORE_INPUT) {
            NIMRTC_LOG_ERROR("Failed to submit decoder input: 0x" << std::hex << static_cast<uint32_t>(res));
            return plugins::VideoCodecResult::ERROR;
        }

        // Query decoded output
        AMFSurfacePtr surface;
        res = decoder_->QueryOutput(&surface);
        if (res == AMF_REPEAT || res == AMF_NO_MORE_SURFACES) {
            // No output ready yet
            return plugins::VideoCodecResult::OK;
        } else if (res != AMF_OK) {
            NIMRTC_LOG_ERROR("Failed to query decoder output: 0x" << std::hex << static_cast<uint32_t>(res));
            return plugins::VideoCodecResult::ERROR;
        }

        if (!surface) {
            return plugins::VideoCodecResult::OK;
        }

        // Get NV12 surface data
        AMFPlanePtr plane;
        res = surface->GetPlane(AMF_PLANE_NV12, &plane);
        if (res != AMF_OK || !plane) {
            NIMRTC_LOG_ERROR("Failed to get NV12 plane from decoded surface");
            return plugins::VideoCodecResult::ERROR;
        }

        // Allocate output frame
        auto output_frame = plugins::I420Buffer::Create(width_, height_);
        if (!output_frame) {
            NIMRTC_LOG_ERROR("Failed to create output I420 buffer");
            return plugins::VideoCodecResult::ERROR;
        }

        // Get plane data
        amf_uint8* nv12_y = plane->GetNative();
        amf_int32 y_stride = plane->GetPitch();
        amf_int32 y_height = plane->GetRowCount();

        // UV plane is right after Y plane
        amf_uint8* nv12_uv = nv12_y + y_stride * y_height;
        amf_int32 uv_stride = y_stride;

        // Convert NV12 to I420
        uint8_t* out_y = output_frame->DataY();
        uint8_t* out_u = output_frame->DataU();
        uint8_t* out_v = output_frame->DataV();

        int y_size = width_ * height_;
        int uv_plane_size = y_size / 4;

        // Copy Y plane
        for (int row = 0; row < height_; ++row) {
            std::memcpy(out_y + row * output_frame->StrideY(),
                        nv12_y + row * y_stride,
                        static_cast<size_t>(width_));
        }

        // Convert UV interleaved to U/V planar
        int uv_height = height_ / 2;
        for (int row = 0; row < uv_height; ++row) {
            for (int col = 0; col < width_ / 2; ++col) {
                out_u[row * output_frame->StrideU() + col] = nv12_uv[row * uv_stride + col * 2];
                out_v[row * output_frame->StrideV() + col] = nv12_uv[row * uv_stride + col * 2 + 1];
            }
        }

        // Create VideoFrame wrapper
        frame->set_timestamp_us(surface->GetTimeStamp());
        frame->set_duration_us(surface->GetDuration());
        frame->set_raw(output_frame);

        // Update stats
        stats_.decoded_bytes += y_size + uv_plane_size;
        if (encoded.is_keyframe()) {
            stats_.num_key_frames++;
        }

        return plugins::VideoCodecResult::OK;
    }

    plugins::VideoCodecResult Close() override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!opened_) {
            return plugins::VideoCodecResult::OK;
        }

        if (decoder_) {
            decoder_->Terminate();
            decoder_.Reset();
        }

        if (context_) {
            context_.Reset();
        }

        if (factory_) {
            factory_.Reset();
        }

        opened_ = false;
        NIMRTC_LOG_INFO("AMF decoder closed");
        return plugins::VideoCodecResult::OK;
    }

    plugins::VideoCodecStats GetStats() const override {
        return stats_;
    }

    void ResetStats() override {
        stats_ = plugins::VideoCodecStats{};
        frame_count_ = 0;
    }

private:
    AMFFactoryPtr factory_;
    AMFContextPtr context_;
    AMFComponentPtr decoder_;

    std::mutex mutex_;
    plugins::VideoCodecConfig cfg_;
    plugins::VideoCodecStats stats_;

    bool opened_;
    int width_;
    int height_;
    int64_t frame_count_;
};

} // namespace nimrtc::h264::amf_backend

namespace nimrtc::h264 {

/**
 * @brief Probe if AMF H.264 codec is available on this system
 * @return true if AMF runtime is present and functional
 */
bool amf_h264_available() noexcept {
#if defined(NIMRTC_PLUGINS_AMF_ON) && defined(_WIN32)
    try {
        AMFFactory* factory = nullptr;
        AMF_RESULT res = AMFInit(AMF_MEMORY_TYPE_DX9EX, AMF_FULL_VERSION, &factory);
        if (res != AMF_OK) {
            // Try DX11
            res = AMFInit(AMF_MEMORY_TYPE_DX11, AMF_FULL_VERSION, &factory);
        }
        if (res == AMF_OK && factory) {
            factory->Release();
            NIMRTC_LOG_INFO("AMF H.264 codec is available");
            return true;
        }
    } catch (...) {
        // AMF runtime not available
    }
    NIMRTC_LOG_INFO("AMF H.264 codec is NOT available");
    return false;
#else
    return false;
#endif
}

/**
 * @brief Factory function to create AMF H.264 encoder/decoder
 * @param config Codec configuration
 * @param direction ENCODE for encoder, DECODE for decoder
 * @return Unique pointer to video codec, or nullptr on failure
 */
std::unique_ptr<plugins::IVideoCodec> make_amf_h264_codec(
    const plugins::VideoCodecConfig& config,
    plugins::VideoCodecDirection direction) noexcept {
#if defined(NIMRTC_PLUGINS_AMF_ON) && defined(_WIN32)
    try {
        if (direction == plugins::VideoCodecDirection::ENCODE) {
            return std::make_unique<amf_backend::AmfEncoder>();
        } else {
            return std::make_unique<amf_backend::AmfDecoder>();
        }
    } catch (const std::exception& e) {
        NIMRTC_LOG_ERROR("Failed to create AMF codec: " << e.what());
        return nullptr;
    }
#else
    return nullptr;
#endif
}

} // namespace nimrtc::h264

#else // Stub implementation for non-Windows or AMF disabled

namespace nimrtc::h264 {

bool amf_h264_available() noexcept {
    return false;
}

std::unique_ptr<plugins::IVideoCodec> make_amf_h264_codec(
    const plugins::VideoCodecConfig&,
    plugins::VideoCodecDirection) noexcept {
    return nullptr;
}

} // namespace nimrtc::h264

#endif // NIMRTC_PLUGINS_AMF_ON && _WIN32
