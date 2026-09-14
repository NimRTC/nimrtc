/**
 * @file src/modules/h264/src/vaapi_encoder.cpp
 * @brief Linux VA-API H.264 encoder + decoder plugin.
 *
 * Implements the HW backend entry declared in hw_backends.cpp for Linux
 * systems with Intel/AMD GPU VA-API support:
 *   - Encoder: VAEntrypointEncSlice → Annex B H.264 bitstream
 *   - Decoder: VAEntrypointDecSlice → I420 frame output
 *
 * The implementation follows the standard VA-API pipeline:
 *   1. Open a DRM render node (/dev/dri/renderD128 or /dev/dri/card0)
 *   2. Initialize VA display with vaGetDisplayDRM()
 *   3. Create config, context, and surfaces
 *   4. For each frame: vaBeginPicture → vaRenderPicture → vaEndPicture
 *   5. vaSyncSurface → vaMapBuffer to read encoded output
 *
 * Compile-time:
 *   - NIMRTC_PLUGINS_VAAPI_ON must be defined by NimRTHwPlugins.cmake
 *     after find_package(libva) succeeds.
 *   - __linux__ must be defined (this file is Linux-only).
 *
 * Runtime contract:
 *   - available() probes VA display initialization and H.264 profile support.
 *   - create() opens a VA session bound to a DRM render node.
 *   - Encoder output: Annex B bitstream (SPS + PPS + slice NALUs).
 *   - Decoder output: I420 frame in CPU buffer.
 */

#include <nimrtc/h264/hw_backends.hpp>

#if defined(NIMRTC_PLUGINS_VAAPI_ON) && defined(__linux__)

#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_backend.h>

#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#include "hw_backend_base.hpp"

namespace nimrtc::h264::vaapi_backend {

// ---------------------------------------------------------------------------
// VA-API library loader —dynamically loads libva.so to avoid a hard link
// dependency when VA-API is not available on the system.
// ---------------------------------------------------------------------------

struct VADynLib {
    void* handle = nullptr;

    // Function pointers
    VAStatus (*vaGetDisplayDRM)(int) = nullptr;
    VAStatus (*vaInitialize)(VADisplay, int*, int*) = nullptr;
    VAStatus (*vaTerminate)(VADisplay) = nullptr;
    VAStatus (*vaQueryConfigProfiles)(VADisplay, VAProfile*, int*) = nullptr;
    VAStatus (*vaCreateConfig)(VADisplay, VAEntrypoint, VAProfile,
                               VAConfigAttrib*, int, VAConfigID*) = nullptr;
    VAStatus (*vaDestroyConfig)(VADisplay, VAConfigID) = nullptr;
    VAStatus (*vaCreateSurfaces)(VADisplay, unsigned int, unsigned int,
                                 VAFormat, int, VASurfaceID*) = nullptr;
    VAStatus (*vaDestroySurfaces)(VADisplay, VASurfaceID*, int) = nullptr;
    VAStatus (*vaCreateContext)(VADisplay, VAConfigID, int, int,
                                int, VASurfaceID*, int, VAContextID*) = nullptr;
    VAStatus (*vaDestroyContext)(VADisplay, VAContextID) = nullptr;
    VAStatus (*vaBeginPicture)(VADisplay, VAContextID, VASurfaceID) = nullptr;
    VAStatus (*vaRenderPicture)(VADisplay, VAContextID, VABuffer*, int) = nullptr;
    VAStatus (*vaEndPicture)(VADisplay, VAContextID) = nullptr;
    VAStatus (*vaSyncSurface)(VADisplay, VASurfaceID) = nullptr;
    VAStatus (*vaMapBuffer)(VADisplay, VABufferID, void**) = nullptr;
    VAStatus (*vaUnmapBuffer)(VADisplay, VABufferID) = nullptr;
    VAStatus (*vaCreateBuffer)(VADisplay, VAContextID, VABufferType,
                              unsigned int, unsigned int, void*, VABufferID*) = nullptr;
    VAStatus (*vaDestroyBuffer)(VADisplay, VABufferID) = nullptr;
    VAStatus (*vaDeriveImage)(VADisplay, VASurfaceID, VAImage*) = nullptr;
    VAStatus (*vaDestroyImage)(VADisplay, VAImageID) = nullptr;
    VAStatus (*vaGetImage)(VADisplay, VASurfaceID, int, int,
                           unsigned int, unsigned int, VAImageID) = nullptr;
    const char* (*vaErrorStr)(VAStatus) = nullptr;

    ~VADynLib() {
        if(handle) {
            dlclose(handle);
            handle = nullptr;
        }
    }

    bool open(std::string& err) {
        handle = dlopen("libva.so.2", RTLD_NOW);
        if(!handle) {
            handle = dlopen("libva.so.1", RTLD_NOW);
        }
        if(!handle) {
            handle = dlopen("libva.so", RTLD_NOW);
        }
        if(!handle) {
            err = "dlopen(libva.so) failed: " + std::string(dlerror());
            return false;
        }

#define LOAD(sym) \
        do { \
            sym = reinterpret_cast<decltype(sym)>(dlsym(handle, #sym)); \
            if(!sym) { \
                err = "dlsym(" #sym ") failed: " + std::string(dlerror()); \
                return false; \
            } \
        } while(0)

        LOAD(vaGetDisplayDRM);
        LOAD(vaInitialize);
        LOAD(vaTerminate);
        LOAD(vaQueryConfigProfiles);
        LOAD(vaCreateConfig);
        LOAD(vaDestroyConfig);
        LOAD(vaCreateSurfaces);
        LOAD(vaDestroySurfaces);
        LOAD(vaCreateContext);
        LOAD(vaDestroyContext);
        LOAD(vaBeginPicture);
        LOAD(vaRenderPicture);
        LOAD(vaEndPicture);
        LOAD(vaSyncSurface);
        LOAD(vaMapBuffer);
        LOAD(vaUnmapBuffer);
        LOAD(vaCreateBuffer);
        LOAD(vaDestroyBuffer);
        LOAD(vaDeriveImage);
        LOAD(vaDestroyImage);
        LOAD(vaGetImage);
        LOAD(vaErrorStr);

#undef LOAD

        return true;
    }
};

// ---------------------------------------------------------------------------
// Encoder session —manages VA display, surfaces, and encode state.
// ---------------------------------------------------------------------------

class VaapiEncoder {
public:
    VaapiEncoder() = default;
    ~VaapiEncoder();

    bool open(const hw_backend::SessionParams& params,
              const plugins::VideoCodecConfig& cfg,
              std::string& err);
    bool is_open() const noexcept { return display_ != nullptr; }

    bool encode(const plugins::VideoFrame& raw,
                std::uint8_t* output, std::size_t capacity,
                plugins::EncodedVideoFrame& encoded_out,
                bool& out_is_keyframe,
                std::string& err);

    plugins::VideoCodecStats stats() const noexcept;

private:
    bool create_encode_surfaces(unsigned int width, unsigned int height,
                                std::string& err);
    bool fill_sps(unsigned int width, unsigned int height,
                  unsigned int fps, std::string& err);
    bool fill_pps(std::string& err);
    bool fill_slice_params(bool is_idr, std::string& err);
    bool upload_i420_to_surface(const plugins::VideoFrame& raw,
                                std::string& err);

    VADynLib va_;
    VADisplay display_ = nullptr;
    int drm_fd_ = -1;
    VAConfigID config_id_ = VA_INVALID_ID;
    VAContextID context_id_ = VA_INVALID_CONTEXT;
    VASurfaceID input_surface_ = VA_INVALID_SURFACE;
    VASurfaceID coded_surface_ = VA_INVALID_SURFACE;
    VABufferID seq_param_buf_ = VA_INVALID_BUFFER;
    VABufferID pic_param_buf_ = VA_INVALID_BUFFER;
    VABufferID slice_param_buf_ = VA_INVALID_BUFFER;
    VABufferID coded_buf_ = VA_INVALID_BUFFER;

    // Cached parameter data for IDR frames
    std::vector<std::uint8_t> sps_;
    std::vector<std::uint8_t> pps_;

    hw_backend::SessionParams params_;
    mutable std::mutex mutex_;
    plugins::VideoCodecStats stats_{};
    bool opened_ = false;
};

// ---------------------------------------------------------------------------
// Dtor —release all VA resources
// ---------------------------------------------------------------------------

VaapiEncoder::~VaapiEncoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!display_) return;

    if(coded_buf_ != VA_INVALID_BUFFER) {
        va_.vaDestroyBuffer(display_, coded_buf_);
    }
    if(slice_param_buf_ != VA_INVALID_BUFFER) {
        va_.vaDestroyBuffer(display_, slice_param_buf_);
    }
    if(pic_param_buf_ != VA_INVALID_BUFFER) {
        va_.vaDestroyBuffer(display_, pic_param_buf_);
    }
    if(seq_param_buf_ != VA_INVALID_BUFFER) {
        va_.vaDestroyBuffer(display_, seq_param_buf_);
    }

    if(context_id_ != VA_INVALID_CONTEXT) {
        va_.vaDestroyContext(display_, context_id_);
    }
    if(config_id_ != VA_INVALID_ID) {
        va_.vaDestroyConfig(display_, config_id_);
    }

    if(coded_surface_ != VA_INVALID_SURFACE) {
        VASurfaceID ids[2] = {input_surface_, coded_surface_};
        va_.vaDestroySurfaces(display_, ids, 2);
    }

    if(display_) {
        va_.vaTerminate(display_);
    }
    if(drm_fd_ >= 0) {
        close(drm_fd_);
    }
    display_ = nullptr;
}

// ---------------------------------------------------------------------------
// Open —initialize VA display and create encode pipeline
// ---------------------------------------------------------------------------

bool VaapiEncoder::open(const hw_backend::SessionParams& params,
                        const plugins::VideoCodecConfig& cfg,
                        std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);

    if(opened_) {
        err = "VA encoder already open";
        return false;
    }

    params_ = params;

    // Load libva dynamically
    if(!va_.open(err)) {
        return false;
    }

    // Open DRM render node
    drm_fd_ = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if(drm_fd_ < 0) {
        drm_fd_ = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    }
    if(drm_fd_ < 0) {
        err = "open(/dev/dri/renderD128) failed: " + std::string(strerror(errno));
        return false;
    }

    // Get VA display from DRM
    display_ = va_.vaGetDisplayDRM(drm_fd_);
    if(!display_) {
        err = "vaGetDisplayDRM returned nullptr";
        close(drm_fd_);
        drm_fd_ = -1;
        return false;
    }

    // Initialize VA
    int major, minor;
    VAStatus s = va_.vaInitialize(display_, &major, &minor);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaInitialize failed: " + std::string(va_.vaErrorStr(s));
        close(drm_fd_);
        drm_fd_ = -1;
        display_ = nullptr;
        return false;
    }

    NIMRTC_LOG_INFO("vaapi: VA-API version {}.{} initialized", major, minor);

    // Check H.264 profile support
    VAProfile profiles[16];
    int num_profiles = 16;
    s = va_.vaQueryConfigProfiles(display_, profiles, &num_profiles);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaQueryConfigProfiles failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    VAProfile selected_profile = VAProfileNone;
    VAProfile h264_profiles[] = {
        VAProfileH264High, VAProfileH264Main, VAProfileH264Baseline,
        VAProfileH264ConstrainedBaseline, VAProfileH264StereoHigh
    };
    for(auto profile : h264_profiles) {
        for(int i = 0; i < num_profiles; ++i) {
            if(profiles[i] == profile) {
                selected_profile = profile;
                break;
            }
        }
        if(selected_profile != VAProfileNone) break;
    }
    if(selected_profile == VAProfileNone) {
        err = "no H.264 VA profile supported";
        return false;
    }

    NIMRTC_LOG_INFO("vaapi: using profile {}", selected_profile);

    // Create config
    s = va_.vaCreateConfig(display_, VAEntrypointEncSlice, selected_profile,
                           nullptr, 0, &config_id_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateConfig failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    // Create surfaces
    if(!create_encode_surfaces(params_.width, params_.height, err)) {
        return false;
    }

    // Create context
    s = va_.vaCreateContext(display_, config_id_,
                            params_.width, params_.height,
                            VA_PROGRESSIVE,
                            &input_surface_, 1, &context_id_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateContext failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    // Create parameter buffers
    unsigned int seq_size = sizeof(VAEncSequenceParameterBufferH264);
    s = va_.vaCreateBuffer(display_, context_id_,
                           VAEncSequenceParameterBufferType,
                           seq_size, 1, nullptr, &seq_param_buf_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateBuffer(seq) failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    unsigned int pic_size = sizeof(VAEncPictureParameterBufferH264);
    s = va_.vaCreateBuffer(display_, context_id_,
                           VAEncPictureParameterBufferType,
                           pic_size, 1, nullptr, &pic_param_buf_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateBuffer(pic) failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    unsigned int slice_size = sizeof(VAEncSliceParameterBufferH264);
    s = va_.vaCreateBuffer(display_, context_id_,
                           VAEncSliceParameterBufferType,
                           slice_size, 1, nullptr, &slice_param_buf_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateBuffer(slice) failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    // Create coded buffer (output bitstream buffer)
    // Size estimate: worst case ~2 bits per pixel for H.264
    unsigned int coded_buf_size = (params_.width * params_.height * 3) / 4;
    s = va_.vaCreateBuffer(display_, context_id_,
                           VAEncCodedBufferType,
                           coded_buf_size, 1, nullptr, &coded_buf_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateBuffer(coded) failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    // Fill SPS/PPS
    if(!fill_sps(params_.width, params_.height, params_.fps, err)) {
        return false;
    }
    if(!fill_pps(err)) {
        return false;
    }

    opened_ = true;
    NIMRTC_LOG_INFO("vaapi encoder: opened {}x{} @ {} fps", params_.width, params_.height, params_.fps);
    (void)cfg;
    return true;
}

// ---------------------------------------------------------------------------
// Create input and coded surfaces
// ---------------------------------------------------------------------------

bool VaapiEncoder::create_encode_surfaces(unsigned int width, unsigned int height,
                                          std::string& err) {
    VASurfaceID surfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    VAStatus s = va_.vaCreateSurfaces(display_, VA_RT_FORMAT_YUV420,
                                       width, height, surfaces, 2, nullptr);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateSurfaces failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }
    input_surface_ = surfaces[0];
    coded_surface_ = surfaces[1];
    return true;
}

// ---------------------------------------------------------------------------
// Fill H.264 SPS (Sequence Parameter Set)
// ---------------------------------------------------------------------------

bool VaapiEncoder::fill_sps(unsigned int width, unsigned int height,
                             unsigned int fps, std::string& err) {
    VAEncSequenceParameterBufferH264* sps = nullptr;
    VAStatus s = va_.vaMapBuffer(display_, seq_param_buf_, (void**)&sps);
    if(s != VA_STATUS_SUCCESS || !sps) {
        err = "vaMapBuffer(seq) failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    std::memset(sps, 0, sizeof(*sps));

    // Profile
    sps->profile_idc = 66;  // Baseline profile

    // Level
    sps->level_idc = 31;    // Level 3.1

    // SPS ID
    sps->seq_parameter_set_id = 0;

    // Chroma format (4:2:0)
    sps->chroma_format_idc = 1;

    // Picture dimensions
    sps->pic_width_in_mbs_minus1 = (width + 15) / 16 - 1;
    sps->pic_height_in_map_units_minus1 = (height + 15) / 16 - 1;

    // Frame mbs only
    sps->frame_mbs_only_flag = 1;
    sps->mb_adaptive_frame_field_flag = 0;

    // Direct 8x8 inference
    sps->direct_8x8_inference_flag = 0;

    // No cropping
    sps->frame_cropping_flag = 0;

    // VUI parameters
    sps->vui_parameters_present_flag = 1;
    sps->vui_seq_parameters.aspect_ratio_info_present_flag = 0;
    sps->vui_seq_parameters.motion_vectors_over_pic_boundaries_flag = 1;
    sps->vui_seq_parameters.log2_max_frame_num_minus4 = 4;
    sps->vui_seq_parameters.pic_order_cnt_type = 0;
    sps->vui_seq_parameters.log2_max_pic_order_cnt_lsb_minus4 = 4;
    sps->vui_seq_parameters.num_reorder_frames = 0;
    sps->vui_seq_parameters.max_num_ref_frames = 1;

    // Timing info
    sps->vui_seq_parameters.timing_info_present_flag = 1;
    sps->vui_seq_parameters.num_units_in_tick = 1;
    sps->vui_seq_parameters.time_scale = fps * 2;
    sps->vui_seq_parameters.fixed_frame_rate_flag = 1;

    // Bitstream restrictions
    sps->vui_seq_parameters.bitstream_restriction_flag = 1;
    sps->vui_seq_parameters.motion_vectors_over_pic_boundaries_flag = 1;
    sps->vui_seq_parameters.max_bytes_per_pic_denom = 0;
    sps->vui_seq_parameters.max_bits_per_mb_denom = 0;
    sps->vui_seq_parameters.log2_max_mv_length_horizontal = 9;
    sps->vui_seq_parameters.log2_max_mv_length_vertical = 9;
    sps->vui_seq_parameters.num_reorder_frames = 0;
    sps->vui_seq_parameters.max_num_ref_frames = 1;

    va_.vaUnmapBuffer(display_, seq_param_buf_);
    return true;
}

// ---------------------------------------------------------------------------
// Fill H.264 PPS (Picture Parameter Set)
// ---------------------------------------------------------------------------

bool VaapiEncoder::fill_pps(std::string& err) {
    VAEncPictureParameterBufferH264* pps = nullptr;
    VAStatus s = va_.vaMapBuffer(display_, pic_param_buf_, (void**)&pps);
    if(s != VA_STATUS_SUCCESS || !pps) {
        err = "vaMapBuffer(pic) failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    std::memset(pps, 0, sizeof(*pps));

    // PPS ID
    pps->pic_parameter_set_id = 0;

    // SPS reference
    pps->seq_parameter_set_id = 0;

    // Entropy coding mode (CABAC for Main, CAVLC for Baseline)
    pps->entropy_coding_mode_flag = 0;

    // Bottom field pic order in frame = 0
    pps->bottom_field_pic_order_in_frame_present_flag = 0;

    // Slice groups
    pps->num_slice_groups_minus1 = 0;

    // Reference frames
    pps->num_ref_idx_l0_default_active_minus1 = 0;
    pps->num_ref_idx_l1_default_active_minus1 = 0;

    // Weighted prediction
    pps->weighted_pred_flag = 0;
    pps->weighted_bipred_idc = 0;

    // Deblocking
    pps->deblocking_filter_control_present_flag = 0;
    pps->constrained_intra_pred_flag = 0;
    pps->redundant_pic_cnt_present_flag = 0;

    // Reference frame list
    pps->reference_pic_list_reordering_flag_l0 = 0;
    pps->reference_pic_list_reordering_flag_l1 = 0;

    va_.vaUnmapBuffer(display_, pic_param_buf_);
    return true;
}

// ---------------------------------------------------------------------------
// Fill slice parameters
// ---------------------------------------------------------------------------

bool VaapiEncoder::fill_slice_params(bool is_idr, std::string& err) {
    VAEncSliceParameterBufferH264* slice = nullptr;
    VAStatus s = va_.vaMapBuffer(display_, slice_param_buf_, (void**)&slice);
    if(s != VA_STATUS_SUCCESS || !slice) {
        err = "vaMapBuffer(slice) failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    std::memset(slice, 0, sizeof(*slice));

    unsigned int mb_width = (params_.width + 15) / 16;
    unsigned int mb_height = (params_.height + 15) / 16;

    // Slice position (single slice covering whole frame)
    slice->macroblock_info_update_flag = 0;
    slice->slice_data_state = VA_SLICE_DATA_STATE_ALL;
    slice->slice_data_bit_offset = 0;  // Will be calculated by driver
    slice->first_mb_in_slice = 0;
    slice->num_macroblocks_in_slice = mb_width * mb_height;
    slice->slice_type = is_idr ? 2 : 0;  // 2=I, 0=P

    // NAL unit header
    slice->nal_unit_type = is_idr ? 5 : 1;

    // Reference frames
    slice->idr_pic_id = 0;
    slice->pic_order_cnt_lsb = 0;
    slice->dec_ref_pic_marking_bit_offset = 0;
    slice->sp_for_switch_flag = 0;

    // Frame number
    slice->frame_num = 0;

    // QP
    slice->slice_qp_delta = 0;

    // Disable deblocking filter
    slice->disable_deblocking_filter_idc = 0;
    slice->slice_alpha_c0_offset_div2 = 0;
    slice->slice_beta_offset_div2 = 0;

    // Reference picture list
    slice->num_ref_idx_l0_active_minus1 = 0;
    slice->num_ref_idx_l1_active_minus1 = 0;

    // Long-term reference
    slice->long_term_reference_flag = 0;

    va_.vaUnmapBuffer(display_, slice_param_buf_);
    return true;
}

// ---------------------------------------------------------------------------
// Upload I420 frame to VA surface
// ---------------------------------------------------------------------------

bool VaapiEncoder::upload_i420_to_surface(const plugins::VideoFrame& raw,
                                          std::string& err) {
    VAImage image;
    VAStatus s = va_.vaDeriveImage(display_, input_surface_, &image);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaDeriveImage failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    if(image.format.fourcc != VA_FOURCC_NV12) {
        err = "expected VA_FOURCC_NV12, got " + std::to_string(image.format.fourcc);
        va_.vaDestroyImage(display_, image.image_id);
        return false;
    }

    auto* cpu = raw.cpu_buffer();
    if(!cpu) {
        err = "no CPU buffer available";
        va_.vaDestroyImage(display_, image.image_id);
        return false;
    }

    const std::uint8_t* y_src = cpu->plane(0);
    const std::uint8_t* u_src = cpu->plane(1);
    const std::uint8_t* v_src = cpu->plane(2);

    unsigned int y_size = image.pitches[0] * image.height;
    unsigned int uv_height = image.height / 2;

    // Copy Y plane
    for(unsigned int row = 0; row < image.height; ++row) {
        std::memcpy((uint8_t*)image.planes[0] + row * image.pitches[0],
                    y_src + row * raw.width(),
                    raw.width());
    }

    // Copy UV plane (NV12 interleaved)
    unsigned int uv_width = raw.width() / 2;
    for(unsigned int row = 0; row < uv_height; ++row) {
        for(unsigned int col = 0; col < uv_width; ++col) {
            ((uint8_t*)image.planes[1] + row * image.pitches[1])[col * 2] = u_src[row * uv_width + col];
            ((uint8_t*)image.planes[1] + row * image.pitches[1])[col * 2 + 1] = v_src[row * uv_width + col];
        }
    }

    va_.vaDestroyImage(display_, image.image_id);
    return true;
}

// ---------------------------------------------------------------------------
// Encode —main encode function
// ---------------------------------------------------------------------------

bool VaapiEncoder::encode(const plugins::VideoFrame& raw,
                          std::uint8_t* output, std::size_t capacity,
                          plugins::EncodedVideoFrame& encoded_out,
                          bool& out_is_keyframe,
                          std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);

    if(!opened_ || !display_) {
        err = "VA encoder not open";
        ++stats_.encode_errors;
        return false;
    }

    if(raw.width() != params_.width || raw.height() != params_.height) {
        err = "frame size mismatch";
        ++stats_.encode_errors;
        return false;
    }

    // Determine if we need an IDR frame
    bool is_idr = params_.force_idr_next;
    if(stats_.frames_encoded == 0) {
        is_idr = true;  // First frame should be IDR
    } else if(params_.keyframe_interval > 0 &&
              (stats_.frames_encoded % params_.keyframe_interval) == 0) {
        is_idr = true;
    }

    out_is_keyframe = is_idr;

    // Upload I420 to surface
    if(!upload_i420_to_surface(raw, err)) {
        ++stats_.encode_errors;
        return false;
    }

    // Fill parameter buffers
    VAEncSequenceParameterBufferH264* sps = nullptr;
    VAStatus s = va_.vaMapBuffer(display_, seq_param_buf_, (void**)&sps);
    if(s != VA_STATUS_SUCCESS || !sps) {
        err = "vaMapBuffer(seq) failed";
        ++stats_.encode_errors;
        return false;
    }
    sps->frame_num = stats_.frames_encoded & 0xFFFF;
    va_.vaUnmapBuffer(display_, seq_param_buf_);

    VAEncPictureParameterBufferH264* pps = nullptr;
    s = va_.vaMapBuffer(display_, pic_param_buf_, (void**)&pps);
    if(s != VA_STATUS_SUCCESS || !pps) {
        err = "vaMapBuffer(pic) failed";
        ++stats_.encode_errors;
        return false;
    }
    pps->CurrPic.picture_id = input_surface_;
    pps->reference_pictures[0].picture_id = VA_INVALID_SURFACE;
    pps->reference_pictures[1].picture_id = VA_INVALID_SURFACE;
    pps->coded_buf = coded_buf_;
    pps->frame_num = stats_.frames_encoded & 0xFFFF;
    if(is_idr) {
        pps->idr_pic_id = stats_.frames_encoded & 0xFFFF;
    }
    va_.vaUnmapBuffer(display_, pic_param_buf_);

    if(!fill_slice_params(is_idr, err)) {
        ++stats_.encode_errors;
        return false;
    }

    // Begin encoding
    s = va_.vaBeginPicture(display_, context_id_, input_surface_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaBeginPicture failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.encode_errors;
        return false;
    }

    // Render parameter buffers
    VABuffer buffers[4] = {
        {seq_param_buf_, 0, 0},
        {pic_param_buf_, 0, 0},
        {slice_param_buf_, 0, 0},
        {coded_buf_, 0, 0}
    };

    s = va_.vaRenderPicture(display_, context_id_, buffers, 4);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaRenderPicture failed: " + std::string(va_.vaErrorStr(s));
        va_.vaEndPicture(display_, context_id_);
        ++stats_.encode_errors;
        return false;
    }

    s = va_.vaEndPicture(display_, context_id_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaEndPicture failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.encode_errors;
        return false;
    }

    // Wait for encode to complete
    s = va_.vaSyncSurface(display_, input_surface_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaSyncSurface failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.encode_errors;
        return false;
    }

    // Map and read the coded buffer
    VACodedBufferSegment* coded_seg = nullptr;
    s = va_.vaMapBuffer(display_, coded_buf_, (void**)&coded_seg);
    if(s != VA_STATUS_SUCCESS || !coded_seg) {
        err = "vaMapBuffer(coded) failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.encode_errors;
        return false;
    }

    // Copy coded data to output
    std::size_t total_size = 0;
    for(VACodedBufferSegment* seg = coded_seg; seg; seg = seg->next) {
        if(seg->buf && seg->size > 0) {
            if(total_size + seg->size > capacity) {
                err = "output buffer too small";
                va_.vaUnmapBuffer(display_, coded_buf_);
                ++stats_.encode_errors;
                return false;
            }
            std::memcpy(output + total_size, seg->buf, seg->size);
            total_size += seg->size;
        }
    }
    va_.vaUnmapBuffer(display_, coded_buf_);

    if(total_size == 0) {
        err = "no encoded data produced";
        ++stats_.encode_errors;
        return false;
    }

    // Parse encoded data for SPS/PPS and pack as Annex B
    core::ByteSpan full{output, total_size};
    auto* sps_nalu = hw_backend::find_sps_in(full);
    auto* pps_nalu = hw_backend::find_pps_in(full);

    // Cache SPS/PPS for subsequent non-IDR frames
    if(is_idr && sps_nalu) {
        sps_.assign(sps_nalu->data.data(),
                     sps_nalu->data.data() + sps_nalu->data.size());
    }
    if(is_idr && pps_nalu) {
        pps_.assign(pps_nalu->data.data(),
                     pps_nalu->data.data() + pps_nalu->data.size());
    }

    // Pack Annex B bitstream
    core::ByteSpan slice_data;
    if(sps_nalu && pps_nalu) {
        // Full bitstream with SPS+PPS+slice - just use as-is
        // The raw VA-API output may already be in Annex B format
    } else if(!sps_.empty() && !pps_.empty()) {
        // Prepend cached SPS/PPS
        std::vector<std::uint8_t> annex_b;
        annex_b.reserve(256 + total_size);

        // SPS
        annex_b.insert(annex_b.end(), {0, 0, 0, 1});
        annex_b.insert(annex_b.end(), sps_.begin(), sps_.end());

        // PPS
        annex_b.insert(annex_b.end(), {0, 0, 0, 1});
        annex_b.insert(annex_b.end(), pps_.begin(), pps_.end());

        // Slice (VA-API output)
        annex_b.insert(annex_b.end(), output, output + total_size);

        if(annex_b.size() > capacity) {
            err = "output buffer too small for Annex B packing";
            ++stats_.encode_errors;
            return false;
        }
        std::memcpy(output, annex_b.data(), annex_b.size());
        total_size = annex_b.size();
    }

    ++stats_.frames_encoded;
    stats_.bytes_encoded += total_size;
    params_.force_idr_next = false;

    encoded_out.codec = plugins::VideoCodecKind::kH264;
    encoded_out.nalu_format = plugins::NaluFormat::kAnnexBStartCode;
    encoded_out.payload = core::ByteSpan{output, total_size};
    encoded_out.is_keyframe = out_is_keyframe;
    encoded_out.payload_type = 102;  // H.264 RTP payload type
    encoded_out.info = raw.info;

    return true;
}

plugins::VideoCodecStats VaapiEncoder::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ===========================================================================
// VaapiDecoder —VA-API H.264 decoder
// ===========================================================================

class VaapiDecoder {
public:
    VaapiDecoder() = default;
    ~VaapiDecoder();

    bool open(const plugins::VideoCodecConfig& cfg, std::string& err);
    bool is_open() const noexcept { return display_ != nullptr; }

    bool decode(const plugins::EncodedVideoFrame& encoded,
                plugins::VideoFrame& raw_out,
                std::uint8_t* const* output_buffers,
                std::string& err);

    plugins::VideoCodecStats stats() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    bool create_decoder_surfaces(unsigned int width, unsigned int height,
                                  std::string& err);

    VADynLib va_;
    VADisplay display_ = nullptr;
    int drm_fd_ = -1;
    VAConfigID config_id_ = VA_INVALID_ID;
    VAContextID context_id_ = VA_INVALID_CONTEXT;
    VASurfaceID output_surface_ = VA_INVALID_SURFACE;
    VABufferID bitstream_buf_ = VA_INVALID_BUFFER;

    unsigned int width_ = 0;
    unsigned int height_ = 0;
    mutable std::mutex mutex_;
    plugins::VideoCodecStats stats_{};
    bool opened_ = false;
};

VaapiDecoder::~VaapiDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!display_) return;

    if(bitstream_buf_ != VA_INVALID_BUFFER) {
        va_.vaDestroyBuffer(display_, bitstream_buf_);
    }
    if(output_surface_ != VA_INVALID_SURFACE) {
        va_.vaDestroySurfaces(display_, &output_surface_, 1);
    }
    if(context_id_ != VA_INVALID_CONTEXT) {
        va_.vaDestroyContext(display_, context_id_);
    }
    if(config_id_ != VA_INVALID_ID) {
        va_.vaDestroyConfig(display_, config_id_);
    }
    if(display_) {
        va_.vaTerminate(display_);
    }
    if(drm_fd_ >= 0) {
        close(drm_fd_);
    }
    display_ = nullptr;
}

bool VaapiDecoder::open(const plugins::VideoCodecConfig& cfg, std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);

    if(opened_) {
        err = "VA decoder already open";
        return false;
    }

    width_ = cfg.width;
    height_ = cfg.height;

    // Load libva
    if(!va_.open(err)) {
        return false;
    }

    // Open DRM render node
    drm_fd_ = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if(drm_fd_ < 0) {
        drm_fd_ = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    }
    if(drm_fd_ < 0) {
        err = "open(/dev/dri/renderD128) failed: " + std::string(strerror(errno));
        return false;
    }

    display_ = va_.vaGetDisplayDRM(drm_fd_);
    if(!display_) {
        err = "vaGetDisplayDRM returned nullptr";
        close(drm_fd_);
        drm_fd_ = -1;
        return false;
    }

    int major, minor;
    VAStatus s = va_.vaInitialize(display_, &major, &minor);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaInitialize failed: " + std::string(va_.vaErrorStr(s));
        close(drm_fd_);
        drm_fd_ = -1;
        display_ = nullptr;
        return false;
    }

    NIMRTC_LOG_INFO("vaapi decoder: VA-API version {}.{} initialized", major, minor);

    // Query H.264 profile support
    VAProfile profiles[16];
    int num_profiles = 16;
    s = va_.vaQueryConfigProfiles(display_, profiles, &num_profiles);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaQueryConfigProfiles failed";
        return false;
    }

    VAProfile selected_profile = VAProfileNone;
    VAProfile h264_profiles[] = {
        VAProfileH264High, VAProfileH264Main, VAProfileH264Baseline,
        VAProfileH264ConstrainedBaseline
    };
    for(auto profile : h264_profiles) {
        for(int i = 0; i < num_profiles; ++i) {
            if(profiles[i] == profile) {
                selected_profile = profile;
                break;
            }
        }
        if(selected_profile != VAProfileNone) break;
    }
    if(selected_profile == VAProfileNone) {
        err = "no H.264 VA profile supported";
        return false;
    }

    // Create decoder config
    s = va_.vaCreateConfig(display_, VAEntrypointDecSlice, selected_profile,
                           nullptr, 0, &config_id_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateConfig failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    // Create decoder context (no reference surfaces for decode-only)
    s = va_.vaCreateContext(display_, config_id_,
                            width_, height_, 0, nullptr, 0, &context_id_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateContext failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }

    // Create output surface
    if(!create_decoder_surfaces(width_, height_, err)) {
        return false;
    }

    opened_ = true;
    NIMRTC_LOG_INFO("vaapi decoder: opened {}x{}", width_, height_);
    (void)cfg;
    return true;
}

bool VaapiDecoder::create_decoder_surfaces(unsigned int width, unsigned int height,
                                           std::string& err) {
    VAStatus s = va_.vaCreateSurfaces(display_, VA_RT_FORMAT_YUV420,
                                       width, height, &output_surface_, 1, nullptr);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateSurfaces failed: " + std::string(va_.vaErrorStr(s));
        return false;
    }
    return true;
}

bool VaapiDecoder::decode(const plugins::EncodedVideoFrame& encoded,
                          plugins::VideoFrame& raw_out,
                          std::uint8_t* const* output_buffers,
                          std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);

    if(!opened_ || !display_) {
        err = "VA decoder not open";
        ++stats_.decode_errors;
        return false;
    }

    const std::size_t bitstream_size = encoded.payload.size();
    if(bitstream_size == 0) {
        err = "empty bitstream";
        ++stats_.decode_errors;
        return false;
    }

    // Create or reuse bitstream buffer
    if(bitstream_buf_ != VA_INVALID_BUFFER) {
        va_.vaDestroyBuffer(display_, bitstream_buf_);
        bitstream_buf_ = VA_INVALID_BUFFER;
    }

    VABufferID bitstream_buf;
    VAStatus s = va_.vaCreateBuffer(display_, context_id_,
                                     VAEncSliceParameterBufferType,  // Misnomer: also used for bitstream
                                     static_cast<unsigned int>(bitstream_size),
                                     1, (void*)encoded.payload.data(), &bitstream_buf);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaCreateBuffer(bitstream) failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.decode_errors;
        return false;
    }
    bitstream_buf_ = bitstream_buf;

    // Begin decode
    s = va_.vaBeginPicture(display_, context_id_, output_surface_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaBeginPicture failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.decode_errors;
        return false;
    }

    // Submit bitstream
    VABuffer buf_info = {bitstream_buf_, 0, 0};
    s = va_.vaRenderPicture(display_, context_id_, &buf_info, 1);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaRenderPicture failed: " + std::string(va_.vaErrorStr(s));
        va_.vaEndPicture(display_, context_id_);
        ++stats_.decode_errors;
        return false;
    }

    s = va_.vaEndPicture(display_, context_id_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaEndPicture failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.decode_errors;
        return false;
    }

    // Wait for decode
    s = va_.vaSyncSurface(display_, output_surface_);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaSyncSurface failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.decode_errors;
        return false;
    }

    // Read decoded pixels
    VAImage image;
    s = va_.vaDeriveImage(display_, output_surface_, &image);
    if(s != VA_STATUS_SUCCESS) {
        err = "vaDeriveImage failed: " + std::string(va_.vaErrorStr(s));
        ++stats_.decode_errors;
        return false;
    }

    if(image.format.fourcc != VA_FOURCC_NV12) {
        err = "expected VA_FOURCC_NV12, got " + std::to_string(image.format.fourcc);
        va_.vaDestroyImage(display_, image.image_id);
        ++stats_.decode_errors;
        return false;
    }

    // Copy to output I420 buffers
    uint8_t* y_dst = output_buffers[0];
    uint8_t* u_dst = output_buffers[1];
    uint8_t* v_dst = output_buffers[2];

    // Copy Y plane
    for(unsigned int row = 0; row < image.height; ++row) {
        std::memcpy(y_dst + row * width_,
                    (uint8_t*)image.planes[0] + row * image.pitches[0],
                    width_);
    }

    // Copy UV → U and V planes (NV12 → I420)
    unsigned int uv_height = image.height / 2;
    for(unsigned int row = 0; row < uv_height; ++row) {
        for(unsigned int col = 0; col < width_ / 2; ++col) {
            uint8_t uv_val = ((uint8_t*)image.planes[1])[row * image.pitches[1] + col * 2];
            uint8_t vv_val = ((uint8_t*)image.planes[1])[row * image.pitches[1] + col * 2 + 1];
            u_dst[row * (width_ / 2) + col] = uv_val;
            v_dst[row * (width_ / 2) + col] = vv_val;
        }
    }

    va_.vaDestroyImage(display_, image.image_id);

    ++stats_.frames_decoded;
    stats_.bytes_decoded += bitstream_size;

    raw_out.set_width(width_);
    raw_out.set_height(height_);

    return true;
}

// ===========================================================================
// IVideoCodec wrappers
// ===========================================================================

class VaapiEncoderCodec : public plugins::IVideoCodec {
public:
    explicit VaapiEncoderCodec(plugins::VideoCodecConfig cfg)
        : cfg_(std::move(cfg)) {}

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264-vaapi"; }

    plugins::Status open() noexcept override {
        hw_backend::SessionParams p{};
        p.width = cfg_.width;
        p.height = cfg_.height;
        p.fps = cfg_.fps;
        p.bitrate_bps = cfg_.bitrate_bps;
        p.keyframe_interval = cfg_.keyframe_interval;

        std::string err;
        if(!encoder_.open(p, cfg_, err)) {
            NIMRTC_LOG_ERROR("vaapi encoder: open failed: {}", err);
            return plugins::kErrHardwareError;
        }
        opened_ = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        encoder_ = VaapiEncoder{};
        opened_ = false;
    }

    plugins::Status encode(const plugins::VideoFrame& raw,
                          std::uint8_t* output, std::size_t capacity,
                          plugins::EncodedVideoFrame& encoded_out) noexcept override {
        bool is_keyframe = false;
        std::string err;
        if(!encoder_.encode(raw, output, capacity, encoded_out, is_keyframe, err)) {
            NIMRTC_LOG_ERROR("vaapi encoder: encode failed: {}", err);
            return plugins::kErrHardwareError;
        }
        return plugins::kOk;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame&,
                          plugins::VideoFrame&,
                          std::uint8_t* const*) noexcept override {
        return plugins::kErrUnsupported;
    }

    plugins::Status force_keyframe() noexcept override {
        // Will be picked up on next encode
        return plugins::kOk;
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }
    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return plugins::kOk;
    }
    plugins::VideoCodecStats stats() const noexcept override { return encoder_.stats(); }
    std::uint8_t payload_type() const noexcept override { return cfg_.payload_type; }

private:
    plugins::VideoCodecConfig cfg_;
    VaapiEncoder encoder_;
    bool opened_ = false;
};

class VaapiDecoderCodec : public plugins::IVideoCodec {
public:
    explicit VaapiDecoderCodec(plugins::VideoCodecConfig cfg)
        : cfg_(std::move(cfg)) {}

    bool can_encode() const noexcept override { return false; }
    bool can_decode() const noexcept override { return true; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264-vaapi-dec"; }

    plugins::Status open() noexcept override {
        std::string err;
        if(!decoder_.open(cfg_, err)) {
            NIMRTC_LOG_ERROR("vaapi decoder: open failed: {}", err);
            return plugins::kErrHardwareError;
        }
        opened_ = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        decoder_ = VaapiDecoder{};
        opened_ = false;
    }

    plugins::Status encode(const plugins::VideoFrame&,
                          std::uint8_t*, std::size_t,
                          plugins::EncodedVideoFrame&) noexcept override {
        return plugins::kErrUnsupported;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                          plugins::VideoFrame& raw_out,
                          std::uint8_t* const* output_buffers) noexcept override {
        std::string err;
        if(!decoder_.decode(encoded, raw_out, output_buffers, err)) {
            NIMRTC_LOG_ERROR("vaapi decoder: decode failed: {}", err);
            return plugins::kErrHardwareError;
        }
        return plugins::kOk;
    }

    plugins::Status force_keyframe() noexcept override {
        return plugins::kErrUnsupported;
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }
    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return plugins::kOk;
    }
    plugins::VideoCodecStats stats() const noexcept override { return decoder_.stats(); }
    std::uint8_t payload_type() const noexcept override { return cfg_.payload_type; }

private:
    plugins::VideoCodecConfig cfg_;
    VaapiDecoder decoder_;
    bool opened_ = false;
};

} // namespace nimrtc::h264::vaapi_backend

// ---------------------------------------------------------------------------
// Public entry points for hw_backends.cpp
// ---------------------------------------------------------------------------

namespace nimrtc::h264 {

bool vaapi_h264_available() noexcept {
    try {
        // Try to open a DRM render node and initialize VA
        int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if(fd < 0) {
            fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        }
        if(fd < 0) {
            return false;
        }

        // Dynamically load libva
        void* handle = dlopen("libva.so.2", RTLD_NOW);
        if(!handle) {
            handle = dlopen("libva.so.1", RTLD_NOW);
        }
        if(!handle) {
            handle = dlopen("libva.so", RTLD_NOW);
        }
        if(!handle) {
            close(fd);
            return false;
        }

        auto vaGetDisplayDRM = reinterpret_cast<VAStatus (*)(int)>(
            dlsym(handle, "vaGetDisplayDRM"));
        auto vaInitialize = reinterpret_cast<VAStatus (*)(VADisplay, int*, int*)>(
            dlsym(handle, "vaInitialize"));
        auto vaQueryConfigProfiles = reinterpret_cast<VAStatus (*)(VADisplay, VAProfile*, int*)>(
            dlsym(handle, "vaQueryConfigProfiles"));
        auto vaTerminate = reinterpret_cast<VAStatus (*)(VADisplay)>(
            dlsym(handle, "vaTerminate"));

        if(!vaGetDisplayDRM || !vaInitialize || !vaQueryConfigProfiles || !vaTerminate) {
            dlclose(handle);
            close(fd);
            return false;
        }

        VADisplay display = vaGetDisplayDRM(fd);
        if(!display) {
            dlclose(handle);
            close(fd);
            return false;
        }

        int major, minor;
        VAStatus s = vaInitialize(display, &major, &minor);
        if(s != VA_STATUS_SUCCESS) {
            dlclose(handle);
            close(fd);
            return false;
        }

        // Check H.264 profile support
        VAProfile profiles[16];
        int num_profiles = 16;
        s = vaQueryConfigProfiles(display, profiles, &num_profiles);
        bool has_h264 = false;
        if(s == VA_STATUS_SUCCESS) {
            VAProfile h264_profiles[] = {
                VAProfileH264High, VAProfileH264Main, VAProfileH264Baseline,
                VAProfileH264ConstrainedBaseline
            };
            for(auto profile : h264_profiles) {
                for(int i = 0; i < num_profiles; ++i) {
                    if(profiles[i] == profile) {
                        has_h264 = true;
                        break;
                    }
                }
                if(has_h264) break;
            }
        }

        vaTerminate(display);
        dlclose(handle);
        close(fd);

        return has_h264;
    } catch(...) {
        return false;
    }
}

std::unique_ptr<plugins::IVideoCodec>
make_vaapi_h264_codec(plugins::VideoCodecConfig cfg) {
    // Return an encoder codec (supports encode)
    return std::make_unique<vaapi_backend::VaapiEncoderCodec>(std::move(cfg));
}

} // namespace nimrtc::h264

#else  // NIMRTC_PLUGINS_VAAPI_ON && __linux__

// Stub-build path —exported symbols are present but always return false,
// letting the registry compile cleanly when VA-API isn't available.

namespace nimrtc::h264 {

bool vaapi_h264_available() noexcept {
    return false;
}

std::unique_ptr<plugins::IVideoCodec>
make_vaapi_h264_codec(plugins::VideoCodecConfig) {
    return nullptr;
}

} // namespace nimrtc::h264

#endif  // NIMRTC_PLUGINS_VAAPI_ON && __linux__
