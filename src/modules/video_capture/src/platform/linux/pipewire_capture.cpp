// modules/video_capture/src/platform/linux/pipewire_capture.cpp
// ----------------------------------------------------------------------------
// The Linux PipeWire capture device (remote-desktop plan A5, the Wayland
// path).
//
// The adapter is *thin*: it asks the XDG Desktop Portal for a source,
// connects `libpipewire` to the node the portal handed back, and gives
// each buffer to the portable core.  The queue, the drop policy, the
// dirty-rect bookkeeping, the statistics and the negotiation all come
// from the portable core.
//
// ## Build-time guards
//
// The PipeWire backend needs `libpipewire-0.3` and `libspa-0.2`.  When
// they are not installed the whole file compiles to a stub that reports
// "unsupported" -- the build never fails for a missing optional
// dependency.
//
// ## The D-Bus handshake
//
// The portal is spoken to over D-Bus.  Rather than vendoring a D-Bus
// library, the adapter speaks the *portal* protocol directly over the
// session bus socket, using the `org.freedesktop.portal.DBus` helper
// (which is what `xdg-desktop-portal`'s own clients do):
//
//   - `org.freedesktop.DBus.Hello` on the session bus,
//   - `org.freedesktop.DBus.GetNameOwner` for
//     `org.freedesktop.portal.Desktop`,
//   - the `org.freedesktop.portal.ScreenCast` / `.RemoteDesktop`
//     methods on the `org.freedesktop.portal.Desktop` object.
//
// The frame the portal returns is an fd-passed `h` (a PipeWire fd) plus
// the node id in `o`.
// ----------------------------------------------------------------------------

#if defined(__linux__)

#    include "pipewire_capture.hpp"

#    include <cerrno>
#    include <chrono>
#    include <cstring>
#    include <poll.h>
#    include <string>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>
#    include <vector>

#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
#        include <pipewire/pipewire.h>
#        include <spa/param/video/format-utils.h>
#        include <spa/pod/builder.h>
#    endif

namespace nimrtc::video_capture {

namespace {

/** The device-id prefix the backend hands out.  The portal owns the
 *  source selection, so the ids are portal-session ids, not monitor
 *  indices. */
constexpr const char* kPortalPrefix = "pipewire-portal:";

/** The format tag the backend reports. */
constexpr const char* kFormatTag = "bgra-pipewire";

#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** True iff the host is running a Wayland session (and therefore
 *  *should* use the portal path). */
bool have_wayland_display() noexcept {
    const char* display = std::getenv("WAYLAND_DISPLAY");
    return display != nullptr && display[0] != '\0';
}

// ---------------------------------------------------------------------------
// A minimal D-Bus client
//
// The portal handshake is a handful of method calls.  Implementing them
// directly over the session bus socket keeps the dependency footprint at
// zero: no libdbus, no GIO.
// ---------------------------------------------------------------------------

/** A tiny D-Bus message builder / reader. */
class DBusMessage {
  public:
    /** Build a method call. */
    static DBusMessage method_call(const std::string& destination, const std::string& path,
                                   const std::string& interface, const std::string& member) {
        DBusMessage msg;
        msg.destination_ = destination;
        msg.path_ = path;
        msg.interface_ = interface;
        msg.member_ = member;
        msg.header_ = 0x01; // METHOD_CALL
        return msg;
    }

    /** Append a string argument (type 's'). */
    void append_string(const std::string& value) {
        align(4);
        const std::uint32_t len = static_cast<std::uint32_t>(value.size());
        body_.append(reinterpret_cast<const char*>(&len), 4);
        body_.append(value);
        body_.push_back('\0');
        align(4);
    }

    /** Append a uint32 argument (type 'u'). */
    void append_uint32(std::uint32_t value) {
        align(4);
        body_.append(reinterpret_cast<const char*>(&value), 4);
    }

    /** Append a boolean argument (type 'b'). */
    void append_boolean(bool value) {
        align(4);
        const std::uint32_t v = value ? 1u : 0u;
        body_.append(reinterpret_cast<const char*>(&v), 4);
    }

    /** Append an object-path argument (type 'o'). */
    void append_object_path(const std::string& value) { append_string(value); }

    /** Append a Unix-fd argument (type 'h'). */
    void append_fd(int fd) {
        align(4);
        body_.append(reinterpret_cast<const char*>(&fd), 4);
    }

    /** Append a variant wrapping the string 'u' (a uint32). */
    void append_variant_uint32(std::uint32_t value) {
        align(4);
        body_.push_back('u');
        align(4);
        body_.append(reinterpret_cast<const char*>(&value), 4);
    }

    /** Append a variant wrapping the string 's' (a string). */
    void append_variant_string(const std::string& value) {
        align(4);
        body_.push_back('s');
        align(4);
        const std::uint32_t len = static_cast<std::uint32_t>(value.size());
        body_.append(reinterpret_cast<const char*>(&len), 4);
        body_.append(value);
        body_.push_back('\0');
        align(4);
    }

    /** The serialised message.
     *
     * Not `const`: every message on the bus needs a *unique* serial, so
     * serialising advances the counter.
     */
    [[nodiscard]] std::vector<char> serialise() {
        // The header is fixed-size (the fields the portal needs are
        // strings, so the header is 12 bytes + the string fields).
        struct Field {
            char code;
            std::string value;
        };
        const std::vector<Field> fields = {
            {'o', path_},
            {'s', interface_},
            {'s', member_},
            {'s', destination_},
        };

        std::size_t header_len = 12;
        for (const auto& f : fields) {
            header_len += 1 + 4 + f.value.size() + 1;
            header_len = (header_len + 3) & ~std::size_t{3};
        }

        std::vector<char> out;
        out.resize(16);
        out[0] = static_cast<char>(header_); // little endian
        out[1] = static_cast<char>(header_);
        out[2] = static_cast<char>(header_);
        out[3] = static_cast<char>(header_);
        out[4] = static_cast<char>(body_.size() & 0xFF);
        out[5] = static_cast<char>((body_.size() >> 8) & 0xFF);
        out[6] = static_cast<char>((body_.size() >> 16) & 0xFF);
        out[7] = static_cast<char>((body_.size() >> 24) & 0xFF);
        out[8] = static_cast<char>(serial_ & 0xFF);
        out[9] = static_cast<char>((serial_ >> 8) & 0xFF);
        out[10] = static_cast<char>((serial_ >> 16) & 0xFF);
        out[11] = static_cast<char>((serial_ >> 24) & 0xFF);

        std::vector<char> fields_blob;
        for (const auto& f : fields) {
            fields_blob.push_back(f.code);
            const std::uint32_t len = static_cast<std::uint32_t>(f.value.size());
            const auto* len_bytes = reinterpret_cast<const char*>(&len);
            fields_blob.insert(fields_blob.end(), len_bytes, len_bytes + 4);
            fields_blob.insert(fields_blob.end(), f.value.begin(), f.value.end());
            fields_blob.push_back('\0');
            while (fields_blob.size() % 4 != 0)
                fields_blob.push_back('\0');
        }

        std::vector<char> header(fields_blob);
        const std::uint32_t fields_len = static_cast<std::uint32_t>(header.size());
        std::size_t offset = 12;
        const auto* fields_len_bytes = reinterpret_cast<const char*>(&fields_len);
        header[offset] = fields_len_bytes[0];
        header[offset + 1] = fields_len_bytes[1];
        header[offset + 2] = fields_len_bytes[2];
        header[offset + 3] = fields_len_bytes[3];

        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), body_.begin(), body_.end());
        ++serial_;
        return out;
    }

  private:
    void align(std::size_t n) {
        while (body_.size() % n != 0)
            body_.push_back('\0');
    }

    std::uint8_t header_ = 0;
    std::uint32_t serial_ = 1;
    std::string destination_;
    std::string path_;
    std::string interface_;
    std::string member_;
    std::string body_;
};

/** A D-Bus connection to the session bus. */
class DBusConnection {
  public:
    DBusConnection() = default;
    ~DBusConnection() { disconnect(); }

    DBusConnection(const DBusConnection&) = delete;
    DBusConnection& operator=(const DBusConnection&) = delete;

    /** Connect to the session bus. */
    bool connect() noexcept {
        const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        if (address == nullptr || address[0] == '\0')
            return false;

        // The session bus is addressed as `unix:path=/run/user/1000/bus`
        // or `unix:abstract=/tmp/dbus-XXXX`.
        const char* key = std::strstr(address, "unix:");
        if (key == nullptr)
            return false;

        const char* path_key = std::strstr(key, "path=");
        const char* abstract_key = std::strstr(key, "abstract=");

        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0)
            return false;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        socklen_t len = 0;

        if (path_key != nullptr) {
            const char* start = path_key + 5;
            const char* end = std::strchr(start, ',');
            const std::size_t n =
                (end != nullptr) ? static_cast<std::size_t>(end - start) : std::strlen(start);
            if (n >= sizeof(addr.sun_path))
                return false;
            std::memcpy(addr.sun_path, start, n);
            len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + n + 1);
        } else if (abstract_key != nullptr) {
            const char* start = abstract_key + 9;
            const char* end = std::strchr(start, ',');
            const std::size_t n =
                (end != nullptr) ? static_cast<std::size_t>(end - start) : std::strlen(start);
            if (n + 1 >= sizeof(addr.sun_path))
                return false;
            // The abstract namespace is a leading NUL.
            addr.sun_path[0] = '\0';
            std::memcpy(addr.sun_path + 1, start, n);
            len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);
        } else {
            return false;
        }

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Say hello, so the bus assigns us a name.
        DBusMessage hello = DBusMessage::method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello");
        if (!call(hello))
            return false;
        return true;
    }

    void disconnect() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /**
     * @brief Call a method and wait for the reply.
     *
     * The reply is parsed for the two things the portal handshake needs:
     * a string (the session handle / the node id) and a file descriptor
     * (the PipeWire fd).
     */
    bool call(DBusMessage& msg, std::string* out_string = nullptr,
              int* out_fd = nullptr) noexcept {
        if (fd_ < 0)
            return false;

        const std::vector<char> bytes = msg.serialise();
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
            if (n <= 0)
                return false;
            sent += static_cast<std::size_t>(n);
        }

        // Read the reply.  The reply carries the fd, so `recvmsg` is
        // required (a plain `recv` would drop it).
        struct msghdr recv_msg{};
        char buffer[4096];
        iovec iov{};
        iov.iov_base = buffer;
        iov.iov_len = sizeof(buffer);
        recv_msg.msg_iov = &iov;
        recv_msg.msg_iovlen = 1;
        char control[CMSG_SPACE(sizeof(int))];

        recv_msg.msg_control = control;
        recv_msg.msg_controllen = sizeof(control);

        const ssize_t n = ::recvmsg(fd_, &recv_msg, 0);
        if (n <= 0)
            return false;

        // Pull the fd out of the control data.
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&recv_msg); cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&recv_msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                if (out_fd != nullptr) {
                    int fd = -1;
                    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
                    *out_fd = fd;
                }
            }
        }

        if (out_string != nullptr) {
            // The reply body is a variant; the string follows the
            // variant signature.  A full D-Bus parser is overkill here:
            // the portal's replies are a single string, so the first
            // printable run after the signature is the value.
            const char* begin = buffer;
            const char* end = buffer + n;
            bool in_string = false;
            std::string value;
            for (const char* p = begin; p < end; ++p) {
                if (!in_string) {
                    if (*p == 's' && p + 1 < end && *(p + 1) == '\0') {
                        in_string = true;
                    }
                    continue;
                }
                if (*p == '\0')
                    break;
                value.push_back(*p);
            }
            *out_string = value;
        }
        return true;
    }

  private:
    int fd_ = -1;
};

#    endif // NIMRTC_HAS_PIPEWIRE_CAPTURE

} // anonymous namespace

// ---------------------------------------------------------------------------
// PipeWireCapture::Impl
// ---------------------------------------------------------------------------

class PipeWireCapture::Impl {
  public:
    Impl(plugins::CaptureConfig cfg, UnixCaptureOptions opts) : cfg_(std::move(cfg)), opts_(opts) {}

    ~Impl() { close_device(); }

    // ---- Enumeration ------------------------------------------------------

    std::vector<plugins::CaptureDevice> enumerate_devices() const noexcept {
        std::vector<plugins::CaptureDevice> devices;

#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
        // The portal owns the source selection: the engine cannot
        // enumerate monitors it was not given.  What *is* known before
        // the user picks anything is the virtual desktop that the
        // compositor would offer, so the backend advertises that as a
        // single, portal-negotiated source.
        plugins::CaptureDevice device;
        device.id = std::string(kPortalPrefix) + "default";
        device.name = "Wayland source (chosen by the XDG Desktop Portal)";
        device.kind = plugins::CaptureDeviceKind::kScreen;
        device.max_width = 0;
        device.max_height = 0;
        device.max_fps = 0;
        devices.push_back(std::move(device));
#    else
        (void)devices;
#    endif
        return devices;
    }

    std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept {
        std::vector<plugins::CaptureFormat> formats;
        (void)device_id;

#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
        // The portal negotiates the format with the compositor, so the
        // backend offers the common ladder rungs and lets the portal
        // clamp.  `zero_copy` is false: the frames arrive through
        // `libpipewire`, which hands out a CPU-visible buffer.
        const std::uint32_t sizes[][2] = {
            {1280, 720},
            {1920, 1080},
        };
        for (const auto& size : sizes) {
            plugins::CaptureFormat format;
            format.width = size[0];
            format.height = size[1];
            format.fps = 30;
            format.tag = kFormatTag;
            format.zero_copy = false;
            format.has_dirty_rects = opts_.enable_pixel_diff;
            formats.push_back(std::move(format));
        }
#    endif
        return formats;
    }

    // ---- Lifecycle --------------------------------------------------------

    plugins::Status open_device(UnixCaptureError& error) noexcept {
#    if !defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
        error = UnixCaptureError::kUnsupportedFormat;
        return plugins::kErrUnsupported;
#    else
        if (!have_wayland_display()) {
            // The PipeWire path is the *Wayland* path.  On an X11
            // session the caller should use the X11 backend instead.
            error = UnixCaptureError::kDisplayUnavailable;
            return plugins::kErrHardwareError;
        }

        // ---- The portal handshake ---------------------------------------
        //
        // 1. `CreateSession` on the Desktop portal.
        // 2. `SelectSources` on the session.
        // 3. `Start` on the session -- the portal answers with the
        //    PipeWire fd and the node id.
        DBusConnection bus;
        if (!bus.connect()) {
            error = UnixCaptureError::kPortalUnavailable;
            return plugins::kErrHardwareError;
        }

        DBusMessage create = DBusMessage::method_call(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Desktop", "CreateSession");
        create.append_string("org.freedesktop.portal.ScreenCast");
        create.append_variant_string("NimRTC screen capture");

        std::string session_handle;
        if (!bus.call(create, &session_handle) || session_handle.empty()) {
            error = UnixCaptureError::kPortalUnavailable;
            return plugins::kErrHardwareError;
        }

        const std::string session_path =
            "/org/freedesktop/portal/desktop/request/" + session_handle + "/session";

        // `SelectSources` -- the user picks the source here.  `types = 1`
        // asks for a monitor, `types = 2` for a window.
        DBusMessage select =
            DBusMessage::method_call("org.freedesktop.portal.Desktop", session_path,
                                     "org.freedesktop.portal.ScreenCast", "SelectSources");
        select.append_string(session_handle);
        select.append_variant_uint32(1); // monitor
        select.append_variant_uint32(0); // multiple = false
        select.append_string("NimRTC");  // app id
        select.append_string("");        // source id (empty = any)

        if (!bus.call(select)) {
            error = UnixCaptureError::kPermissionDenied;
            return plugins::kErrHardwareError;
        }

        // `Start` -- the portal answers with the PipeWire fd and the
        // node id.
        DBusMessage start = DBusMessage::method_call("org.freedesktop.portal.Desktop", session_path,
                                                     "org.freedesktop.portal.ScreenCast", "Start");
        start.append_string(session_handle);
        start.append_string(""); // parent_window = ""

        int pipewire_fd = -1;
        std::string node_string;
        if (!bus.call(start, &node_string, &pipewire_fd) || pipewire_fd < 0) {
            error = UnixCaptureError::kPermissionDenied;
            return plugins::kErrHardwareError;
        }

        node_id_ = 0;
        try {
            const unsigned long parsed = std::stoul(node_string);
            node_id_ = static_cast<std::uint32_t>(parsed);
        } catch (const std::exception&) {
            node_id_ = 0;
        }

        // ---- The PipeWire connection ------------------------------------
        pw_init(nullptr, nullptr);

        pw_loop_ = pw_loop_new(nullptr);
        if (pw_loop_ == nullptr) {
            ::close(pipewire_fd);
            error = UnixCaptureError::kCaptureFailed;
            return plugins::kErrHardwareError;
        }

        pw_context_ = pw_context_new(pw_loop_, nullptr, 0);
        if (pw_context_ == nullptr) {
            pw_loop_destroy(pw_loop_);
            pw_loop_ = nullptr;
            ::close(pipewire_fd);
            error = UnixCaptureError::kCaptureFailed;
            return plugins::kErrHardwareError;
        }

        // The portal handed back a connected PipeWire socket; the
        // context takes that fd over (`pw_context_connect_fd` closes it
        // on disconnect).
        pw_core_ = pw_context_connect_fd(pw_context_, pipewire_fd, nullptr, 0);
        if (pw_core_ == nullptr) {
            pw_context_destroy(pw_context_);
            pw_context_ = nullptr;
            pw_loop_destroy(pw_loop_);
            pw_loop_ = nullptr;
            error = UnixCaptureError::kCaptureFailed;
            return plugins::kErrHardwareError;
        }

        // The stream: a video stream that only *reads* the node.
        //
        // `pw_stream_new()` takes the stream's *properties*, not a
        // media type and not the stream flags -- the flags belong to
        // `pw_stream_connect()`.
        pw_properties* stream_props = pw_properties_new(
            nullptr, nullptr);
        if (stream_props != nullptr) {
            pw_properties_set(stream_props, PW_KEY_MEDIA_TYPE, "Video");
            pw_properties_set(stream_props, PW_KEY_MEDIA_CATEGORY, "Capture");
            pw_properties_set(stream_props, PW_KEY_MEDIA_ROLE, "Screen");
        }
        pw_stream_ = pw_stream_new(pw_core_, "NimRTC screen capture", stream_props);
        if (pw_stream_ == nullptr) {
            close_device();
            error = UnixCaptureError::kCaptureFailed;
            return plugins::kErrHardwareError;
        }

        // The listener is a *mutable* copy of the events table: the
        // `process` callback is filled in per instance, so it cannot be
        // `static`.
        stream_events_.version = PW_VERSION_STREAM_EVENTS;
        stream_events_.process = &Impl::on_stream_process;
        stream_events_.param_changed = &Impl::on_stream_param_changed;
        pw_stream_add_listener(pw_stream_, &stream_hook_, &stream_events_, this);

        // ---- The format we ask for --------------------------------------
        //
        // The stream has to *offer* a format; PipeWire then negotiates
        // one.  The offer is built as a `SPA_PARAM_Format` pod
        // describing a raw BGRA video frame.
        std::uint8_t pod_buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        spa_video_info_raw raw_info = {};
        raw_info.format = SPA_VIDEO_FORMAT_BGRA;
        // `spa_rectangle::width` / `::height` are `uint32_t`.
        raw_info.size.width = cfg_.format.width != 0 ? cfg_.format.width : 1920u;
        raw_info.size.height = cfg_.format.height != 0 ? cfg_.format.height : 1080u;
        raw_info.framerate.num = 30;
        raw_info.framerate.denom = 1;
        spa_pod* format_pod = spa_format_video_raw_build(
            &builder, SPA_PARAM_Format, &raw_info);
        if (format_pod == nullptr) {
            close_device();
            error = UnixCaptureError::kCaptureFailed;
            return plugins::kErrHardwareError;
        }
        const spa_pod* params[] = { format_pod };

        const int rc = pw_stream_connect(pw_stream_, PW_DIRECTION_INPUT, node_id_,
                                         PW_STREAM_FLAG_DONT_RECONNECT, params, 1);
        if (rc < 0) {
            close_device();
            error = UnixCaptureError::kCaptureFailed;
            return plugins::kErrHardwareError;
        }

        started_ = true;

        plugins::CaptureFormat negotiated;
        negotiated.width = cfg_.format.width != 0 ? cfg_.format.width : 1920u;
        negotiated.height = cfg_.format.height != 0 ? cfg_.format.height : 1080u;
        negotiated.fps = cfg_.format.fps != 0 ? cfg_.format.fps : 30u;
        negotiated.tag = kFormatTag;
        negotiated.zero_copy = false;
        negotiated.has_dirty_rects = opts_.enable_pixel_diff;
        negotiated_format_ = negotiated;

        return plugins::kOk;
#    endif
    }

    void close_device() noexcept {
#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
        if (pw_stream_ != nullptr) {
            pw_stream_destroy(pw_stream_);
            pw_stream_ = nullptr;
        }
        if (pw_core_ != nullptr) {
            pw_core_disconnect(pw_core_);
            pw_core_ = nullptr;
        }
        if (pw_context_ != nullptr) {
            pw_context_destroy(pw_context_);
            pw_context_ = nullptr;
        }
        if (pw_loop_ != nullptr) {
            pw_loop_destroy(pw_loop_);
            pw_loop_ = nullptr;
        }
        started_ = false;
#    endif
    }

    void start() noexcept {
#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
        // The PipeWire stream is already connected by `open_device()`;
        // the capture thread only has to pump the loop.
#    endif
    }

    void stop() noexcept {
#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
        // The stream is torn down by `close_device()`.
#    endif
    }

    plugins::Status poll_frame() noexcept {
#    if !defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
        return plugins::kErrUnsupported;
#    else
        if (pw_loop_ == nullptr)
            return plugins::kErrHardwareError;

        // Pump the PipeWire loop: the buffers arrive on its own
        // thread, and the loop dispatches them to the stream's
        // `process` callback, which stores them in `latest_`.
        pw_loop_iterate(pw_loop_, 0);

        if (failed_)
            return plugins::kErrHardwareError;

        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (!take_latest(pixels, width, height)) {
            return plugins::kErrNotReady;
        }

        // The frame was copied out of the PipeWire buffer, so the copy
        // is counted.
        core_ptr_->add_bytes_copied(static_cast<std::uint64_t>(pixels.size()));

        return core_ptr_->submit_frame(pixels.data(), pixels.size(),
                                       static_cast<std::int32_t>(width * 4u), width, height,
                                       now_us(), nullptr);
#    endif
    }

    plugins::CaptureCapabilities capabilities() const noexcept {
        plugins::CaptureCapabilities caps;
        caps.zero_copy = false;
        caps.dirty_rects = opts_.enable_pixel_diff;
        // The compositor draws the cursor into the frame.
        caps.cursor_composited = true;
        caps.region_selection = true;
        // The portal owns the source selection: the engine does not
        // enumerate monitors.  The list is whatever the portal granted.
        caps.enumeration = false;
        caps.max_pixels = 0;
        caps.max_fps = 0;
        caps.backend = "linux-pipewire";
        return caps;
    }

    [[nodiscard]] const plugins::CaptureFormat& negotiated_format() const noexcept {
        return negotiated_format_;
    }

    void set_core(UnixCaptureCore* core) noexcept { core_ptr_ = core; }

  private:
#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
    /** The PipeWire stream callback: it copies the buffer out and keeps
     *  the latest one. */
    static void on_stream_process(void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || self->pw_stream_ == nullptr)
            return;

        // For a capture stream `pw_stream_dequeue_buffer()` hands back
        // the *one* buffer that is ready; there is no array and no
        // count.
        pw_buffer* buffer = pw_stream_dequeue_buffer(self->pw_stream_);
        if (buffer == nullptr)
            return;

        spa_buffer* sbuf = buffer->buffer;
        if (sbuf == nullptr || sbuf->n_datas == 0)
            return;

        spa_data* data = &sbuf->datas[0];
        if (data->data == nullptr || data->chunk == nullptr)
            return;

        // The *size* of the frame comes from the chunk, and the stride
        // from the chunk too -- `spa_data` has no `info` member.  The
        // frame's width and height come from the negotiated format,
        // which PipeWire reports through the `param_changed` event.
        const std::uint32_t stride =
            data->chunk->stride > 0 ? static_cast<std::uint32_t>(data->chunk->stride) : 0u;
        const std::uint32_t height =
            data->chunk->size > 0 && stride > 0
                ? static_cast<std::uint32_t>(data->chunk->size) / stride
                : 0u;
        const std::uint32_t width = self->frame_width_;
        if (width == 0 || height == 0 || stride == 0)
            return;

        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(stride) * static_cast<std::size_t>(height));
        const std::size_t copy = pixels.size() < data->chunk->size
            ? pixels.size()
            : static_cast<std::size_t>(data->chunk->size);
        std::memcpy(pixels.data(), data->data, copy);

        std::lock_guard<std::mutex> lock(self->mutex_);
        self->latest_ = std::move(pixels);
        self->latest_width_ = width;
        self->latest_height_ = height;
    }

    /** PipeWire's `param_changed` event: the format was negotiated. */
    static void on_stream_param_changed(void* userdata, std::uint32_t id,
                                        const spa_pod* param) {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || id != SPA_PARAM_Format || param == nullptr)
            return;

        spa_video_info_raw info = {};
        if (spa_format_video_raw_parse(param, &info) != 0)
            return;

        const std::uint32_t width =
            info.size.width > 0 ? static_cast<std::uint32_t>(info.size.width) : 0u;
        const std::uint32_t height =
            info.size.height > 0 ? static_cast<std::uint32_t>(info.size.height) : 0u;
        if (width == 0 || height == 0)
            return;

        std::lock_guard<std::mutex> lock(self->mutex_);
        self->frame_width_ = width;
        self->frame_height_ = height;
    }

    bool take_latest(std::vector<std::uint8_t>& out, std::uint32_t& width,
                     std::uint32_t& height) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_.empty())
            return false;
        out = std::move(latest_);
        latest_.clear();
        width = latest_width_;
        height = latest_height_;
        return true;
    }
#    endif

    plugins::CaptureConfig cfg_;
    UnixCaptureOptions opts_;
    plugins::CaptureFormat negotiated_format_{};
    std::uint32_t node_id_ = 0;
    bool started_ = false;
    bool failed_ = false;
    UnixCaptureCore* core_ptr_ = nullptr;

#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
    std::mutex mutex_;
    std::vector<std::uint8_t> latest_;
    std::uint32_t latest_width_ = 0;
    std::uint32_t latest_height_ = 0;

    /** The negotiated frame size (from PipeWire's `param_changed`). */
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;

    pw_loop* pw_loop_ = nullptr;
    pw_context* pw_context_ = nullptr;
    pw_core* pw_core_ = nullptr;
    pw_stream* pw_stream_ = nullptr;

    /** The listener hook and the events table for the stream. */
    spa_hook stream_hook_{};
    pw_stream_events stream_events_{};
#    endif
};

// ---------------------------------------------------------------------------
// PipeWireCapture
// ---------------------------------------------------------------------------

PipeWireCapture::PipeWireCapture(plugins::CaptureConfig cfg, UnixCaptureOptions opts) noexcept
    : UnixCaptureDevice(cfg, opts) {
    impl_ = std::make_unique<Impl>(std::move(cfg), opts);
    impl_->set_core(&core());
}

PipeWireCapture::~PipeWireCapture() = default;

std::vector<plugins::CaptureDevice> PipeWireCapture::platform_enumerate_devices() const noexcept {
    return impl_ ? impl_->enumerate_devices() : std::vector<plugins::CaptureDevice>{};
}

std::vector<plugins::CaptureFormat>
PipeWireCapture::platform_supported_formats(const std::string& device_id) const noexcept {
    return impl_ ? impl_->supported_formats(device_id) : std::vector<plugins::CaptureFormat>{};
}

plugins::Status PipeWireCapture::platform_open_device(UnixCaptureError& error) noexcept {
    if (!impl_) {
        error = UnixCaptureError::kCaptureFailed;
        return plugins::kErrInternal;
    }
    const plugins::Status status = impl_->open_device(error);
    if (status == plugins::kOk) {
        set_negotiated_format(impl_->negotiated_format());
    }
    return status;
}

void PipeWireCapture::platform_close_device() noexcept {
    if (impl_)
        impl_->close_device();
}

plugins::CaptureCapabilities PipeWireCapture::platform_capabilities() const noexcept {
    return impl_ ? impl_->capabilities() : plugins::CaptureCapabilities{};
}

void PipeWireCapture::platform_start() noexcept {
    if (impl_)
        impl_->start();
}

void PipeWireCapture::platform_stop() noexcept {
    if (impl_)
        impl_->stop();
}

plugins::Status PipeWireCapture::platform_poll_frame() noexcept {
    if (!impl_)
        return plugins::kErrNotReady;
    return impl_->poll_frame();
}

} // namespace nimrtc::video_capture

#endif // defined(__linux__)
