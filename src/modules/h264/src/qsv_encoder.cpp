/**
 * @file qsv_encoder.cpp
 * @brief Intel Quick Sync Video (QSV / libvpl / oneVPL) H.264 encoder + decoder plugin for NimRTC
 */
#include <nimrtc/h264/hw_backends.hpp>

#if defined(NIMRTC_PLUGINS_QSV_ON)

#include <vpl/mfxvideo.h>
#include <vpl/mfxstructures.h>
#include <vpl/mfxdispatcher.h>

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

namespace nimrtc {
namespace h264 {
namespace qsv_backend {
namespace {

constexpr uint32_t START_CODE[] = {0x01000000, 0x00010000, 0x00000100};
constexpr uint8_t  START_CODE_LONG[] = {0x00, 0x00, 0x00, 0x01};
constexpr uint8_t  START_CODE_SHORT[] = {0x00, 0x00, 0x01};

inline uint32_t get_le32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

void convert_i420_to_nv12(const uint8_t* i420_y, const uint8_t* i420_u, const uint8_t* i420_v,
                           int width, int height, uint8_t* nv12_y, uint8_t* nv12_uv) {
    const int y_size = width * height;
    const int uv_size = y_size / 4;

    std::memcpy(nv12_y, i420_y, static_cast<size_t>(y_size));

    for (int i = 0; i < uv_size; ++i) {
        nv12_uv[i * 2] = i420_u[i];
        nv12_uv[i * 2 + 1] = i420_v[i];
    }
}

void convert_length_prefix_to_annex_b(const uint8_t* input, size_t input_len,
                                      std::vector<uint8_t>& output) {
    output.clear();
    size_t offset = 0;

    while (offset + 4 <= input_len) {
        uint32_t nal_length = get_le32(input + offset);
        offset += 4;

        if (nal_length == 0 || offset + nal_length > input_len) {
            break;
        }

        output.insert(output.end(), START_CODE_LONG, START_CODE_LONG + 4);
        output.insert(output.end(), input + offset, input + offset + nal_length);
        offset += nal_length;
    }
}

mfxU16 get_pic_struct_from_frame(const plugins::VideoFrame& frame) {
    if (frame.is_keyframe()) {
        return MFX_PICSTRUCT_UNKNOWN;
    }
    return MFX_PICSTRUCT_UNKNOWN;
}

} // namespace

class QsvEncoder : public plugins::IVideoCodec {
public:
    explicit QsvEncoder(const plugins::VideoCodecConfig& cfg)
        : cfg_(cfg)
        , session_(nullptr)
        , opened_(false)
        , frame_num_(0) {
        NIMRTC_LOG_INFO("QsvEncoder: created with config width=" << cfg_.width
                   << " height=" << cfg_.height << " bitrate=" << cfg_.bitrate_bps);
    }

    ~QsvEncoder() override {
        close();
    }

    plugins::VideoCodecResult open() override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (opened_) {
            NIMRTC_LOG_WARN("QsvEncoder: already opened");
            return plugins::VideoCodecResult::kVideoCodecNoError;
        }

        mfxVersion ver = {1, 11};
        mfxStatus st = MFXInit(MFX_IMPL_TYPE_HARDWARE, &ver, &session_);
        if (st != MFX_ERR_NONE) {
            st = MFXInit(MFX_IMPL_TYPE_ANY, &ver, &session_);
        }
        if (st != MFX_ERR_NONE) {
            NIMRTC_LOG_ERROR("QsvEncoder: MFXInit failed, status=" << st);
            session_ = nullptr;
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        std::memset(&params_, 0, sizeof(params_));
        params_.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

        params_.mfx.CodecId = MFX_CODEC_AVC;
        params_.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;

        params_.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
        params_.mfx.TargetBitrate = static_cast<mfxU32>(cfg_.bitrate_bps);

        params_.mfx.FrameInfo.FrameRateExtN = cfg_.framerate > 0 ? cfg_.framerate : 30;
        params_.mfx.FrameInfo.FrameRateExtD = 1;

        params_.mfx.GopPicSize = cfg_.keyframe_interval > 0 ? cfg_.keyframe_interval : 300;
        params_.mfx.GopRefDist = 1;
        params_.mfx.NumSlice = 1;

        params_.mfx.Profile = MFX_PROFILE_AVC_BASELINE;
        params_.mfx.Level = MFX_LEVEL_UNKNOWN;

        params_.mfx.Width = cfg_.width;
        params_.mfx.Height = cfg_.height;
        params_.mfx.FrameInfo.CropW = cfg_.width;
        params_.mfx.FrameInfo.CropH = cfg_.height;
        params_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        params_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;

        params_.mfx.BufferSizeInKB = 2000;

        mfxVideoParam in_params = params_;
        mfxVideoParam out_params = {};

        st = MFXVideoENCODE_Query(session_, &in_params, &out_params);
        if (st != MFX_ERR_NONE && st != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
            NIMRTC_LOG_WARN("QsvEncoder: MFXVideoENCODE_Query returned " << st
                       << ", trying anyway");
        }
        if (st == MFX_ERR_NONE || st == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
            params_ = out_params;
        }

        st = MFXVideoENCODE_Init(session_, &params_);
        if (st != MFX_ERR_NONE) {
            NIMRTC_LOG_ERROR("QsvEncoder: MFXVideoENCODE_Init failed, status=" << st);
            MFXClose(session_);
            session_ = nullptr;
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        mfxFrameAllocRequest alloc_request = {};
        st = MFXVideoENCODE_QueryIOSurf(session_, &params_, &alloc_request);
        if (st != MFX_ERR_NONE) {
            NIMRTC_LOG_WARN("QsvEncoder: MFXVideoENCODE_QueryIOSurf failed: " << st);
            num_surfaces_ = 4;
        } else {
            num_surfaces_ = static_cast<int>(alloc_request.NumFrameSuggested);
            if (num_surfaces_ < 4) {
                num_surfaces_ = 4;
            }
        }

        NIMRTC_LOG_INFO("QsvEncoder: allocated " << num_surfaces_ << " surfaces");

        input_surfaces_.resize(num_surfaces_ + 2);
        for (int i = 0; i < static_cast<int>(input_surfaces_.size()); ++i) {
            std::memset(&input_surfaces_[i], 0, sizeof(mfxFrameSurface1));
            input_surfaces_[i].Info = params_.mfx.FrameInfo;
            input_surfaces_[i].Data.MemId = nullptr;
        }

        mfxU16 width16 = static_cast<mfxU16>((cfg_.width + 15) & ~15);
        mfxU16 height16 = static_cast<mfxU16>((cfg_.height + 15) & ~15);
        nv12_buffer_.resize(static_cast<size_t>(width16) * height16 * 3 / 2);

        for (int i = 0; i < static_cast<int>(input_surfaces_.size()); ++i) {
            input_surfaces_[i].Data.Y = nv12_buffer_.data();
            input_surfaces_[i].Data.U = nv12_buffer_.data() + width16 * height16;
            input_surfaces_[i].Data.V = input_surfaces_[i].Data.U + 1;
            input_surfaces_[i].Data.PitchY = width16;
            input_surfaces_[i].Data.PitchUV = width16;
            input_surfaces_[i].Data.MemId = reinterpret_cast<mfxHDL>(
                const_cast<mfxFrameSurface1*>(&input_surfaces_[i]));
        }

        mfxVideoParam enc_params_check = {};
        st = MFXVideoENCODE_GetVideoParam(session_, &enc_params_check);
        if (st == MFX_ERR_NONE) {
            NIMRTC_LOG_INFO("QsvEncoder: actual FrameRate="
                       << enc_params_check.mfx.FrameInfo.FrameRateExtN << "/"
                       << enc_params_check.mfx.FrameInfo.FrameRateExtD
                       << " Width=" << enc_params_check.mfx.Width
                       << " Height=" << enc_params_check.mfx.Height);
        }

        opened_ = true;
        NIMRTC_LOG_INFO("QsvEncoder: opened successfully");
        return plugins::VideoCodecResult::kVideoCodecNoError;
    }

    plugins::VideoCodecResult encode(const plugins::VideoFrame& in_frame,
                                     std::vector<uint8_t>& out_encoded) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!opened_) {
            NIMRTC_LOG_ERROR("QsvEncoder: not opened");
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        mfxFrameSurface1* surface = nullptr;
        for (auto& surf : input_surfaces_) {
            mfxSyncPoint sync = nullptr;
            if (surf.Data.Locked == 0) {
                surface = &surf;
                break;
            }
        }

        if (!surface) {
            NIMRTC_LOG_WARN("QsvEncoder: no free surface, dropping frame");
            stats_.num_dropped_frames++;
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        const uint8_t* frame_data = in_frame.cpu_buffer();
        if (!frame_data) {
            NIMRTC_LOG_ERROR("QsvEncoder: no CPU buffer in frame");
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        int width = cfg_.width;
        int height = cfg_.height;

        const uint8_t* y_plane = frame_data;
        const uint8_t* u_plane = y_plane + width * height;
        const uint8_t* v_plane = u_plane + (width * height / 4);

        mfxU16 width16 = static_cast<mfxU16>((width + 15) & ~15);
        mfxU16 height16 = static_cast<mfxU16>((height + 15) & ~15);
        uint8_t* surf_y = surface->Data.Y;
        uint8_t* surf_uv = surface->Data.U;

        for (int row = 0; row < height; ++row) {
            std::memcpy(surf_y + row * width16, y_plane + row * width, static_cast<size_t>(width));
        }

        for (int row = 0; row < height / 2; ++row) {
            for (int col = 0; col < width / 2; ++col) {
                surf_uv[row * width16 + col * 2] = u_plane[row * (width / 2) + col];
                surf_uv[row * width16 + col * 2 + 1] = v_plane[row * (width / 2) + col];
            }
        }

        surface->Data.TimeStamp = in_frame.timestamp_ns() / 100;
        surface->Data.FrameType = in_frame.is_keyframe() ? MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR
                                                         : MFX_FRAMETYPE_P;

        mfxEncodeCtrl enc_ctrl = {};
        enc_ctrl.FrameType = surface->Data.FrameType;

        mfxBitstream bitstream = {};
        std::vector<uint8_t> temp_bitstream(1024 * 1024);
        bitstream.MaxLength = static_cast<mfxU32>(temp_bitstream.size());
        bitstream.Data = temp_bitstream.data();
        bitstream.DataLength = 0;
        bitstream.DataOffset = 0;

        mfxSyncPoint sync_point = nullptr;
        mfxStatus st = MFX_ERR_NONE;

        for (int retry = 0; retry < 10; ++retry) {
            st = MFXVideoENCODE_FrameEncode(session_, &enc_ctrl, surface, &sync_point,
                                            &bitstream, MFX_POLL_BLOCKING);

            if (st == MFX_WRN_DEVICE_BUSY) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            break;
        }

        if (st != MFX_ERR_NONE && st != MFX_ERR_MORE_DATA && st != MFX_WRN_VIDEO_PARAM_CHANGED) {
            NIMRTC_LOG_ERROR("QsvEncoder: MFXVideoENCODE_FrameEncode failed, status=" << st);
            stats_.num_encode_errors++;
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        if (bitstream.DataLength > 4) {
            std::vector<uint8_t> annex_b;
            convert_length_prefix_to_annex_b(bitstream.Data, bitstream.DataLength, annex_b);

            if (!annex_b.empty()) {
                out_encoded.insert(out_encoded.end(), annex_b.begin(), annex_b.end());
                frame_num_++;
                stats_.num_encoded_frames++;
                stats_.total_bytes_encoded += out_encoded.size();
                NIMRTC_LOG_DEBUG("QsvEncoder: encoded frame " << frame_num_
                             << " size=" << out_encoded.size()
                             << " is_keyframe=" << in_frame.is_keyframe());
                return plugins::VideoCodecResult::kVideoCodecNoError;
            }
        }

        return plugins::VideoCodecResult::kVideoCodecNoError;
    }

    plugins::VideoCodecResult close() override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!opened_) {
            return plugins::VideoCodecResult::kVideoCodecNoError;
        }

        if (session_) {
            MFXVideoENCODE_Close(session_);
            MFXClose(session_);
            session_ = nullptr;
        }

        input_surfaces_.clear();
        opened_ = false;
        NIMRTC_LOG_INFO("QsvEncoder: closed");
        return plugins::VideoCodecResult::kVideoCodecNoError;
    }

    plugins::VideoCodecStats get_stats() const override {
        return stats_;
    }

    void reset_stats() override {
        stats_ = plugins::VideoCodecStats{};
    }

    std::string get_implementation_name() const override {
        return "QsvEncoder";
    }

private:
    plugins::VideoCodecConfig cfg_;
    mfxSession session_;
    mfxVideoParam params_;
    std::vector<mfxFrameSurface1> input_surfaces_;
    std::vector<uint8_t> nv12_buffer_;
    std::mutex mutex_;
    plugins::VideoCodecStats stats_;
    bool opened_;
    int frame_num_;
    int num_surfaces_;
};

class QsvDecoder : public plugins::IVideoCodec {
public:
    explicit QsvDecoder(const plugins::VideoCodecConfig& cfg)
        : cfg_(cfg)
        , session_(nullptr)
        , opened_(false) {
        NIMRTC_LOG_INFO("QsvDecoder: created with config width=" << cfg_.width
                   << " height=" << cfg_.height);
    }

    ~QsvDecoder() override {
        close();
    }

    plugins::VideoCodecResult open() override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (opened_) {
            NIMRTC_LOG_WARN("QsvDecoder: already opened");
            return plugins::VideoCodecResult::kVideoCodecNoError;
        }

        mfxVersion ver = {1, 11};
        mfxStatus st = MFXInit(MFX_IMPL_TYPE_HARDWARE, &ver, &session_);
        if (st != MFX_ERR_NONE) {
            st = MFXInit(MFX_IMPL_TYPE_ANY, &ver, &session_);
        }
        if (st != MFX_ERR_NONE) {
            NIMRTC_LOG_ERROR("QsvDecoder: MFXInit failed, status=" << st);
            session_ = nullptr;
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        std::memset(&params_, 0, sizeof(params_));
        params_.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

        params_.mfx.CodecId = MFX_CODEC_AVC;
        params_.mfx.Width = cfg_.width > 0 ? cfg_.width : 1920;
        params_.mfx.Height = cfg_.height > 0 ? cfg_.height : 1080;

        params_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        params_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;

        mfxVideoParam in_params = params_;
        mfxVideoParam out_params = {};

        st = MFXVideoDECODE_Query(session_, &in_params, &out_params);
        if (st != MFX_ERR_NONE && st != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
            NIMRTC_LOG_WARN("QsvDecoder: MFXVideoDECODE_Query returned " << st);
        }
        if (st == MFX_ERR_NONE || st == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
            params_ = out_params;
        }

        st = MFXVideoDECODE_Init(session_, &params_);
        if (st != MFX_ERR_NONE) {
            NIMRTC_LOG_ERROR("QsvDecoder: MFXVideoDECODE_Init failed, status=" << st);
            MFXClose(session_);
            session_ = nullptr;
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        mfxFrameAllocRequest alloc_request = {};
        st = MFXVideoDECODE_QueryIOSurf(session_, &params_, &alloc_request);
        if (st != MFX_ERR_NONE) {
            NIMRTC_LOG_WARN("QsvDecoder: MFXVideoDECODE_QueryIOSurf failed: " << st);
            num_surfaces_ = 4;
        } else {
            num_surfaces_ = static_cast<int>(alloc_request.NumFrameSuggested);
            if (num_surfaces_ < 4) {
                num_surfaces_ = 4;
            }
        }

        output_surfaces_.resize(num_surfaces_ + 2);
        for (auto& surf : output_surfaces_) {
            std::memset(&surf, 0, sizeof(mfxFrameSurface1));
            surf.Info = params_.mfx.FrameInfo;
        }

        mfxU16 width16 = static_cast<mfxU16>((params_.mfx.Width + 15) & ~15);
        mfxU16 height16 = static_cast<mfxU16>((params_.mfx.Height + 15) & ~15);
        decoded_buffer_.resize(static_cast<size_t>(width16) * height16 * 3 / 2);

        for (size_t i = 0; i < output_surfaces_.size(); ++i) {
            output_surfaces_[i].Data.Y = decoded_buffer_.data();
            output_surfaces_[i].Data.U = decoded_buffer_.data() + width16 * height16;
            output_surfaces_[i].Data.V = output_surfaces_[i].Data.U + 1;
            output_surfaces_[i].Data.PitchY = width16;
            output_surfaces_[i].Data.PitchUV = width16;
        }

        pending_bs_.MaxLength = 1024 * 1024;
        pending_bs_.Data = new uint8_t[pending_bs_.MaxLength];
        pending_bs_->DataLength = 0;

        opened_ = true;
        NIMRTC_LOG_INFO("QsvDecoder: opened successfully with " << num_surfaces_ << " surfaces");
        return plugins::VideoCodecResult::kVideoCodecNoError;
    }

    plugins::VideoCodecResult decode(const std::vector<uint8_t>& in_encoded,
                                     plugins::VideoFrame& out_frame) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!opened_) {
            NIMRTC_LOG_ERROR("QsvDecoder: not opened");
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        if (in_encoded.empty()) {
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        mfxBitstream bitstream = {};
        std::vector<uint8_t> temp_bs(in_encoded.begin(), in_encoded.end());
        bitstream.Data = temp_bs.data();
        bitstream.DataLength = static_cast<mfxU32>(temp_bs.size());
        bitstream.DataOffset = 0;
        bitstream.MaxLength = bitstream.DataLength;

        mfxFrameSurface1* decoded_surface = nullptr;
        mfxStatus st = MFX_ERR_NONE;

        for (int retry = 0; retry < 10; ++retry) {
            st = MFXVideoDECODE_DecodeFrame(session_, &bitstream, nullptr, &decoded_surface);

            if (st == MFX_WRN_DEVICE_BUSY) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (st == MFX_WRN_VIDEO_PARAM_CHANGED || st == MFX_ERR_MORE_SURFACE) {
                continue;
            }
            break;
        }

        if (st != MFX_ERR_NONE && st != MFX_WRN_VIDEO_PARAM_CHANGED &&
            st != MFX_ERR_MORE_SURFACE && st != MFX_ERR_MORE_DATA) {
            NIMRTC_LOG_ERROR("QsvDecoder: MFXVideoDECODE_DecodeFrame failed, status=" << st);
            stats_.num_decode_errors++;
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        if (!decoded_surface) {
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        if (bitstream.DataLength == 0 && bitstream.MaxLength == 0) {
            stats_.num_decoded_frames++;
            return plugins::VideoCodecResult::kVideoCodecNoError;
        }

        mfxFrameSurface1* work_surface = decoded_surface;

        mfxU16 width = work_surface->Info.CropW;
        mfxU16 height = work_surface->Info.CropH;
        mfxU16 pitch_y = work_surface->Data.PitchY;

        size_t frame_size = static_cast<size_t>(width) * height * 3 / 2;
        std::vector<uint8_t> i420_frame(frame_size);

        uint8_t* y_out = i420_frame.data();
        uint8_t* u_out = y_out + width * height;
        uint8_t* v_out = u_out + (width * height / 4);

        uint8_t* src_y = work_surface->Data.Y;
        uint8_t* src_uv = work_surface->Data.U;

        for (mfxU16 row = 0; row < height; ++row) {
            std::memcpy(y_out + row * width, src_y + row * pitch_y, width);
        }

        mfxU16 pitch_uv = work_surface->Data.PitchUV;
        for (mfxU16 row = 0; row < height / 2; ++row) {
            for (mfxU16 col = 0; col < width / 2; ++col) {
                u_out[row * (width / 2) + col] = src_uv[row * pitch_uv + col * 2];
                v_out[row * (width / 2) + col] = src_uv[row * pitch_uv + col * 2 + 1];
            }
        }

        if (!out_frame.set_buffer(i420_frame.data(), i420_frame.size(), true)) {
            NIMRTC_LOG_ERROR("QsvDecoder: failed to set frame buffer");
            return plugins::VideoCodecResult::kVideoCodecError;
        }

        out_frame.set_timestamp_ns(work_surface->Data.TimeStamp * 100);
        out_frame.set_width(width);
        out_frame.set_height(height);
        out_frame.set_format(plugins::VideoFrameFormat::kVideoFrameFormatI420);

        bool is_keyframe = false;
        mfxU16 frame_type = work_surface->Info.FrameType;
        if (frame_type & (MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR)) {
            is_keyframe = true;
        }
        out_frame.set_is_keyframe(is_keyframe);

        stats_.num_decoded_frames++;
        stats_.total_bytes_decoded += in_encoded.size();

        NIMRTC_LOG_DEBUG("QsvDecoder: decoded frame size=" << in_encoded.size()
                      << " is_keyframe=" << is_keyframe);

        return plugins::VideoCodecResult::kVideoCodecNoError;
    }

    plugins::VideoCodecResult close() override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!opened_) {
            return plugins::VideoCodecResult::kVideoCodecNoError;
        }

        if (session_) {
            MFXVideoDECODE_Close(session_);
            MFXClose(session_);
            session_ = nullptr;
        }

        output_surfaces_.clear();
        decoded_buffer_.clear();
        if (pending_bs_.Data) {
            delete[] pending_bs_.Data;
            pending_bs_.Data = nullptr;
        }

        opened_ = false;
        NIMRTC_LOG_INFO("QsvDecoder: closed");
        return plugins::VideoCodecResult::kVideoCodecNoError;
    }

    plugins::VideoCodecStats get_stats() const override {
        return stats_;
    }

    void reset_stats() override {
        stats_ = plugins::VideoCodecStats{};
    }

    std::string get_implementation_name() const override {
        return "QsvDecoder";
    }

private:
    plugins::VideoCodecConfig cfg_;
    mfxSession session_;
    mfxVideoParam params_;
    std::vector<mfxFrameSurface1> output_surfaces_;
    std::vector<uint8_t> decoded_buffer_;
    mfxBitstream pending_bs_{};
    std::mutex mutex_;
    plugins::VideoCodecStats stats_;
    bool opened_;
    int num_surfaces_;
};

bool qsv_h264_available() noexcept {
    mfxSession test_session = nullptr;
    mfxVersion ver = {1, 11};

    mfxStatus st = MFXInit(MFX_IMPL_TYPE_HARDWARE, &ver, &test_session);
    if (st != MFX_ERR_NONE) {
        st = MFXInit(MFX_IMPL_TYPE_ANY, &ver, &test_session);
    }

    if (st != MFX_ERR_NONE) {
        NIMRTC_LOG_INFO("QSV H.264: not available (MFXInit failed: " << st << ")");
        return false;
    }

    mfxVideoParam params = {};
    params.mfx.CodecId = MFX_CODEC_AVC;
    params.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    params.mfx.Width = 640;
    params.mfx.Height = 480;
    params.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    params.mfx.FrameInfo.CropW = 640;
    params.mfx.FrameInfo.CropH = 480;
    params.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    params.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    params.mfx.TargetBitrate = 500000;
    params.mfx.FrameInfo.FrameRateExtN = 30;
    params.mfx.FrameInfo.FrameRateExtD = 1;
    params.mfx.GopPicSize = 30;
    params.mfx.GopRefDist = 1;
    params.mfx.Profile = MFX_PROFILE_AVC_BASELINE;
    params.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

    mfxVideoParam out_params = {};
    st = MFXVideoENCODE_Query(test_session, &params, &out_params);

    MFXClose(test_session);

    if (st == MFX_ERR_NONE || st == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
        NIMRTC_LOG_INFO("QSV H.264: available");
        return true;
    }

    NIMRTC_LOG_INFO("QSV H.264: not available (MFXVideoENCODE_Query failed: " << st << ")");
    return false;
}

std::unique_ptr<plugins::IVideoCodec> make_qsv_h264_codec(const plugins::VideoCodecConfig& cfg) {
    if (cfg.mode == plugins::VideoCodecMode::kVideoCodecModeEncoder) {
        return std::make_unique<QsvEncoder>(cfg);
    } else if (cfg.mode == plugins::VideoCodecMode::kVideoCodecModeDecoder) {
        return std::make_unique<QsvDecoder>(cfg);
    }
    NIMRTC_LOG_ERROR("QSV H.264: unsupported codec mode");
    return nullptr;
}

} // namespace qsv_backend

bool qsv_h264_available() noexcept {
    return qsv_backend::qsv_h264_available();
}

std::unique_ptr<plugins::IVideoCodec> make_qsv_h264_codec(const plugins::VideoCodecConfig& cfg) {
    return qsv_backend::make_qsv_h264_codec(cfg);
}

} // namespace h264
} // namespace nimrtc

#else

namespace nimrtc {
namespace h264 {

bool qsv_h264_available() noexcept {
    return false;
}

std::unique_ptr<plugins::IVideoCodec> make_qsv_h264_codec(const plugins::VideoCodecConfig&) {
    return nullptr;
}

} // namespace h264
} // namespace nimrtc

#endif // NIMRTC_PLUGINS_QSV_ON
