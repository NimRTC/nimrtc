// dxva_decoder.cpp - Microsoft DXVA H.264 decoder plugin for NimRTC
// Uses Media Foundation H.264 MFT which internally leverages DXVA hardware acceleration

#include <nimrtc/h264/hw_backends.hpp>

#if defined(NIMRTC_PLUGINS_DXVA_ON) && defined(_WIN32)

#include <d3d11.h>
#include <dxva2api.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <evr.h>
#include <wrl/client.h>  // ComPtr

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#include "hw_backend_base.hpp"

using Microsoft::WRL::ComPtr;

namespace nimrtc::h264::dxva_backend {

namespace detail {

// NV12 to I420 conversion utility
// NV12: Y plane (w*h) followed by UV interleaved plane (w*h/2)
// I420: Y plane (w*h) followed by U plane (w*h/4) and V plane (w*h/4)
static void ConvertNv12ToI420(
    const uint8_t* nv12,
    int width,
    int height,
    uint8_t* i420) noexcept
{
    if (!nv12 || !i420 || width <= 0 || height <= 0) {
        return;
    }

    const int ySize = width * height;
    const int uvSize = ySize / 4;

    // Copy Y plane
    std::memcpy(i420, nv12, static_cast<size_t>(ySize));

    // Pointers for UV plane in NV12 and U/V planes in I420
    const uint8_t* nv12Uv = nv12 + ySize;
    uint8_t* i420U = i420 + ySize;
    uint8_t* i420V = i420 + ySize + uvSize;

    // UV plane width is half of Y plane width (subsampled)
    const int uvStride = width;  // NV12 UV plane has same width as Y

    // Convert interleaved UV to planar U and V
    for (int row = 0; row < (height + 1) / 2; ++row) {
        for (int col = 0; col < (width + 1) / 2; ++col) {
            // NV12: U and V are interleaved (UVUV...)
            // I420: separate U and V planes
            const size_t uvIndex = static_cast<size_t>(row * uvStride + col * 2);
            i420U[row * ((width + 1) / 2) + col] = nv12Uv[uvIndex];     // U
            i420V[row * ((width + 1) / 2) + col] = nv12Uv[uvIndex + 1]; // V
        }
    }
}

// Convert I420 to I420 (pass-through, for output compatibility)
static void ConvertI420ToI420(
    const uint8_t* i420,
    int width,
    int height,
    uint8_t* out) noexcept
{
    if (!i420 || !out || width <= 0 || height <= 0) {
        return;
    }

    const int ySize = width * height;
    const int uvSize = ySize / 4;

    // Copy Y plane
    std::memcpy(out, i420, static_cast<size_t>(ySize));

    // Copy U plane
    std::memcpy(out + ySize, i420 + ySize, static_cast<size_t>(uvSize));

    // Copy V plane
    std::memcpy(out + ySize + uvSize, i420 + ySize + uvSize, static_cast<size_t>(uvSize));
}

// Helper to convert from DXGI format to I420
static void ConvertDxgiToI420(
    const uint8_t* yPlane,
    const uint8_t* uvPlane,
    int width,
    int height,
    int yPitch,
    int uvPitch,
    uint8_t* i420) noexcept
{
    if (!yPlane || !uvPlane || !i420 || width <= 0 || height <= 0) {
        return;
    }

    const int ySize = width * height;
    const int uvSize = ySize / 4;

    // Copy Y plane with pitch handling
    for (int row = 0; row < height; ++row) {
        std::memcpy(i420 + row * width, yPlane + row * yPitch, static_cast<size_t>(width));
    }

    // Pointers for UV plane in output and U/V planes in I420
    uint8_t* i420U = i420 + ySize;
    uint8_t* i420V = i420 + ySize + uvSize;

    // Convert interleaved UV to planar U and V with pitch handling
    const int chromaHeight = (height + 1) / 2;
    const int chromaWidth = (width + 1) / 2;

    for (int row = 0; row < chromaHeight; ++row) {
        for (int col = 0; col < chromaWidth; ++col) {
            const size_t srcIndex = static_cast<size_t>(row * uvPitch + col * 2);
            i420U[row * chromaWidth + col] = uvPlane[srcIndex];     // U
            i420V[row * chromaWidth + col] = uvPlane[srcIndex + 1]; // V
        }
    }
}

}  // namespace detail

class DxvaDecoder : public plugins::IVideoCodec {
public:
    DxvaDecoder() = delete;
    explicit DxvaDecoder(plugins::VideoCodecConfig config);
    ~DxvaDecoder() override;

    // IVideoCodec interface
    [[nodiscard]] int GetPayloadType() const noexcept override;
    [[nodiscard]] const char* GetPayloadName() const noexcept override;
    [[nodiscard]] plugins::IVideoCodec* GetFallbackCodec() noexcept override;
    [[nodiscard]] bool HasEncoder() const noexcept override;
    [[nodiscard]] plugins::VideoCodecConfig GetConfig() const noexcept override;
    [[nodiscard]] int Open() noexcept override;
    [[nodiscard]] int Close() noexcept override;
    [[nodiscard]] bool IsOpen() const noexcept override;
    [[nodiscard]] int Decode(
        const uint8_t* packet,
        int packetLen,
        plugins::VideoFrameType frameType,
        int64_t timestamp,
        plugins::VideoFrame& frame) noexcept override;
    [[nodiscard]] int Encode(
        const plugins::VideoFrame& frame,
        bool keyframe,
        plugins::EncodedImage& encoded) noexcept override;
    [[nodiscard]] int SetEncoderConfig(const plugins::VideoEncoderConfig& config) noexcept override;
    [[nodiscard]] int RequestKeyFrame() noexcept override;
    [[nodiscard]] plugins::VideoCodecStats GetStats() noexcept override;
    [[nodiscard]] int SetOptions(const plugins::VideoCodecOptions& options) noexcept override;

private:
    // Initialize the Media Foundation H.264 decoder MFT
    int InitializeMfDecoder() noexcept;

    // Process one frame through the MFT and return decoded data
    int DecodeFrame(const uint8_t* data, int dataLen, std::vector<uint8_t>& outYuv) noexcept;

    // Drain any pending output from the MFT
    int DrainOutput(std::vector<uint8_t>& outYuv) noexcept;

    // Convert MF output format to I420
    int ConvertOutputToI420(
        const uint8_t* rawData,
        int rawLen,
        int width,
        int height,
        std::vector<uint8_t>& i420Out) noexcept;

    plugins::VideoCodecConfig cfg_;
    plugins::VideoCodecStats stats_{};
    bool opened_{false};

    // Media Foundation decoder
    ComPtr<IMFTransform> mfDecoder_;

    // D3D11 device for MF DXVA interop (optional, for hardware surfaces)
    ComPtr<ID3D11Device> d3dDevice_;

    // Input media type
    ComPtr<IMFMediaType> inputMediaType_;
    ComPtr<IMFMediaType> outputMediaType_;

    // Frame dimensions
    int width_{0};
    int height_{0};

    // Pending output sample from MFT
    ComPtr<IMFMediaBuffer> pendingOutputBuffer_;
    bool hasPendingOutput_{false};

    // Mutex for thread safety
    std::mutex mutex_;

    // Last decode result for stats
    int lastDecodeResult_{0};
};

// Format GUID to string for logging
static const char* GuidToString(GUID guid) {
    if (guid == MFVideoFormat_H264) return "H264";
    if (guid == MFVideoFormat_H264_ES) return "H264_ES";
    if (guid == MFVideoFormat_NV12) return "NV12";
    if (guid == MFVideoFormat_I420) return "I420";
    if (guid == MFVideoFormat_IYUV) return "IYUV";
    if (guid == MFVideoFormat_YV12) return "YV12";
    if (guid == MFVideoFormat_YUY2) return "YUY2";
    if (guid == MFVideoFormat_RGB32) return "RGB32";
    if (guid == MFVideoFormat_RGB24) return "RGB24";
    return "Unknown";
}

DxvaDecoder::DxvaDecoder(plugins::VideoCodecConfig config)
    : cfg_(config), width_(config.width), height_(config.height) {
    NIMRTC_LOG_INFO("DxvaDecoder: Created with config: %dx%d, PT=%d",
                    width_, height_, config.payloadType);
}

DxvaDecoder::~DxvaDecoder() {
    Close();
}

int DxvaDecoder::InitializeMfDecoder() noexcept {
    HRESULT hr;

    // Step 1: CoCreateInstance the H.264 decoder MFT
    // CLSID_CMSH264DecoderMFT is the Windows system H.264 decoder that uses DXVA internally
    hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&mfDecoder_));

    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to create H.264 MFT, hr=0x%08X", hr);
        return -1;
    }

    NIMRTC_LOG_INFO("DxvaDecoder: Successfully created H.264 MFT");

    // Step 2: Configure input media type (H.264 bitstream)
    hr = MFCreateMediaType(&inputMediaType_);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to create input media type, hr=0x%08X", hr);
        return -1;
    }

    hr = inputMediaType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set input major type, hr=0x%08X", hr);
        return -1;
    }

    hr = inputMediaType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set input subtype, hr=0x%08X", hr);
        return -1;
    }

    // Set frame size
    hr = MFSetAttributeSize(inputMediaType_, MF_MT_FRAME_SIZE, width_, height_);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set frame size, hr=0x%08X", hr);
        return -1;
    }

    // Set frame rate (default to 30fps)
    hr = MFSetAttributeRatio(inputMediaType_, MF_MT_FRAME_RATE, 30, 1);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set frame rate, hr=0x%08X", hr);
        return -1;
    }

    // Set pixel aspect ratio to 1:1
    hr = MFSetAttributeRatio(inputMediaType_, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set pixel aspect ratio, hr=0x%08X", hr);
        return -1;
    }

    // Set interlace mode to progressive
    hr = inputMediaType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set interlace mode, hr=0x%08X", hr);
        return -1;
    }

    hr = mfDecoder_->SetInputType(0, inputMediaType_.Get(), 0);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set input type, hr=0x%08X", hr);
        return -1;
    }

    NIMRTC_LOG_INFO("DxvaDecoder: Input media type configured");

    // Step 3: Configure output media type (NV12 - what DXVA hardware outputs)
    hr = MFCreateMediaType(&outputMediaType_);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to create output media type, hr=0x%08X", hr);
        return -1;
    }

    hr = outputMediaType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set output major type, hr=0x%08X", hr);
        return -1;
    }

    // Use NV12 as output - DXVA hardware decoders output NV12
    hr = outputMediaType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set output subtype to NV12, hr=0x%08X", hr);
        return -1;
    }

    // Set frame size
    hr = MFSetAttributeSize(outputMediaType_, MF_MT_FRAME_SIZE, width_, height_);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set output frame size, hr=0x%08X", hr);
        return -1;
    }

    // Set frame rate
    hr = MFSetAttributeRatio(outputMediaType_, MF_MT_FRAME_RATE, 30, 1);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set output frame rate, hr=0x%08X", hr);
        return -1;
    }

    // Set interlace mode to progressive
    hr = outputMediaType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set output interlace mode, hr=0x%08X", hr);
        return -1;
    }

    hr = mfDecoder_->SetOutputType(0, outputMediaType_.Get(), 0);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set output type, hr=0x%08X", hr);
        return -1;
    }

    NIMRTC_LOG_INFO("DxvaDecoder: Output media type configured as NV12");

    // Step 4: Try to set up D3D11 device for DXVA hardware acceleration
    // This is optional but enables hardware-accelerated decoding
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    hr = D3D11CreateDevice(
        nullptr,                    // Adapter (default)
        D3D_DRIVER_TYPE_HARDWARE,   // Driver type
        nullptr,                    // Software rasterizer
        0,                          // Flags
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &d3dDevice_,
        &featureLevel,
        nullptr                     // Immediate context (not needed)
    );

    if (SUCCEEDED(hr)) {
        NIMRTC_LOG_INFO("DxvaDecoder: D3D11 device created successfully, feature level=0x%08X",
                        featureLevel);

        // Try to set the D3D device on the MFT for hardware acceleration
        // Note: Not all MFTs support this, but it's worth trying
        IUnknown* deviceUnknown = d3dDevice_.Get();
        mfDecoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceUnknown));
    } else {
        NIMRTC_LOG_INFO("DxvaDecoder: D3D11 device creation failed (0x%08X), "
                        "will use software decoding through MF", hr);
    }

    // Step 5: Start the MFT
    hr = mfDecoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to begin streaming, hr=0x%08X", hr);
        return -1;
    }

    hr = mfDecoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to start stream, hr=0x%08X", hr);
        return -1;
    }

    NIMRTC_LOG_INFO("DxvaDecoder: MFT streaming started");

    return 0;
}

int DxvaDecoder::DecodeFrame(const uint8_t* data, int dataLen, std::vector<uint8_t>& outYuv) noexcept {
    if (!data || dataLen <= 0 || !mfDecoder_) {
        return -1;
    }

    HRESULT hr;

    // Create input buffer
    ComPtr<IMFMediaBuffer> inputBuffer;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(dataLen), &inputBuffer);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to create input buffer, hr=0x%08X", hr);
        return -1;
    }

    // Copy data to buffer
    uint8_t* bufferData = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = inputBuffer->Lock(&bufferData, &maxLength, &currentLength);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to lock input buffer, hr=0x%08X", hr);
        return -1;
    }

    std::memcpy(bufferData, data, static_cast<size_t>(dataLen));
    currentLength = static_cast<DWORD>(dataLen);

    hr = inputBuffer->Unlock();
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to unlock input buffer, hr=0x%08X", hr);
        return -1;
    }

    hr = inputBuffer->SetCurrentLength(currentLength);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set buffer length, hr=0x%08X", hr);
        return -1;
    }

    // Create media sample and add buffer
    ComPtr<IMFMediaSample> inputSample;
    hr = MFCreateSample(&inputSample);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to create input sample, hr=0x%08X", hr);
        return -1;
    }

    hr = inputSample->AddBuffer(inputBuffer.Get());
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to add buffer to sample, hr=0x%08X", hr);
        return -1;
    }

    // Set sample time (approximate, MFT will adjust)
    LONGLONG sampleTime = static_cast<LONGLONG>(stats_.frameCount) * 333333;  // ~30fps in 100ns units
    hr = inputSample->SetSampleTime(sampleTime);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to set sample time, hr=0x%08X", hr);
        // Non-fatal, continue
    }

    // Process input
    hr = mfDecoder_->ProcessInput(0, inputSample.Get(), 0);
    if (FAILED(hr)) {
        if (hr == MF_E_NOTACCEPTING) {
            // MFT is not accepting input right now, try to get output first
            NIMRTC_LOG_INFO("DxvaDecoder: MFT not accepting input, draining output first");
            int drainResult = DrainOutput(outYuv);
            if (drainResult == 0 && !outYuv.empty()) {
                return 0;  // Got output
            }
            // Retry input after drain
            hr = mfDecoder_->ProcessInput(0, inputSample.Get(), 0);
            if (FAILED(hr)) {
                NIMRTC_LOG_ERROR("DxvaDecoder: Retry ProcessInput failed, hr=0x%08X", hr);
                return -1;
            }
        } else {
            NIMRTC_LOG_ERROR("DxvaDecoder: ProcessInput failed, hr=0x%08X", hr);
            return -1;
        }
    }

    stats_.frameCount++;

    // Try to get output
    return DrainOutput(outYuv);
}

int DxvaDecoder::DrainOutput(std::vector<uint8_t>& outYuv) noexcept {
    if (!mfDecoder_) {
        return -1;
    }

    HRESULT hr;
    MFT_OUTPUT_DATA_BUFFER outputBuffer{};
    DWORD status = 0;

    // Create output buffer
    const size_t nv12Size = static_cast<size_t>(width_) * height_ * 3 / 2;  // NV12 size
    ComPtr<IMFMediaBuffer> outputBufferCOM;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12Size), &outputBufferCOM);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to create output buffer, hr=0x%08X", hr);
        return -1;
    }

    outputBuffer.pSample = nullptr;
    outputBuffer.pBuffer = outputBufferCOM.Get();
    outputBuffer.pEvents = nullptr;

    hr = mfDecoder_->ProcessOutput(0, 1, &outputBuffer, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        // No output available yet
        return -1;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Output format changed, reconfigure
        NIMRTC_LOG_INFO("DxvaDecoder: Output format changed, need to reconfigure");
        return -1;
    }

    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: ProcessOutput failed, hr=0x%08X", hr);
        return -1;
    }

    // Get output data
    uint8_t* outputData = nullptr;
    DWORD outputLength = 0;
    hr = outputBuffer.pBuffer->Lock(&outputData, nullptr, &outputLength);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to lock output buffer, hr=0x%08X", hr);
        return -1;
    }

    // Convert NV12 to I420
    outYuv.resize(nv12Size);  // Same size for NV12 and I420
    detail::ConvertNv12ToI420(outputData, width_, height_, outYuv.data());

    hr = outputBuffer.pBuffer->Unlock();
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to unlock output buffer, hr=0x%08X", hr);
        return -1;
    }

    stats_.decodeFrames++;

    return 0;
}

int DxvaDecoder::ConvertOutputToI420(
    const uint8_t* rawData,
    int rawLen,
    int imgWidth,
    int imgHeight,
    std::vector<uint8_t>& i420Out) noexcept {
    if (!rawData || rawLen <= 0) {
        return -1;
    }

    // Assume NV12 input (standard DXVA output format)
    const size_t i420Size = static_cast<size_t>(imgWidth) * imgHeight * 3 / 2;
    i420Out.resize(i420Size);

    detail::ConvertNv12ToI420(rawData, imgWidth, imgHeight, i420Out.data());

    return 0;
}

int DxvaDecoder::Open() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    if (opened_) {
        NIMRTC_LOG_WARN("DxvaDecoder: Already opened");
        return 0;
    }

    NIMRTC_LOG_INFO("DxvaDecoder: Opening decoder for %dx%d", width_, height_);

    // Initialize COM for this thread if not already done
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        NIMRTC_LOG_ERROR("DxvaDecoder: CoInitializeEx failed, hr=0x%08X", hr);
        return -1;
    }

    // Initialize MF
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        NIMRTC_LOG_ERROR("DxvaDecoder: MFStartup failed, hr=0x%08X", hr);
        CoUninitialize();
        return -1;
    }

    // Initialize the MFT decoder
    if (InitializeMfDecoder() != 0) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Failed to initialize MFT decoder");
        MFShutdown();
        CoUninitialize();
        return -1;
    }

    opened_ = true;
    NIMRTC_LOG_INFO("DxvaDecoder: Opened successfully");

    return 0;
}

int DxvaDecoder::Close() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!opened_) {
        return 0;
    }

    NIMRTC_LOG_INFO("DxvaDecoder: Closing");

    // Flush the MFT
    if (mfDecoder_) {
        mfDecoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mfDecoder_.Reset();
    }

    // Release media types
    inputMediaType_.Reset();
    outputMediaType_.Reset();

    // Release D3D device
    d3dDevice_.Reset();

    // Shutdown MF
    MFShutdown();

    // Uninitialize COM
    CoUninitialize();

    opened_ = false;
    NIMRTC_LOG_INFO("DxvaDecoder: Closed");

    return 0;
}

bool DxvaDecoder::IsOpen() const noexcept {
    return opened_;
}

int DxvaDecoder::Decode(
    const uint8_t* packet,
    int packetLen,
    plugins::VideoFrameType frameType,
    int64_t timestamp,
    plugins::VideoFrame& frame) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!opened_ || !packet || packetLen <= 0) {
        NIMRTC_LOG_ERROR("DxvaDecoder: Decode called in invalid state");
        return -1;
    }

    stats_.decodeRequests++;

    // The H.264 decoder expects Annex B format (start codes before NAL units)
    // If the input doesn't have start codes, we need to check and possibly add them

    std::vector<uint8_t> decodedYuv;
    const uint8_t* dataToDecode = packet;
    int dataLenToDecode = packetLen;

    // Check if we need to add start codes (0x00 0x00 0x00 0x01 or 0x00 0x00 0x01)
    bool needsStartCodes = false;
    if (packetLen >= 4) {
        // Check for existing start code
        if (!(packet[0] == 0 && packet[1] == 0 && (packet[2] == 0 || (packet[2] == 1))) ||
            (packet[2] == 0 && packet[3] != 1)) {
            needsStartCodes = true;
        }
    } else if (packetLen >= 3) {
        if (!(packet[0] == 0 && packet[1] == 0 && packet[2] == 1)) {
            needsStartCodes = true;
        }
    } else {
        needsStartCodes = true;
    }

    std::vector<uint8_t> prefixedData;
    if (needsStartCodes) {
        // Add Annex B start code (0x00 0x00 0x01)
        prefixedData.reserve(static_cast<size_t>(packetLen) + 4);
        prefixedData.push_back(0);
        prefixedData.push_back(0);
        prefixedData.push_back(1);
        prefixedData.insert(prefixedData.end(), packet, packet + packetLen);
        dataToDecode = prefixedData.data();
        dataLenToDecode = static_cast<int>(prefixedData.size());
    }

    int result = DecodeFrame(dataToDecode, dataLenToDecode, decodedYuv);

    if (result != 0 || decodedYuv.empty()) {
        stats_.decodeErrors++;
        lastDecodeResult_ = result;
        return result;
    }

    // Fill in the output frame
    frame.width = width_;
    frame.height = height_;
    frame.stride = width_;  // I420 Y plane stride
    frame.chromaStride = (width_ + 1) / 2;  // I420 chroma stride
    frame.timestamp = timestamp;
    frame.frameType = frameType;

    // Copy decoded data to frame buffers
    const int ySize = width_ * height_;
    const int uvSize = ySize / 4;

    // Y plane
    if (frame.yPlane && frame.yPlaneSize >= ySize) {
        std::memcpy(frame.yPlane, decodedYuv.data(), static_cast<size_t>(ySize));
    }

    // U plane
    if (frame.uPlane && frame.uPlaneSize >= uvSize) {
        std::memcpy(frame.uPlane, decodedYuv.data() + ySize, static_cast<size_t>(uvSize));
    }

    // V plane
    if (frame.vPlane && frame.vPlaneSize >= uvSize) {
        std::memcpy(frame.vPlane, decodedYuv.data() + ySize + uvSize, static_cast<size_t>(uvSize));
    }

    lastDecodeResult_ = 0;
    return 0;
}

int DxvaDecoder::Encode(
    const plugins::VideoFrame& frame,
    bool keyframe,
    plugins::EncodedImage& encoded) noexcept {
    // This is a decoder-only plugin
    NIMRTC_LOG_ERROR("DxvaDecoder: Encode not supported");
    return -1;
}

int DxvaDecoder::SetEncoderConfig(const plugins::VideoEncoderConfig& config) noexcept {
    (void)config;
    NIMRTC_LOG_ERROR("DxvaDecoder: SetEncoderConfig not supported");
    return -1;
}

int DxvaDecoder::RequestKeyFrame() noexcept {
    // For decoding, we don't need to request keyframes
    // The decoder will decode whatever it receives
    return 0;
}

plugins::VideoCodecStats DxvaDecoder::GetStats() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    plugins::VideoCodecStats s = stats_;
    s.lastDecodeResult = lastDecodeResult_;
    return s;
}

int DxvaDecoder::SetOptions(const plugins::VideoCodecOptions& options) noexcept {
    (void)options;
    // Options are already set in the config during construction
    return 0;
}

int DxvaDecoder::GetPayloadType() const noexcept {
    return cfg_.payloadType;
}

const char* DxvaDecoder::GetPayloadName() const noexcept {
    return "H264-DXVA";
}

plugins::IVideoCodec* DxvaDecoder::GetFallbackCodec() noexcept {
    // No fallback codec in this plugin
    return nullptr;
}

bool DxvaDecoder::HasEncoder() const noexcept {
    return false;
}

plugins::VideoCodecConfig DxvaDecoder::GetConfig() const noexcept {
    return cfg_;
}

}  // namespace nimrtc::h264::dxva_backend

namespace nimrtc::h264 {

bool dxva_h264_available() noexcept {
    HRESULT hr;

    // Ensure COM is initialized
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        NIMRTC_LOG_INFO("dxva_h264_available: COM initialization failed");
        return false;
    }

    // Try to create the H.264 MFT
    Microsoft::WRL::ComPtr<IMFTransform> testDecoder;
    hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&testDecoder));

    if (FAILED(hr)) {
        NIMRTC_LOG_INFO("dxva_h264_available: H.264 MFT not available, hr=0x%08X", hr);
        CoUninitialize();
        return false;
    }

    NIMRTC_LOG_INFO("dxva_h264_available: H.264 MFT is available");
    testDecoder.Reset();

    // Don't uninitialize COM here - let the caller use it
    // CoUninitialize();

    return true;
}

std::unique_ptr<plugins::IVideoCodec> make_dxva_h264_codec(VideoCodecConfig config) {
    auto codec = std::make_unique<dxva_backend::DxvaDecoder>(config);
    if (codec->Open() != 0) {
        NIMRTC_LOG_ERROR("make_dxva_h264_codec: Failed to open DXVA H.264 codec");
        return nullptr;
    }

    NIMRTC_LOG_INFO("make_dxva_h264_codec: Successfully created DXVA H.264 codec");
    return codec;
}

}  // namespace nimrtc::h264

#else  // NIMRTC_PLUGINS_DXVA_ON && _WIN32

namespace nimrtc::h264 {

bool dxva_h264_available() noexcept {
    return false;
}

std::unique_ptr<plugins::IVideoCodec> make_dxva_h264_codec(VideoCodecConfig) {
    return nullptr;
}

}  // namespace nimrtc::h264

#endif  // NIMRTC_PLUGINS_DXVA_ON && _WIN32
