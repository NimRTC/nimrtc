// modules/video_capture/src/platform/linux/v4l2_camera.cpp
// ----------------------------------------------------------------------------
// The Linux V4L2 camera (remote-desktop plan A9).
//
// The V4L2 capture path is the classic one:
//
//   1. `open("/dev/videoN", O_RDWR | O_NONBLOCK)`.
//   2. `VIDIOC_QUERYCAP` to check that the device is a capture device
//      and to learn its formats.
//   3. `VIDIOC_S_FMT` with `V4L2_BUF_TYPE_VIDEO_CAPTURE` and
//      `V4L2_PIX_FMT_NV12` to set the format.
//   4. `VIDIOC_REQBUFS` to ask for the buffers, `VIDIOC_QUERYBUF` to map
//      them, and `mmap` to map them into the process's address space.
//   5. `VIDIOC_QBUF` to queue them, `VIDIOC_STREAMON` to start.
//   6. `select` / `poll` on the device, then `VIDIOC_DQBUF` to
//      dequeue a filled buffer.
//
// The dequeued buffer is copied into the portable layer's own storage
// (the device reuses the buffer as soon as it is re-queued, so the copy
// is mandatory), and the copy is counted honestly in
// `CaptureStats::bytes_copied`.
//
// The device is opened **non-blocking** so that the capture thread can
// poll it with a short timeout instead of parking inside `read()`.
//
// @note Remote-desktop plan A9.  Platform adapter — the portable half
//       lives in `src/camera_capture.cpp`.
// ----------------------------------------------------------------------------

#include "platform/camera_factory.hpp"

#if defined(__linux__)

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace nimrtc::video_capture {

namespace {

/** The number of buffers the camera is asked for. */
constexpr int kBufferCount = 4;

/** One mmap'ed buffer. */
struct MappedBuffer {
    void* data = nullptr;
    std::size_t size = 0;
    bool queued = false;
};

/**
 * @brief The V4L2 camera frame source.
 */
class V4l2CameraSource final : public ICameraFrameSource {
  public:
    V4l2CameraSource() = default;

    ~V4l2CameraSource() override { close(); }

    V4l2CameraSource(const V4l2CameraSource&) = delete;
    V4l2CameraSource& operator=(const V4l2CameraSource&) = delete;

    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_cameras() const noexcept override {
        std::vector<plugins::CaptureDevice> devices;

        // The camera nodes are `/dev/videoN`.  The loop is bounded so a
        // pathological host cannot make this hang.
        for (int index = 0; index < 32; ++index) {
            const std::string path = "/dev/video" + std::to_string(index);
            const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
            if (fd < 0) continue;

            v4l2_capability cap{};
            if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
                ::close(fd);
                continue;
            }
            // Only capture devices are cameras; a V4L2 *output* node is
            // something else entirely.
            const bool is_capture = (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) != 0 ||
                                    (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
            if (!is_capture) {
                ::close(fd);
                continue;
            }

            plugins::CaptureDevice device;
            device.id = path;
            device.name = std::string(reinterpret_cast<const char*>(cap.card));
            device.kind = plugins::CaptureDeviceKind::kCamera;

            // The largest format the device offers.
            v4l2_frmsizeenum sizes{};
            std::uint32_t max_w = 0;
            std::uint32_t max_h = 0;
            for (std::uint32_t i = 0; i < cap.max_framesize && i < 4096; ++i) {
                sizes.index = i;
                sizes.pixel_format = V4L2_PIX_FMT_NV12;
                sizes.which = V4L2_FRMSIZE_TYPE_DISCRETE;
                if (::ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &sizes) != 0) continue;
                if (sizes.discrete.width > max_w) max_w = sizes.discrete.width;
                if (sizes.discrete.height > max_h) max_h = sizes.discrete.height;
            }
            device.max_width = max_w;
            device.max_height = max_h;
            device.max_fps = 30;

            devices.push_back(std::move(device));
            ::close(fd);
        }

        return devices;
    }

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override {
        std::vector<plugins::CaptureFormat> formats;

        const int fd = ::open(device_id.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) return formats;

        v4l2_fmtdesc desc{};
        desc.index = 0;
        desc.pixel_format = V4L2_PIX_FMT_NV12;
        desc.which = V4L2_FMT_DESC_COMPRESSED;

        // The NV12 sizes the device offers.
        v4l2_frmsizeenum sizes{};
        std::uint32_t max_w = 0;
        std::uint32_t max_h = 0;
        for (std::uint32_t i = 0; i < 4096; ++i) {
            sizes.index = i;
            sizes.pixel_format = V4L2_PIX_FMT_NV12;
            sizes.which = V4L2_FRMSIZE_TYPE_DISCRETE;
            if (::ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &sizes) != 0) break;
            if (sizes.discrete.width > max_w) max_w = sizes.discrete.width;
            if (sizes.discrete.height > max_h) max_h = sizes.discrete.height;
        }
        ::close(fd);

        if (max_w == 0 || max_h == 0) return formats;

        const std::uint32_t sizes_out[][2] = {{640, 480}, {1280, 720}, {max_w, max_h}};
        for (const auto& s : sizes_out) {
            if (s[0] > max_w || s[1] > max_h) continue;
            plugins::CaptureFormat format;
            format.width = s[0];
            format.height = s[1];
            format.fps = 30;
            format.tag = "nv12-v4l2";
            format.zero_copy = false;
            format.has_dirty_rects = false;
            formats.push_back(std::move(format));
        }
        return formats;
    }

    plugins::Status open(const std::string& device_id,
                         const plugins::CaptureFormat& format) noexcept override {
        close();

        fd_ = ::open(device_id.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) return plugins::kErrHardwareError;

        v4l2_capability cap{};
        if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) != 0) {
            close();
            return plugins::kErrHardwareError;
        }

        // ---- The format ---------------------------------------------------
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = format.width;
        fmt.fmt.pix.height = format.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;

        if (::ioctl(fd_, VIDIOC_S_FMT, &fmt) != 0) {
            close();
            return plugins::kErrUnsupported;
        }
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) {
            // The device refused NV12.  The camera path is NV12 by
            // contract (it is what the encoder path wants), so this is
            // an honest failure rather than a silent format change.
            close();
            return plugins::kErrUnsupported;
        }

        width_ = fmt.fmt.pix.width;
        height_ = fmt.fmt.pix.height;
        stride_ = static_cast<std::int32_t>(width_);

        // ---- The buffers ----------------------------------------------------
        v4l2_requestbuffers req{};
        req.count = kBufferCount;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd_, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
            close();
            return plugins::kErrHardwareError;
        }

        buffers_.resize(req.count);
        for (std::uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (::ioctl(fd_, VIDIOC_QUERYBUF, &buf) != 0) {
                close();
                return plugins::kErrHardwareError;
            }
            void* mapped = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                                  buf.m.offset);
            if (mapped == MAP_FAILED) {
                close();
                return plugins::kErrHardwareError;
            }
            buffers_[i].data = mapped;
            buffers_[i].size = buf.length;

            if (::ioctl(fd_, VIDIOC_QBUF, &buf) != 0) {
                close();
                return plugins::kErrHardwareError;
            }
            buffers_[i].queued = true;
        }

        // ---- The stream -----------------------------------------------------
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (::ioctl(fd_, VIDIOC_STREAMON, &type) != 0) {
            close();
            return plugins::kErrHardwareError;
        }
        streaming_ = true;
        return plugins::kOk;
    }

    void close() noexcept {
        if (streaming_) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ::ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (auto& buf : buffers_) {
            if (buf.data != nullptr && buf.data != MAP_FAILED) {
                ::munmap(buf.data, buf.size);
            }
        }
        buffers_.clear();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        width_ = 0;
        height_ = 0;
    }

    plugins::Status next_frame(std::vector<std::uint8_t>& pixels, std::size_t& size,
                               std::int32_t& stride, std::uint32_t& width,
                               std::uint32_t& height, std::int64_t& ts) noexcept override {
        if (fd_ < 0) return plugins::kErrNotReady;

        // The device is non-blocking, so the wait is a `poll` with a
        // short timeout.  The wait happens on the camera's own thread.
        struct pollfd pfd {};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 50);
        if (ready == 0) return plugins::kErrNotReady;
        if (ready < 0) {
            if (errno == EINTR) return plugins::kErrNotReady;
            return plugins::kErrHardwareError;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd_, VIDIOC_DQBUF, &buf) != 0) {
            if (errno == EAGAIN) return plugins::kErrNotReady;
            return plugins::kErrHardwareError;
        }

        // The buffer is the device's own storage, and it is re-queued
        // right away — so the bytes are copied out first.
        const std::size_t needed = static_cast<std::size_t>(width_) * height_ * 3u / 2u;
        const std::size_t available =
            (buf.index < buffers_.size()) ? buffers_[buf.index].size : 0u;
        const std::size_t copy = (needed < available) ? needed : available;
        if (pixels.size() < copy) pixels.resize(copy);
        if (copy > 0 && buf.bytesused > 0) {
            const std::size_t usable = (buf.bytesused < copy) ? buf.bytesused : copy;
            std::memcpy(pixels.data(), buffers_[buf.index].data, usable);
        }

        // The buffer goes back to the device.
        if (::ioctl(fd_, VIDIOC_QBUF, &buf) != 0) {
            return plugins::kErrHardwareError;
        }

        size = copy;
        stride = stride_;
        width = width_;
        height = height_;
        // V4L2's timestamp is the time the *driver* captured the frame;
        // it is monotonic on Linux and already in microseconds.
        ts = (buf.timestamp.tv_sec > 0)
                  ? (static_cast<std::int64_t>(buf.timestamp.tv_sec) * 1000000 +
                     static_cast<std::int64_t>(buf.timestamp.tv_usec))
                  : 0;
        return plugins::kOk;
    }

  private:
    int fd_ = -1;
    bool streaming_ = false;
    std::vector<MappedBuffer> buffers_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::int32_t stride_ = 0;
};

} // anonymous namespace

namespace detail {

plugins::IVideoCapture* create_camera_capture(plugins::CaptureConfig cfg, CameraBackend backend) {
    if (backend != CameraBackend::kV4l2) {
        // The other backends are not available on Linux.
        return nullptr;
    }
    auto source = std::make_unique<V4l2CameraSource>();
    CameraCaptureOptions opts;
    opts.backend = backend;
    opts.pixel_format = plugins::VideoPixelFormat::kNV12;
    return new CameraCapture(std::move(cfg), std::move(source), opts);
}

} // namespace detail

} // namespace nimrtc::video_capture

#endif // defined(__linux__)
