# HW Plugin Seam — Hardware Acceleration Integration Guide

> 面向 NimRTC R3-Batch 出 tree HW 插件作者的实操指南。阅读本文前请先熟悉
> `ARCHITECTURE.md §2.3`（设备插件化）和 `ARCHITECTURE.md §6`（视频管线）。
> 本文所有示例均对应 `src/plugins/include/nimrtc/plugins/hw_seam.hpp` 与
> `src/plugins/include/nimrtc/plugins/video_pipeline.hpp` 中的当前签名。

---

## 1. Overview

NimRTC 把"视频编解码 / 捕获 / 渲染"建模为四个无 HW 偏好的参考接口
`IVideoSource` / `IVideoSink` / `IVideoReceiver` / `IVideoSender`，它们只描述
数据流，不绑定任何具体的厂商 SDK。R3-Batch 在 `hw_seam.hpp` 中引入了一套
"HW 插件缝合层（seam）"，让出 tree 的 HW 实现（NDK MediaCodec、VideoToolbox、
NVDEC、NVENC、VAAPI、DXVA、QSV、AMF、V4L2、AVFoundation、DirectShow、Android
Camera2 等）能在不污染核心头文件的前提下接入。四个 `IVideo*` 接口已经各自
新增 capability flag——`is_hardware_accelerated()` 与 `hardware_backend()`——
加上三个抽象基类 `IHwVideoEncoder` / `IHwVideoDecoder` / `IHwVideoCapture`
作为多继承锚点，以及一个类型安全的 helper `is_hardware_accelerated<>`。本文
给出从基类实现到注册、配置、能力查询、零拷贝 surface 合约和后端选择策略
的完整路径，并提供四份可编译的伪真实样例（MediaCodec encoder、VideoToolbox
decoder、VAAPI encoder、V4L2 capture）让插件作者直接抄改。

---

## 2. Architecture: Capability Flags + Abstract Bases

### 2.1 Capability flags（`is_hardware_accelerated` / `hardware_backend`）

四个参考接口已经分别新增两个 `virtual` 方法（默认实现 = 软件路径）：

```60:80:src/plugins/include/nimrtc/plugins/video_source.hpp
    // ---- HW capability flag (R3-Batch) -----------------------------------
    // Default = false (software). HW plugins (V4L2, AVFoundation, NDK
    // Camera2, DirectShow, ...) override to true.
    virtual bool is_hardware_accelerated() const noexcept { return false; }

    // ---- HW backend name (R3-Batch) --------------------------------------
    // Returns a short identifier (e.g. "v4l2", "avfoundation", "android-camera2",
    // "directshow", "mediacodec", "software"). Used for diagnostics +
    // capabilities introspection. Default = "software".
    virtual std::string_view hardware_backend() const noexcept {
        return "software";
    }
};
```

设计原则：

- **两接口对齐**：返回值与 `hw_seam.hpp::hw_backend_name(HwBackend)` 一一对应
  （字符串视角一致），调用方可以直接比较 `plugin->hardware_backend() ==
  "mediacodec"`。
- **默认软实现**：`false` / `"software"` 表示纯 CPU 路径，HW 插件必须
  override。
- **轻量**：两个方法都是 `const noexcept`、零分配；可用在热路径（譬如每帧
  统计日志）。
- **不强制依赖 HW 头文件**：仅声明能力字符串，不引入 `ANativeWindow*`、
  `CVPixelBufferRef`、`VASurfaceID` 等平台类型——具体 surface 由 §7
  `IHwVideo*::target_surface_handle()` 暴露。

`IVideoSink` / `IVideoReceiver` / `IVideoSender` 的对应声明完全相同，只是
注释里列举了各自常见的 HW 后端名。

### 2.2 `HwBackend` 枚举 + `hw_backend_name()`

`HwBackend` 是一个稳定的 16-bit 枚举，每个值对应一个 short string id。所有
出 tree 插件都必须把自己的 capability flag 字符串与这里对齐，否则会破坏
`is_hardware_accelerated<>` helper 的判断。

```42:75:src/plugins/include/nimrtc/plugins/hw_seam.hpp
/** Stable identifier for the HW backend a plugin targets.
 *  `Software` is the default; HW plugins MUST return one of the others. */
enum class HwBackend : std::uint16_t {
    Software         = 0,
    MediaCodec       = 1,    // Android NDK MediaCodec
    VideoToolbox     = 2,    // Apple VideoToolbox
    Nvdec            = 3,    // NVIDIA NVDEC
    Nvenc            = 4,    // NVIDIA NVENC
    Vaapi            = 5,    // VA-API (Linux Intel/AMD)
    Dxva             = 6,    // DirectX Video Acceleration (Windows)
    Qsv              = 7,    // Intel Quick Sync Video
    Amf              = 8,    // AMD AMF / VCN
    V4l2             = 9,    // V4L2 M2M (Linux)
    AvFoundation     = 10,   // Apple AVFoundation capture
    DirectShow       = 11,   // Windows DirectShow capture
    AndroidCamera2   = 12,   // Android Camera2 capture
    OpenH264         = 100,  // software but HW-tuned
    Libvpx           = 101,
    Custom           = 0xFFFE,
    Unknown          = 0xFFFF,
};
```

使用样例：

```cpp
#include <nimrtc/plugins/hw_seam.hpp>

// 在诊断日志里打印后端：
auto* sink = engine.video_sink();
NIMRTC_LOG_INFO("video sink backend = {}",
                std::string(sink->hardware_backend()));

// 反向查找枚举值（譬如把外部配置文件里的字符串映射进来）：
auto parse = [](std::string_view s) -> nimrtc::plugins::HwBackend {
    using nimrtc::plugins::HwBackend;
    if (s == "mediacodec")  return HwBackend::MediaCodec;
    if (s == "videotoolbox")return HwBackend::VideoToolbox;
    if (s == "vaapi")       return HwBackend::Vaapi;
    return HwBackend::Unknown;
};
```

### 2.3 Abstract base interfaces（`IHwVideoEncoder/Decoder/Capture`）

`hw_seam.hpp` 提供三个"partner"基类，HW 插件通过**多继承**把它们和
对应的 `IVideo*` 接口一起实现。这样既保留了通用注册路径
（`IVideoSenderFactory::create()`），又暴露了 HW 特有的查询入口。

```cpp
// 来自 src/plugins/include/nimrtc/plugins/hw_seam.hpp
class IHwVideoEncoder {
public:
    virtual ~IHwVideoEncoder() = default;
    virtual HwBackend backend() const noexcept = 0;
    std::string_view backend_name() const noexcept {
        return hw_backend_name(backend());
    }
    virtual void* target_surface_handle() noexcept = 0;
    virtual bool supports(VideoCodecKind codec,
                          std::uint32_t width,
                          std::uint32_t height,
                          std::uint32_t fps) const noexcept = 0;
    virtual std::uint32_t encoding_latency_us() const noexcept = 0;
};

class IHwVideoDecoder { /* ... 同形：output_surface_handle / supports / decoding_latency_us */ };
class IHwVideoCapture { /* supports_device / zero_copy_to_gpu */ };
```

继承示意：

```cpp
class MediaCodecVideoSender final
    : public nimrtc::plugins::IVideoSender        // 通用管线 + 包化
    , public nimrtc::plugins::IHwVideoEncoder     // HW seam: 后端 / surface / capability
{
public:
    // ---- IVideoSender (RTP packetization) ----
    void   set_packet_callback(VideoSenderPacketCallback) noexcept override;
    Status push_frame(const EncodedVideoFrame&, std::uint32_t rtp_ts) noexcept override;
    void   force_keyframe() noexcept override;
    VideoSenderStats stats() const noexcept override;
    std::uint16_t    next_seq() const noexcept override;
    VideoSenderConfig config() const noexcept override;

    // ---- IVideoSender HW capability overrides ----
    bool                is_hardware_accelerated() const noexcept override { return true; }
    std::string_view    hardware_backend()        const noexcept override { return "mediacodec"; }

    // ---- IHwVideoEncoder (HW seam) ----
    HwBackend           backend()                 const noexcept override { return HwBackend::MediaCodec; }
    void*               target_surface_handle()         noexcept override;
    bool                supports(VideoCodecKind, std::uint32_t w, std::uint32_t h, std::uint32_t fps) const noexcept override;
    std::uint32_t       encoding_latency_us()    const noexcept override;
};
```

多继承顺序：

- 第一基类必须是 `IVideo*`（注册路径只看它）。
- 第二基类 `IHwVideo*` 是**可选**的，仅用于需要暴露 surface / 能力查询的
  HW 插件。纯 CPU 插件不必实现 `IHwVideo*`，但仍可选择性地继承以保持 API
  对称（这时 `backend()` 直接返回 `HwBackend::Software`）。

---

## 3. Implementing a HW Plugin

下面四份样例覆盖三大主流 OS（Android / iOS / Linux）+ 跨平台捕获。所有
示例都遵循同一模板：

1. 类同时继承 `IVideo*` 和对应的 `IHwVideo*`；
2. capability flag 一律返回 `true` / `"<backend>"`；
4. `backend()` 返回对应 `HwBackend` 枚举；
5. `target_surface_handle()` / `output_surface_handle()` 返回 platform-specific
   opaque 指针（nullptr 表示不暴露）；
6. `supports*()` 永远不要假定 GPU 一定能撑住 4K60——按设备能力做 query；
7. 注册用 `NIMRTC_REGISTER_VIDEO_*` 宏，必须在 `main()` 之前完成。

### 3.1 Android NDK MediaCodec Encoder

```cpp
// file: mediacodec_sender.cpp
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <memory>
#include <string_view>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/hw_seam.hpp>
#include <nimrtc/core/registry.hpp>

namespace {

class MediaCodecVideoSender final
    : public nimrtc::plugins::IVideoSender
    , public nimrtc::plugins::IHwVideoEncoder
{
public:
    explicit MediaCodecVideoSender(nimrtc::plugins::VideoSenderConfig cfg)
        : cfg_(std::move(cfg)) {}

    ~MediaCodecVideoSender() override { close_codec(); }

    // ---- IPlugin ----
    nimrtc::Status open() noexcept override {
        // 1) 创建 encoder，输入 surface 让 GPU 直接合成（Camera2 → Surface）
        AMediaCodec* codec = AMediaCodec_createEncoderByType("video/avc");
        if (!codec) return nimrtc::kErrResourceExhausted;

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, "mime", "video/avc");
        AMediaFormat_setInt32(fmt, "width",  cfg_.ssrc ? 1280 : 1280); // 见 §5 tuning
        AMediaFormat_setInt32(fmt, "height", 720);
        AMediaFormat_setInt32(fmt, "bitrate", 2'500'000);
        AMediaFormat_setInt32(fmt, "frame-rate", 30);
        AMediaFormat_setInt32(fmt, "i-frame-interval", 1);
        AMediaFormat_setInt32(fmt, "color-format",
                              OMX_COLOR_FormatAndroidFlexibleSurface);
        AMediaCodec_configure(codec, fmt, /*surface*/ nullptr,
                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);

        // 2) 创建输入 ANativeWindow（应用可以把 SurfaceTexture 拿来用）
        ANativeWindow* win = nullptr;
        AMediaCodec_createInputSurface(codec, &win);  // NDK r21+
        surface_ = win;

        AMediaCodec_start(codec);
        codec_ = codec;
        return nimrtc::kOk;
    }

    void close() noexcept override { close_codec(); }

    // ---- IVideoSender ----
    void set_packet_callback(nimrtc::plugins::VideoSenderPacketCallback cb) noexcept override {
        cb_ = std::move(cb);
    }
    nimrtc::Status push_frame(const nimrtc::plugins::EncodedVideoFrame& frame,
                              std::uint32_t rtp_ts) noexcept override {
        // 1) 把 access unit 喂给 MediaCodec
        size_t idx = AMediaCodec_dequeueInputBuffer(codec_, /*timeoutUs*/ 10'000);
        if (idx >= 0) {
            uint8_t* dst = nullptr;
            size_t  cap  = 0;
            AMediaCodec_getInputBuffer(codec_, idx, &dst, &cap);
            std::memcpy(dst, frame.payload.data(),
                        std::min<size_t>(cap, frame.payload.size()));
            AMediaCodec_queueInputBuffer(codec_, idx, /*offset*/ 0,
                                         frame.payload.size(),
                                         frame.capture_ts_us,
                                         /*flags*/ 0);
        }
        // 2) 拉输出包，再走参考 sender 的 FU-A/STAP-A 拆分
        drain_output(rtp_ts);
        return nimrtc::kOk;
    }
    void force_keyframe() noexcept override { request_idr_ = true; }
    nimrtc::plugins::VideoSenderStats stats() const noexcept override { return stats_; }
    std::uint16_t next_seq() const noexcept override { return seq_; }
    nimrtc::plugins::VideoSenderConfig config() const noexcept override { return cfg_; }

    // ---- IVideoSender HW capability ----
    bool is_hardware_accelerated() const noexcept override { return true; }
    std::string_view hardware_backend() const noexcept override { return "mediacodec"; }

    // ---- IHwVideoEncoder ----
    nimrtc::plugins::HwBackend backend() const noexcept override {
        return nimrtc::plugins::HwBackend::MediaCodec;
    }
    void* target_surface_handle() noexcept override {
        return static_cast<void*>(surface_);
    }
    bool supports(nimrtc::plugins::VideoCodecKind codec,
                  std::uint32_t w, std::uint32_t h, std::uint32_t fps) const noexcept override {
        if (codec != nimrtc::plugins::VideoCodecKind::kH264) return false;
        // 大多数 Android 8+ 设备能撑 1080p30；4K60 只在部分 SoC 上 OK
        if (w * h >= 3840 * 2160 && fps > 30) return false;
        return w <= 1920 && h <= 1080 && fps <= 60;
    }
    std::uint32_t encoding_latency_us() const noexcept override { return 6'000; }

private:
    void drain_output(std::uint32_t rtp_ts) {
        AMediaCodecBufferInfo info{};
        while (AMediaCodec_dequeueOutputBuffer(codec_, &info, 0) >= 0) {
            // 把 access unit 推到参考 IVideoSender 风格的分片器
            // （此处省略 FU-A 切分，仅示意）
            nimrtc::plugins::EncodedVideoFrame f{};
            f.payload = nimrtc::plugins::BufferView{
                AMediaCodec_getOutputBuffer(codec_, info.index, nullptr), info.size};
            f.is_keyframe = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0
                         || (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
            f.capture_ts_us = info.presentationTimeUs;
            (void)rtp_ts;
            AMediaCodec_releaseOutputBuffer(codec_, info.index, /*render*/ false);
            stats_.frames_pushed++;
        }
    }
    void close_codec() {
        if (codec_) { AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); codec_ = nullptr; }
        if (surface_) { ANativeWindow_release(surface_); surface_ = nullptr; }
    }

    nimrtc::plugins::VideoSenderConfig cfg_;
    AMediaCodec* codec_ = nullptr;
    ANativeWindow* surface_ = nullptr;
    nimrtc::plugins::VideoSenderPacketCallback cb_;
    nimrtc::plugins::VideoSenderStats stats_{};
    std::uint16_t seq_ = 0;
    bool request_idr_ = false;
};

} // namespace

// 注册到 core::PluginRegistry，ID 必须与 §5 EngineConfig::video_sender_name 一致。
static const nimrtc::plugins::SimpleVideoSenderFactory<MediaCodecVideoSender>
    g_factory{"mediacodec", "Android NDK MediaCodec H.264 encoder"};
NIMRTC_REGISTER_VIDEO_SENDER(mediacodec, &g_factory);
```

> 注意：MediaCodec 的实际包化（FU-A 拆分 + RTP 头）由参考实现
> `VideoSender` 完成；上面的 sender 把 MediaCodec 视作"NALU 源"，再
> 复用基类分片器。

### 3.2 Apple VideoToolbox Decoder

```cpp
// file: videotoolbox_receiver.cpp
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>

#include <atomic>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/hw_seam.hpp>
#include <nimrtc/core/registry.hpp>

namespace {

class VideoToolboxReceiver final
    : public nimrtc::plugins::IVideoReceiver
    , public nimrtc::plugins::IHwVideoDecoder
{
public:
    explicit VideoToolboxReceiver(nimrtc::plugins::VideoReceiverConfig cfg)
        : cfg_(std::move(cfg)) {}

    ~VideoToolboxReceiver() override { teardown(); }

    nimrtc::Status open() noexcept override {
        // 1) 创建 decompression session
        CFDictionaryRef attrs = nullptr;  // 由 CMSampleBuffer 携带 SPS/PPS
        OSStatus st = VTDecompressionSessionCreate(
            /*allocator*/ kCFAllocatorDefault,
            /*videoFormatDesc*/ nullptr, attrs, nullptr,
            nullptr, &session_);
        if (st != noErr) return nimrtc::kErrResourceExhausted;
        return nimrtc::kOk;
    }
    void close() noexcept override { teardown(); }

    // ---- IVideoReceiver ----
    void set_frame_callback(nimrtc::plugins::VideoReceiverFrameCallback cb) noexcept override {
        on_frame_ = std::move(cb);
    }
    void set_nack_callback(nimrtc::plugins::VideoReceiverNackCallback cb) noexcept override {
        on_nack_ = std::move(cb);
    }
    nimrtc::Status push_rtp(const nimrtc::plugins::VideoRtpPacket& pkt,
                            nimrtc::TimestampUs now_us) noexcept override {
        // 1) 把 RTP payload 视作 CMSampleBuffer-like NALU 块喂给 VideoToolbox
        CMBlockBufferRef bb = nullptr;
        CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr,
                                           pkt.payload.size(), kCFAllocatorDefault,
                                           nullptr, 0, pkt.payload.size(),
                                           0, &bb);
        CMFormatDescriptionRef fmt = nullptr;
        // 省略：解析 SPS/PPS 构造 CMFormatDescription
        VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
        VTDecompressionSessionDecodeFrame(session_, fmt, /*decodeTime*/ nullptr,
                                          /*sourceFrameRefCon*/ nullptr, bb, flags);
        // 2) output handler 在另一线程 fire —— 这里以回调形式转发
        return nimrtc::kOk;
    }
    std::vector<std::uint16_t> pop_nack_entries() noexcept override {
        auto out = std::move(nack_buffer_); nack_buffer_.clear(); return out;
    }
    nimrtc::Status tick(nimrtc::TimestampUs) noexcept override { return nimrtc::kOk; }
    void reset() noexcept override { /* VTDecompressionSessionInvalidate + 重建 */ }
    nimrtc::plugins::VideoReceiverStats stats() const noexcept override { return stats_; }
    nimrtc::plugins::VideoReceiverConfig config() const noexcept override { return cfg_; }

    // ---- IVideoReceiver HW capability ----
    bool is_hardware_accelerated() const noexcept override { return true; }
    std::string_view hardware_backend() const noexcept override { return "videotoolbox"; }

    // ---- IHwVideoDecoder ----
    nimrtc::plugins::HwBackend backend() const noexcept override {
        return nimrtc::plugins::HwBackend::VideoToolbox;
    }
    void* output_surface_handle() noexcept override {
        // 返回最近一帧的 CVPixelBufferRef，让上层做 Metal 直接渲染
        return static_cast<void*>(latest_pixbuf_);
    }
    bool supports(nimrtc::plugins::VideoCodecKind codec,
                  std::uint32_t w, std::uint32_t h, std::uint32_t fps) const noexcept override {
        if (codec != nimrtc::plugins::VideoCodecKind::kH264) return false;
        // iPhone 5s 起的设备普遍支持 1080p30；4K60 仅 A12+
        return w <= 1920 && h <= 1080 && fps <= 60;
    }
    std::uint32_t decoding_latency_us() const noexcept override { return 4'000; }

private:
    void teardown() {
        if (session_) { VTDecompressionSessionInvalidate(session_); CFRelease(session_); session_ = nullptr; }
        if (latest_pixbuf_) { CVPixelBufferRelease(latest_pixbuf_); latest_pixbuf_ = nullptr; }
    }

    nimrtc::plugins::VideoReceiverConfig cfg_;
    VTDecompressionSessionRef session_ = nullptr;
    CVPixelBufferRef latest_pixbuf_ = nullptr;
    nimrtc::plugins::VideoReceiverFrameCallback on_frame_;
    nimrtc::plugins::VideoReceiverNackCallback  on_nack_;
    std::vector<std::uint16_t> nack_buffer_;
    nimrtc::plugins::VideoReceiverStats stats_{};
};

} // namespace

static const nimrtc::plugins::SimpleVideoReceiverFactory<VideoToolboxReceiver>
    g_factory{"videotoolbox", "Apple VideoToolbox H.264 decoder"};
NIMRTC_REGISTER_VIDEO_RECEIVER(videotoolbox, &g_factory);
```

### 3.3 Linux VAAPI Encoder

```cpp
// file: vaapi_sender.cpp
#include <va/va.h>
#include <va/va_enc_h264.h>

#include <vector>
#include <cstring>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/hw_seam.hpp>
#include <nimrtc/core/registry.hpp>

namespace {

class VaapiVideoSender final
    : public nimrtc::plugins::IVideoSender
    , public nimrtc::plugins::IHwVideoEncoder
{
public:
    explicit VaapiVideoSender(nimrtc::plugins::VideoSenderConfig cfg)
        : cfg_(std::move(cfg)) {}

    ~VaapiVideoSender() override { teardown(); }

    nimrtc::Status open() noexcept override {
        // 1) 打开 DRM render node 拿 VADisplay
        display_ = vaGetDisplayDRM(/*fd*/ drm_fd_);
        if (!display_) return nimrtc::kErrResourceExhausted;
        int major = 0, minor = 0;
        if (vaInitialize(display_, &major, &minor) != VA_STATUS_SUCCESS) {
            return nimrtc::kErrResourceExhausted;
        }
        // 2) 创建编码 context + H.264 包化 (AnnexG)
        VAConfigAttrib attr{};
        VAContextID ctx;
        vaCreateConfig(display_, VAProfileH264High,
                       VAEntrypointEncSlice, &attr, 1, &config_id_);
        vaCreateContext(display_, config_id_, 1920, 1080, 0, surfaces_.data(),
                       surfaces_.size(), &ctx);
        context_ = ctx;
        return nimrtc::kOk;
    }
    void close() noexcept override { teardown(); }

    // ---- IVideoSender ----
    void set_packet_callback(nimrtc::plugins::VideoSenderPacketCallback cb) noexcept override {
        cb_ = std::move(cb);
    }
    nimrtc::Status push_frame(const nimrtc::plugins::EncodedVideoFrame&,
                              std::uint32_t rtp_ts) noexcept override {
        // VAAPI encoder 输出在 VA surface（ID = VASurfaceID），需要
        // vaSyncSurface → vaMapBuffer（packed headers）+ bitstream 拷贝；
        // 之后按参考 VideoSender 的 FU-A/STAP-A 切分
        // （这里仅展示 hook 点）
        (void)rtp_ts;
        return nimrtc::kOk;
    }
    void force_keyframe() noexcept override { /* 设置 VAEncSequenceParameterBuffer 的 idr_period */ }
    nimrtc::plugins::VideoSenderStats stats() const noexcept override { return stats_; }
    std::uint16_t next_seq() const noexcept override { return seq_; }
    nimrtc::plugins::VideoSenderConfig config() const noexcept override { return cfg_; }

    // ---- IVideoSender HW capability ----
    bool is_hardware_accelerated() const noexcept override { return true; }
    std::string_view hardware_backend() const noexcept override { return "vaapi"; }

    // ---- IHwVideoEncoder ----
    nimrtc::plugins::HwBackend backend() const noexcept override {
        return nimrtc::plugins::HwBackend::Vaapi;
    }
    void* target_surface_handle() noexcept override {
        // VASurfaceID 在 NimRTC 里以 uintptr_t 形式传递；调用方需要按
        // VADisplay* 强制转换回 VASurfaceID
        return reinterpret_cast<void*>(static_cast<uintptr_t>(input_surface_id_));
    }
    bool supports(nimrtc::plugins::VideoCodecKind codec,
                  std::uint32_t w, std::uint32_t h, std::uint32_t fps) const noexcept override {
        if (codec != nimrtc::plugins::VideoCodecKind::kH264 &&
            codec != nimrtc::plugins::VideoCodecKind::kH265) return false;
        return w <= 3840 && h <= 2160 && fps <= 60;
    }
    std::uint32_t encoding_latency_us() const noexcept override { return 8'000; }

private:
    void teardown() {
        if (context_) { vaDestroyContext(display_, context_); context_ = 0; }
        if (config_id_) { vaDestroyConfig(display_, config_id_); config_id_ = 0; }
        if (display_)   { vaTerminate(display_); display_ = nullptr; }
    }

    nimrtc::plugins::VideoSenderConfig cfg_;
    VADisplay     display_   = nullptr;
    VAConfigID     config_id_ = 0;
    VAContextID    context_   = 0;
    std::vector<VASurfaceID> surfaces_{};
    VASurfaceID    input_surface_id_ = VA_INVALID_SURFACE;
    int            drm_fd_    = -1;
    nimrtc::plugins::VideoSenderPacketCallback cb_;
    nimrtc::plugins::VideoSenderStats stats_{};
    std::uint16_t seq_ = 0;
};

} // namespace

static const nimrtc::plugins::SimpleVideoSenderFactory<VaapiVideoSender>
    g_factory{"vaapi", "Linux VA-API H.264/H.265 encoder"};
NIMRTC_REGISTER_VIDEO_SENDER(vaapi, &g_factory);
```

### 3.4 Cross-platform capture (V4L2)

V4L2 / AVFoundation / DirectShow 三者结构相同；这里给 V4L2 版本：

```cpp
// file: v4l2_source.cpp
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <vector>
#include <nimrtc/plugins/video_source.hpp>
#include <nimrtc/plugins/hw_seam.hpp>
#include <nimrtc/core/registry.hpp>

namespace {

class V4L2VideoSource final
    : public nimrtc::plugins::IVideoSource
    , public nimrtc::plugins::IHwVideoCapture
{
public:
    explicit V4L2VideoSource(nimrtc::plugins::VideoSourceConfig cfg)
        : cfg_(std::move(cfg)) {}

    ~V4L2VideoSource() override { close(); }

    // ---- IPlugin ----
    nimrtc::Status open() noexcept override {
        fd_ = open(cfg_.pattern == nimrtc::plugins::VideoSourcePattern::kCustom
                       ? device_path_.c_str() : "/dev/video0",
                   O_RDWR | O_NONBLOCK);
        if (fd_ < 0) return nimrtc::kErrResourceExhausted;
        v4l2_format fmt{};
        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = cfg_.width;
        fmt.fmt.pix.height      = cfg_.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        ioctl(fd_, VIDIOC_S_FMT, &fmt);
        request_and_mmap_buffers();
        return nimrtc::kOk;
    }
    void close() noexcept override {
        if (fd_ >= 0) ::close(fd_);
        for (auto& b : bufs_) { if (b.start) munmap(b.start, b.length); }
        bufs_.clear();
    }

    // ---- IVideoSource ----
    nimrtc::Status start() noexcept override {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMON, &type);
        running_ = true;
        return nimrtc::kOk;
    }
    void stop() noexcept override {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
        running_ = false;
    }
    bool running() const noexcept override { return running_; }
    void produce_one() noexcept override { /* 在 V4L2 模型里通常无需手动触发 */ }
    nimrtc::plugins::VideoSourceConfig config() const noexcept override { return cfg_; }
    void set_pattern(nimrtc::plugins::VideoSourcePattern) noexcept override {}
    void set_callback(nimrtc::plugins::VideoSourceFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }
    nimrtc::plugins::IVideoSource::Stats stats() const noexcept override { return stats_; }

    // ---- IVideoSource HW capability ----
    bool is_hardware_accelerated() const noexcept override { return true; }
    std::string_view hardware_backend() const noexcept override { return "v4l2"; }

    // ---- IHwVideoCapture ----
    nimrtc::plugins::HwBackend backend() const noexcept override {
        return nimrtc::plugins::HwBackend::V4l2;
    }
    bool supports_device(std::string_view device_id, std::uint32_t w,
                         std::uint32_t h, std::uint32_t fps) const noexcept override {
        // 真实实现应临时 open() + VIDIOC_ENUM_FRAMESIZES / TIMEOUT
        return !device_id.empty() && w <= 1920 && h <= 1080 && fps <= 60;
    }
    bool zero_copy_to_gpu() const noexcept override {
        // 当用户绑定 V4L2 → DRM prime export 时为 true
        return use_drm_prime_;
    }

private:
    void request_and_mmap_buffers() {
        v4l2_requestbuffers req{};
        req.count  = 4;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        ioctl(fd_, VIDIOC_REQBUFS, &req);
        bufs_.resize(req.count);
        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_buffer b{};
            b.type = req.type; b.memory = req.memory; b.index = i;
            ioctl(fd_, VIDIOC_QUERYBUF, &b);
            bufs_[i].length = b.length;
            bufs_[i].start  = mmap(nullptr, b.length, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd_, b.m.offset);
            ioctl(fd_, VIDIOC_QBUF, &b);
        }
    }

    struct Buf { void* start = nullptr; std::size_t length = 0; };
    nimrtc::plugins::VideoSourceConfig cfg_;
    std::string device_path_ = "/dev/video0";
    int fd_ = -1;
    std::vector<Buf> bufs_;
    bool running_ = false;
    bool use_drm_prime_ = false;
    nimrtc::plugins::VideoSourceFrameCallback cb_;
    nimrtc::plugins::IVideoSource::Stats stats_{};
};

} // namespace

static const nimrtc::plugins::SimpleVideoSourceFactory<V4L2VideoSource>
    g_factory{"v4l2", "Linux V4L2 video capture"};
NIMRTC_REGISTER_VIDEO_SOURCE(v4l2, &g_factory);
```

AVFoundation / DirectShow 的实现完全平行：把 `v4l2_*` 换成
`AVCaptureSession` / `DirectShow::CaptureGraph`，device id 换成相机 UUID
或显示器 handle，`supports_device()` / `zero_copy_to_gpu()` 同样在接口
里体现。

---

## 4. Registration

所有 HW 插件都通过 `core::PluginRegistry::register_video_*()` 把自己
注册到全局表，注册名（id）必须**短且稳定**——它就是用户写在
`EngineConfig::video_*_name` 里的字符串。下列代码展示出 tree 插件
如何挂载自己的 id：

```cpp
// file: my_hw_plugin_init.cpp
#include <nimrtc/core/registry.hpp>

// 1) MediaCodec encoder（继承 IVideoSender + IHwVideoEncoder）
static const nimrtc::plugins::SimpleVideoSenderFactory<MediaCodecVideoSender>
    mediacodec_sender_factory{"mediacodec", "Android NDK MediaCodec encoder"};
NIMRTC_REGISTER_VIDEO_SENDER(mediacodec, &mediacodec_sender_factory);

// 2) VideoToolbox decoder
static const nimrtc::plugins::SimpleVideoReceiverFactory<VideoToolboxReceiver>
    videotoolbox_rx_factory{"videotoolbox", "Apple VideoToolbox decoder"};
NIMRTC_REGISTER_VIDEO_RECEIVER(videotoolbox, &videotoolbox_rx_factory);

// 3) Linux VAAPI encoder
static const nimrtc::plugins::SimpleVideoSenderFactory<VaapiVideoSender>
    vaapi_sender_factory{"vaapi", "Linux VA-API encoder"};
NIMRTC_REGISTER_VIDEO_SENDER(vaapi, &vaapi_sender_factory);

// 4) V4L2 / AVFoundation / DirectShow capture
static const nimrtc::plugins::SimpleVideoSourceFactory<V4L2VideoSource>
    v4l2_source_factory{"v4l2", "Linux V4L2 capture"};
NIMRTC_REGISTER_VIDEO_SOURCE(v4l2, &v4l2_source_factory);

extern "C" void my_hw_plugin_register() {
    // 上面那些静态对象在 dynamic library 加载时就已注册；
    // 此函数仅作为显式触发点（譬如 dlopen 后调用）。
}
```

插件被加载后，调用方可以枚举当前可用的 HW 后端：

```cpp
#include <nimrtc/core/registry.hpp>
#include <iostream>

int main() {
    nimrtc::core::register_all_default_plugins();  // 先装默认软实现
    my_hw_plugin_register();                       // 再装 HW 实现

    auto& reg = nimrtc::core::PluginRegistry::instance();
    std::cout << "available video senders:\n";
    for (auto id : reg.list_video_senders()) {
        std::cout << "  - " << std::string(id) << "\n";
    }
    return 0;
}
```

---

## 5. EngineConfig wiring

`EngineConfig` 的四个 `video_*_name` 字段决定 engine 在 `open()` 时从
`PluginRegistry` 里挑哪个实现。默认值是软实现，HW 用户只需覆盖成上面
注册的 id：

```104:160:src/engine/include/nimrtc/engine/engine.hpp
    /** IVideoSource plugin id (e.g. "memory", or HW plugin id). */
    std::string_view video_source_name = "memory";

    /** IVideoSink plugin id (e.g. "headless", "pinned", or HW renderer id). */
    std::string_view video_sink_name   = "headless";

    /** IVideoReceiver plugin id (e.g. "reference"). */
    std::string_view video_receiver_name = "reference";

    /** IVideoSender plugin id (e.g. "reference"). */
    std::string_view video_sender_name   = "reference";
```

应用代码：

```cpp
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/core/registry.hpp>

int main() {
    // 1) 先注册（默认 + 第三方 HW）
    nimrtc::core::register_all_default_plugins();
    load_my_hw_plugins();   // 见 §4

    // 2) 选 HW 后端
    nimrtc::engine::EngineConfig cfg;
    cfg.video_source_name   = "v4l2";          // Linux 摄像头
    cfg.video_sink_name     = "headless";      // 暂不显示（demo）
    cfg.video_receiver_name = "videotoolbox";  // iOS / macOS 端
    cfg.video_sender_name   = "mediacodec";    // Android 端
    cfg.video_sender_tuning.ssrc         = 0xCAFEBABE;
    cfg.video_sender_tuning.payload_type = 102;
    cfg.video_sender_tuning.mtu          = 1200;

    nimrtc::engine::NimRTCEngine engine(cfg);
    if (engine.open() != 0) return 1;

    // 3) 也可以运行时切换后端（譬如 fallback 到 software）
    if (auto* sender = engine.video_sender()) {
        if (!sender->is_hardware_accelerated()) {
            // 警告：实际重连需要重建 engine 实例；
            // 本例仅展示查询用法
        }
    }
    return 0;
}
```

> 引擎启动后会调用 `init_video_plugins()` 按 cfg 把四个插件实例化；
> 若 id 没找到，对应的 `engine.video_*()` 返回 `nullptr`，调用方应
> 自己兜底（譬如退到默认软实现）。

---

## 6. Capability Queries

`hw_seam.hpp` 提供了类型安全的 helper `is_hardware_accelerated<>()`，
它**综合三件事**：

1. 指针非空；
2. `IVideo*::is_hardware_accelerated()` 返回 true；
3. `IVideo*::hardware_backend()` 字符串 ≠ `"software"`。

```cpp
template <class PluginT, class HwInterfaceT>
inline bool is_hardware_accelerated(PluginT* plugin, HwInterfaceT* /*hint*/) noexcept {
    if (!plugin) return false;
    if (!plugin->is_hardware_accelerated()) return false;
    return plugin->hardware_backend() != "software";
}
```

注意第二个参数是 `HwInterfaceT* hint`——它把"我期望这个 plugin 至少是
IHwVideoEncoder/Decoder/Capture 之一"的契约写在签名里，调用方必须
传对应的 hint：

```cpp
#include <nimrtc/plugins/hw_seam.hpp>

void describe(nimrtc::plugins::IVideoSender* s) {
    // 用 IHwVideoEncoder 作为 hint，编译器会在多继承缺失时报错
    nimrtc::plugins::IHwVideoEncoder* hint = nullptr;
    if (nimrtc::plugins::is_hardware_accelerated(s, hint)) {
        NIMRTC_LOG_INFO("HW sender: {}", std::string(s->hardware_backend()));
    } else {
        NIMRTC_LOG_WARN("falling back to software sender");
    }
}

// 只看 capability flag 字符串的场景：
std::string_view backend_name(nimrtc::plugins::IVideoReceiver* r) {
    return r ? r->hardware_backend() : "none";
}

// 跨四个接口汇总一份"当前 pipeline 报告"
void dump_capabilities(const nimrtc::engine::NimRTCEngine& e) {
    auto* src = e.video_source();
    auto* snk = e.video_sink();
    auto* rx  = e.video_receiver();
    auto* tx  = e.video_sender();
    NIMRTC_LOG_INFO("source={} sink={} rx={} tx={}",
        src ? src->hardware_backend() : "none",
        snk ? snk->hardware_backend() : "none",
        rx  ? rx ->hardware_backend() : "none",
        tx  ? tx ->hardware_backend() : "none");
}
```

---

## 7. Zero-Copy Surface Handles

HW 后端最值钱的能力之一是"GPU 纹理到 GPU 编码器"的零拷贝。`hw_seam.hpp`
为此暴露**三个** opaque 入口：

| 方法 | 所在类 | 含义 |
| --- | --- | --- |
| `IHwVideoEncoder::target_surface_handle()` | encoder | encoder **输入** surface（GPU 上写入） |
| `IHwVideoDecoder::output_surface_handle()` | decoder | decoder **输出** surface（GPU 上读出） |
| `IHwVideoCapture::zero_copy_to_gpu()` | capture | capture 能否直接产 GPU buffer |

合约：

- 返回 `nullptr` 表示该实例不暴露 HW surface（譬如 MediaCodec 用
  byte buffer 模式而非 surface 模式）；调用方必须按 `nullptr` 走 CPU
  fallback，不能 cast 成任何具体类型。
- **调用方负责类型转换**：`void*` 的目标类型是平台相关的，常见映射：

```cpp
// 1) Android NDK MediaCodec encoder
ANativeWindow* win = static_cast<ANativeWindow*>(
    encoder->target_surface_handle());

// 2) Apple VideoToolbox decoder
CVPixelBufferRef pixbuf = static_cast<CVPixelBufferRef>(
    decoder->output_surface_handle());

// 3) Linux VAAPI encoder / decoder
VASurfaceID surface = static_cast<VASurfaceID>(
    reinterpret_cast<uintptr_t>(encoder->target_surface_handle()));

// 4) Windows DirectX Video Acceleration
ID3D11Texture2D* tex = static_cast<ID3D11Texture2D*>(
    decoder->output_surface_handle());
```

- **生命周期**：handle 归实现所有；调用方不得释放。encoder 在
  `close()` / 析构前一直有效；decoder 的 `output_surface_handle()`
  可能每帧变化（譬如重建 pool），调用方应**每帧取一次**。
- **线程模型**：surface 访问与 plugin 主线程同步；调用方如果跨线程
  持有 handle，必须自己做同步（譬如 OpenGL 上下文切换、Metal
  command buffer barriers）。

零拷贝 capture：

```cpp
// 譬如 V4L2 用了 V4L2_PIX_FMT_DRM_PRIME 时：
if (capture->zero_copy_to_gpu()) {
    // 直接把 DRM prime fd → DMA-BUF → Vulkan / OpenGL import
    // 无需走 IVideoSourceFrameCallback 的 plane_y/u/v 路径
}
```

---

## 8. Backend Selection Logic（应用作者视角）

下列伪代码展示了应用运行时**如何根据平台 + capability 选择最佳
后端**。把它放进 `select_best_video_sender(cfg)` 之类的辅助函数里：

```cpp
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/hw_seam.hpp>
#include <nimrtc/core/registry.hpp>

#if defined(__ANDROID__)
#  define NIMRTC_TARGET_OS "android"
#elif defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    define NIMRTC_TARGET_OS "ios"
#  else
#    define NIMRTC_TARGET_OS "macos"
#  endif
#elif defined(_WIN32)
#  define NIMRTC_TARGET_OS "windows"
#else
#  define NIMRTC_TARGET_OS "linux"
#endif

std::string_view select_best_video_sender() {
    auto& reg = nimrtc::core::PluginRegistry::instance();

    auto try_id = [&](std::string_view id) -> std::optional<std::string_view> {
        auto* f = reg.get_video_sender(id);
        if (!f) return std::nullopt;
        // 通过 factory 探一探 capabilities：
        std::unique_ptr< nimrtc::plugins::IVideoSender >
            probe(f->create({nimrtc::plugins::VideoCodecKind::kH264,
                              /*ssrc*/ 0, /*pt*/ 102, /*mtu*/ 1200,
                              nimrtc::plugins::VideoPacketizationMode::kNonInterleaved,
                              /*init_seq*/ 0}));
        probe->open();
        if (probe->is_hardware_accelerated()) return id;
        probe->close();
        return std::nullopt;
    };

    // 1) 平台偏好
#if defined(NIMRTC_TARGET_OS) && NIMRTC_TARGET_OS == "android"
    if (auto id = try_id("mediacodec"))    return *id;
#elif NIMRTC_TARGET_OS == "ios" || NIMRTC_TARGET_OS == "macos"
    if (auto id = try_id("videotoolbox"))  return *id;
#elif NIMRTC_TARGET_OS == "windows"
    if (auto id = try_id("nvenc"))         return *id;
    if (auto id = try_id("qsv"))           return *id;
    if (auto id = try_id("amf"))           return *id;
#else
    if (auto id = try_id("vaapi"))         return *id;
    if (auto id = try_id("nvdec"))         return *id;
    if (auto id = try_id("qsv"))           return *id;
#endif

    // 2) 通用回退
    if (auto id = try_id("openh264"))       return *id;
    return "reference";  // 永远存在的纯软实现
}
```

要点：

- **早查询**：`probe->is_hardware_accelerated()` 在 GPU 上 H.264 不可用
  时会返回 false；这是判定 SoC 是否真的支持的关键信号。
- **probe 的代价**：create + open 一个 dummy 实例大约几十 ms，可放在
  `engine.open()` 之前一次性完成；上线后只在配置变更时再跑。
- **避免 dynamic_cast**：所有判定都走 capability flag / `IHwVideo*::backend()`，
  与 §6 的 helper 一致。

---

## 9. Best Practices

- **永远实现 capability flag**：哪怕你的实现其实就是软路径（比如基于
  OpenH264 的 SIMD 优化版），仍然 override `is_hardware_accelerated()`
  返回 `false`、`hardware_backend()` 返回 `"openh264"`。这让 telemetry
  / 诊断 / 自动降级逻辑能正确工作。
- **`backend()` 与字符串对齐**：`IHwVideo*::backend()` 的
  `hw_backend_name()` 结果必须等于 `IVideo*::hardware_backend()` 的
  返回值。同一 plugin 出现差异会被 §6 helper 误判为软件。
- **surface handle 必为 nullptr-安全**：若你的实现不暴露 surface，
  让 `target_surface_handle()` / `output_surface_handle()` 返回
  `nullptr`，调用方会按 CPU fallback 处理。
- **`supports*()` 必须保守**：4K60 / AV1 硬件编码在大量 SoC 上不可
  用；返回 false 让上层把任务路由到软实现或拒绝。
- **延迟报告按基线**：HW 编码器典型 0–10 ms；软件实现参考软件路径；
  用于 jitter buffer 调参的 telemetry。
- **多继承顺序固定**：`IVideo*` 在前，`IHwVideo*` 在后，避免菱形
  vtable 冲突。
- **registration 在 main 之前**：`NIMRTC_REGISTER_VIDEO_*` 宏依赖静态
  构造注册，dylib 加载顺序要保证在 `register_all_default_plugins()`
  之前完成。
- **不要在 `hw_seam.hpp` 里 include 平台 SDK**：保持头文件纯净；
  所有 platform-specific 类型只在 `.cpp` 里出现，通过 `void*` 出参。
- **测试样例**：`run_test_target_surface_handle()` 应该验证非 nullptr
  + 类型转换有效，并在 `close()` 后验证 vtable 仍可读（析构安全）。

---

## 10. References

---

## 11. In-Tree HW Backends (P3-Batch)

As of P3-Batch, NimRTC ships **real SDK bindings** for six HW backends,
not just placeholder stubs. Each backend lives in its own source file under
`src/modules/h264/src/` and is gated by a `NIMRTC_PLUGINS_<NAME>_ON` compile
flag plus a corresponding SDK detection step in `cmake/NimRTHwPlugins.cmake`.

### 11.1 Backend matrix

| Backend    | File                          | Platforms       | SDK / library                | Priority |
|------------|-------------------------------|-----------------|------------------------------|----------|
| NVENC      | `nvenc_encoder.cpp`           | Win + Linux     | NVIDIA Video Codec SDK       | 450      |
| NVDEC      | `nvenc_encoder.cpp` (decoder) | Win + Linux     | NVIDIA Video Codec SDK       | 430      |
| AMF        | `amf_encoder.cpp`             | Windows only    | AMD AMF SDK                  | 380      |
| VA-API     | `vaapi_encoder.cpp`           | Linux only      | libva + libva-drm            | 360      |
| QSV        | `qsv_encoder.cpp`             | Win + Linux     | Intel libvpl / oneVPL        | 340      |
| DXVA       | `dxva_decoder.cpp`            | Windows only    | Windows SDK d3d11 + MF       | 320      |
| OpenH264   | `openh264_encoder.cpp`        | Win + Linux     | libopenh264                  | 50       |
| Stub       | `codec_plugin.cpp`            | all             | (none)                       | 0        |

### 11.2 CMake flags

```sh
# Default: all HW plugins OFF (clean tree build).
cmake -S . -B build -DNIMRTC_BUILD_TESTS=ON

# Enable NVIDIA NVENC + NVDEC:
cmake -S . -B build -DNIMRTC_PLUGINS_NVENC=ON
# (CMake auto-probes NVIDIA Video Codec SDK / CUDA include dirs)

# Enable AMD AMF (Windows only):
cmake -S . -B build -DNIMRTC_PLUGINS_AMF=ON
# (CMake auto-probes AMD AMF SDK)

# Enable Intel QSV via libvpl:
cmake -S . -B build -DNIMRTC_PLUGINS_QSV=ON
# (CMake auto-probes Intel oneVPL / MediaSDK)

# Enable Microsoft DXVA decoder (Windows only):
cmake -S . -B build -DNIMRTC_PLUGINS_DXVA=ON

# Enable Linux VA-API encoder + decoder:
cmake -S . -B build -DNIMRTC_PLUGINS_VAAPI=ON

# Enable OpenH264 software fallback:
cmake -S . -B build -DNIMRTC_PLUGINS_OPENH264=ON
```

CMake honours the following environment variables for path overrides:

| Variable                       | Effect                          |
|--------------------------------|---------------------------------|
| `NVIDIA_VIDEO_CODEC_SDK_DIR`   | NVENC include/lib root          |
| `CUDA_PATH` / `CUDA_HOME`      | CUDA include / lib64            |
| `AMF_SDK_DIR`                  | AMD AMF header root             |
| `LIBVPL_SDK_DIR` / `MFX_HOME`  | Intel libvpl / MediaSDK root    |
| `LIBVA_INCLUDE_DIR` / `LIBVA_LIBRARY` | VA-API header + lib root   |

### 11.3 How `available()` works

Each backend exposes two symbols:

```cpp
namespace nimrtc::h264 {
    bool <backend>_h264_available() noexcept;
    std::unique_ptr<plugins::IVideoCodec>
    make_<backend>_h264_codec(plugins::VideoCodecConfig cfg);
}
```

`hw_backends.cpp` wires those into the `HwVideoBackendRegistry` table
(`register_default_video_backends()`). The selector
(`select_encoder_backend()` / `select_decoder_backend()`) probes each
backend in priority order; the first one whose `available()` returns true
is selected. When the SDK is absent, the stub backend (priority 0) wins.

### 11.4 Testing HW backends

The unit test suite `tests/test_hw_backends.cpp` validates the dispatch
surface (registration, priority ordering, helper functions in
`hw_backend_base.hpp`) on **any host** — no GPU required. Integration
tests that actually exercise each SDK live in the SDK's vendor package
(sunshine's `tests/hw/` for NVENC, etc.) and are out of scope here.

### 11.5 Adding a new backend

1. Create `Find<X>.cmake` in `cmake/` (search pattern, env vars, fallback).
2. Add a `NIMRTC_PLUGINS_<X>` cache option to the top-level
   `CMakeLists.txt`.
3. Add a `_nimrtc_locate_<x>` function and a `nimrtc_link_<x>` helper to
   `cmake/NimRTHwPlugins.cmake`.
4. Implement `<x>_encoder.cpp` (and/or `<x>_decoder.cpp`) in
   `src/modules/h264/src/`, exporting the `<x>_h264_available()` /
   `make_<x>_h264_codec()` symbols.
5. Wire the backend entry into `hw_backends.cpp::register_default_video_backends()`.
6. Add a unit test in `tests/test_hw_backends.cpp`.

核心头文件（本文所有片段的来源）：

- `src/plugins/include/nimrtc/plugins/hw_seam.hpp` — `HwBackend` 枚举、
  `hw_backend_name()`、`IHwVideoEncoder` / `IHwVideoDecoder` /
  `IHwVideoCapture`、`is_hardware_accelerated<>()` helper。
- `src/plugins/include/nimrtc/plugins/video_source.hpp` —
  `IVideoSource` / `IVideoSourceFactory` 与 capability flag overrides。
- `src/plugins/include/nimrtc/plugins/video_sink.hpp` — `IVideoSink`
  与 capability flag overrides。
- `src/plugins/include/nimrtc/plugins/video_pipeline.hpp` —
  `IVideoReceiver` / `IVideoSender` 与 capability flag overrides。
- `src/engine/include/nimrtc/engine/engine.hpp` — `EngineConfig` 的
  `video_*_name` 字段、`video_*_tuning` 子结构，以及 `video_source()` /
  `video_sink()` / `video_receiver()` / `video_sender()` 访问器。
- `src/core/include/nimrtc/core/registry.hpp` — `PluginRegistry` 的
  `register_video_*` / `get_video_*` / `list_video_*`，以及
  `NIMRTC_REGISTER_VIDEO_*` 宏族。

文档与 ADR：

- `ARCHITECTURE.md` §2.3（设备插件化）与 §6（视频管线）。
- `ARCHITECTURE.md` §8.4（统一 timeline / capture timestamp）。
- ADR-001（plugin seam 选择 by string ID）—— 见 `docs/adr/001-plugin-seam.md`
  （如有）。

跨平台 HW SDK 参考（链接官方文档，便于实现细节查询）：

- Android NDK MediaCodec：`https://developer.android.com/ndk/reference/group/media`
- Apple VideoToolbox：`https://developer.apple.com/documentation/videotoolbox`
- Linux VA-API：`https://github.com/intel/libva`
- NVIDIA Video Codec SDK：`https://developer.nvidia.com/nvidia-video-codec-sdk`
- Intel Media SDK / oneVPL：`https://www.intel.com/content/www/us/en/developer/tools/onevpl/overview.html`
- AMD AMF：`https://github.com/GPUOpen-LibrariesAndSDKs/AMF`
- Microsoft DXVA：`https://learn.microsoft.com/en-us/windows/win32/medfound/directx-video-acceleration`
- Linux V4L2：`https://www.linuxtv.org/docs.php`

> 最后修订：R3-Batch。接口稳定但二进制布局 TBD P3；新增 `HwBackend`
> 枚举值时记得同步更新 `hw_backend_name()` 与本文第 2.2 节。

---

## 12. Known driver/runtime issues

This section records *specific* NVENC / driver combinations we have
debugged on the bench, so the next person to hit them doesn't lose an
afternoon.

### 12.1 Driver 591.86 / nvEncodeAPI64.dll v32.0.15.9186 (RTX 3060)

Symptom: `tests/nvenc_probe.cpp` exits 77 with
`"nvEncOpenEncodeSessionEx failed (status=15)"`. Status 15 is
`NV_ENC_ERR_INVALID_VERSION`.

Findings (verified on this machine):

1. The driver DLL only exports **10** symbols:
   - `NvEncodeAPICreateInstance`
   - `NvEncodeAPIGetMaxSupportedVersion`
   - Eight legacy `NvTool*` helpers

   The pre-CUDA-11 direct exports (`NvEncOpenEncodeSessionEx`,
   `NvEncInitializeEncoder`, …) are gone — anyone trying
   `GetProcAddress(dll, "NvEncOpenEncodeSessionEx")` fails with
   `GetLastError() == 127`. The fix is to use
   `NvEncodeAPICreateInstance(&fnList)` and call through `fnList.*`.

2. `NvEncodeAPIGetMaxSupportedVersion()` returns `0x000000D0` from this
   driver — this **does not** match `NVENCAPI_VERSION = (MAJOR) |
   (MINOR << 24)` for any documented SDK. Treat that value as
   driver-internal and don't rely on it for compatibility gating.

3. `NvEncodeAPICreateInstance()` succeeds and populates a 43-entry
   `NV_ENCODE_API_FUNCTION_LIST`. The function at struct offset `0x0F0`
   (our `nvEncOpenEncodeSessionEx`) **still** rejects every
   `NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS` struct version we tried — SDK
   8/9/10/11/12/13/14, both raw major-only and
   `(MAJOR | (MINOR << 24))` forms, plus 28-byte SDK 11.0 minimal
   layouts and the full 1552-byte SDK 13.1 layout. All return
   `NV_ENC_ERR_INVALID_VERSION` (15).

   The deprecated `nvEncOpenEncodeSession` (offset `0x008`) returns
   `NV_ENC_ERR_INVALID_CALL` (9) — its SDK-doc contract is "deprecated,
   use Ex", so it will never succeed through the function-table path.

Root cause hypothesis: this driver 32.0.15.9186 (Studio 591.86) bundles
an **NVENC SDK 11.0 runtime** internally. `NvEncodeAPICreateInstance`
accepts any SDK 12/13 function-table version (we get a populated
table), but the underlying NVENC dispatch is hard-wired to the SDK 11.0
struct ABI, so it rejects any `OPEN_ENCODE_SESSION_EX_PARAMS` whose
`version` field encodes a non-11.x SDK.

Fix paths (in increasing order of pain):

- **A. Update the driver.** Studio / Game Ready 560+ ships an NVENC
  runtime whose ABI matches SDK 12.x headers. This is the right answer
  for a normal development machine; the RTX 3060 supports up to SDK 13.x.
- **B. Vendor SDK 11.0 headers.** The legacy SDK 11.1.5 ZIP is on
  NVIDIA's archive page; place it under
  `build/nv_sdk/Video_Codec_Interface_11.1.5/Interface/`, redefine
  `NVENCAPI_VERSION = 11`, and use the smaller SDK 11.0
  `NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS` (no `reserved1[253]` /
  `reserved2[64]`). Then update `FindNVENC.cmake` to prefer the 11.x
  include dir.
- **C. Auto-detect the bundled ABI at runtime.** Load `nvEncodeAPI64.dll`,
  call `NvEncodeAPICreateInstance`, then probe a small set of struct
  versions (0x7000000B, 0x7001000B, …, 0x7101000D) against
  `nvEncOpenEncodeSessionEx` with a stub device and pick the first one
  that doesn't return `INVALID_VERSION`. This is what production
  tooling like OBS / FFmpeg / sunshine does.

For now, `nvenc_encoder.cpp` correctly returns `nullptr` from
`make_nvenc_h264_codec()`, so the engine falls through to the next
backend in priority order — no risk of a runtime crash.

### 12.2 Ad-hoc probe binaries

`build/nvenc_probe_v2.exe`, `build/nvenc_maxver.exe`,
`build/nvenc_walk_versions.exe`, `build/nvenc_session_open.exe`,
`build/nvenc_real_device.exe`, `build/nvenc_struct_ver.exe`,
`build/nvenc_sdk11_struct.exe`, `build/nvenc_deprecated.exe`, and the
accompanying `*.cpp` / `*.ps1` / `*.bat` files are **diagnostic
artefacts only** and should be deleted before commit (`rm build/nvenc_*
build/*.ps1`). The committed probe is `tests/nvenc_probe.cpp`.