// modules/video_capture/src/platform/portal_capture.cpp
// ----------------------------------------------------------------------------
// The XDG Desktop Portal + PipeWire capture device (remote-desktop
// plan A5).
//
// This is the Wayland capture path on Linux.  It is two halves:
//
//   1. The **portal** half (D-Bus): asks the user to pick a source and
//      gets a PipeWire node id back.
//   2. The **PipeWire** half: connects a capture stream to that node
//      and pulls the frames out of it.
//
// It is a thin adapter over the portable capture layer: it fills the
// shared bounded queue with BGRA frames and does nothing else.
//
// See include/nimrtc/video_capture/portal_capture.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/portal_capture.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#if defined(__linux__) && defined(NIMRTC_HAVE_GIO) && \
    defined(NIMRTC_HAVE_PIPEWIRE)
#  define NIMRTC_PORTAL_CAPTURE 1
#  include <gio/gio.h>
#  include <pipewire/pipewire.h>
#  include <spa/param/format.h>
#  include <spa/param/param.h>
#  include <spa/param/video/format-utils.h>
#  include <spa/param/video/raw.h>
#  include <spa/pod/builder.h>
#  include <spa/pod/parser.h>
#endif

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>

namespace nimrtc::video_capture {

#if defined(NIMRTC_PORTAL_CAPTURE)

namespace {

/** The portal's well-known bus name. */
constexpr const char* kPortalBusName = "org.freedesktop.portal.Desktop";
/** The portal's object path. */
constexpr const char* kPortalObjectPath = "/org/freedesktop/portal/desktop";
/** The `ScreenCast` interface. */
constexpr const char* kScreenCastIface = "org.freedesktop.portal.ScreenCast";

/** The PipeWire node id the portal handed back.  Zero means "none". */
constexpr std::uint32_t kNoNodeId = 0;

/**
 * @brief The state a `Response` signal carries.
 *
 * The portal answers a `CreateSession` / `Start` request with a
 * `Response` signal whose payload is `(u response, a{sv} results)`.
 * The `results` dictionary for a `ScreenCast` holds a `streams` entry:
 * an array of `a{sv}`, each with a `node-id` (a *string*, because the
 * portal's API types everything as strings) and a `properties` entry.
 */
struct PortalResponse {
    /** True once a response has arrived. */
    bool answered = false;
    /** The portal's response code: 0 = success, 1 = the user
     *  cancelled, 2 = the other end closed. */
    std::uint32_t code = 0;
    /** The PipeWire node id of the granted source. */
    std::uint32_t node_id = kNoNodeId;
    /** The granted source's size, when the portal reported one. */
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /** The granted source's position on the virtual desktop. */
    std::int32_t x = 0;
    std::int32_t y = 0;
};

/** Read a `string` out of a `GVariant` dictionary entry. */
std::string dict_get_string(GVariant* dict, const char* key) {
    if (dict == nullptr) return {};
    GVariant* value = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING);
    if (value == nullptr) return {};
    const char* text = g_variant_get_string(value, nullptr);
    std::string out = text != nullptr ? text : "";
    g_variant_unref(value);
    return out;
}

/** The `Response` signal handler.
 *
 * It is a plain C function (GIO's C API), so the user data carries the
 * `PortalResponse` the caller is waiting for.
 */
void on_portal_response(GDBusConnection* /*connection*/,
                        const char* /*sender*/,
                        const char* /*path*/,
                        const char* /*interface*/,
                        const char* /*signal*/,
                        GVariant* parameters,
                        gpointer user_data) {
    auto* response = static_cast<PortalResponse*>(user_data);
    if (response == nullptr || parameters == nullptr) return;

    std::uint32_t code = 0;
    GVariant* results = nullptr;
    g_variant_get(parameters, "(u@a{sv})", &code, &results);
    response->code = code;

    if (code == 0 && results != nullptr) {
        // The `streams` entry is an array of dictionaries.
        GVariant* streams =
            g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("aa{sv}"));
        if (streams != nullptr) {
            GVariantIter iter;
            GVariant* item = nullptr;
            g_variant_iter_init(&iter, streams);
            while (g_variant_iter_next(&iter, "a{sv}", &item)) {
                const std::string node_text = dict_get_string(item, "node-id");
                if (!node_text.empty()) {
                    try {
                        const unsigned long value = std::stoul(node_text);
                        if (value != 0) {
                            response->node_id = static_cast<std::uint32_t>(value);
                        }
                    } catch (...) {
                        // Not a number: leave the node id at zero.
                    }
                }

                // The `properties` entry describes the granted source's
                // geometry.
                GVariant* props =
                    g_variant_lookup_value(item, "properties",
                                           G_VARIANT_TYPE_VARDICT);
                if (props != nullptr) {
                    GVariant* size =
                        g_variant_lookup_value(props, "size",
                                               G_VARIANT_TYPE("(ii)"));
                    if (size != nullptr) {
                        gint32 w = 0;
                        gint32 h = 0;
                        g_variant_get(size, "(ii)", &w, &h);
                        if (w > 0) response->width = static_cast<std::uint32_t>(w);
                        if (h > 0) response->height = static_cast<std::uint32_t>(h);
                        g_variant_unref(size);
                    }
                    GVariant* position =
                        g_variant_lookup_value(props, "position",
                                               G_VARIANT_TYPE("(ii)"));
                    if (position != nullptr) {
                        gint32 px = 0;
                        gint32 py = 0;
                        g_variant_get(position, "(ii)", &px, &py);
                        response->x = px;
                        response->y = py;
                        g_variant_unref(position);
                    }
                    g_variant_unref(props);
                }
                g_variant_unref(item);
            }
            g_variant_unref(streams);
        }
    }

    if (results != nullptr) g_variant_unref(results);
    response->answered = true;
}

/**
 * @brief Ask the portal for a screen-cast source and wait for the
 *        answer.
 *
 * The portal shows the user a picker (a monitor, a window, a region)
 * and answers asynchronously with a `Response` signal.  This function
 * subscribes to that signal and pumps the main context until the answer
 * arrives.
 *
 * A *denial* — the user dismissing the dialog, or the portal refusing
 * outright — is reported as a failure, never as a crash: that is the
 * plan's rule for the macOS permission case, and the same rule applies
 * to the portal.
 *
 * @return true iff the user granted a source.
 */
bool request_screen_cast(PortalResponse& out) {
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (bus == nullptr) {
        core::log::Logger::instance().error(
            "xdg-portal: the session bus is not reachable");
        if (error != nullptr) g_error_free(error);
        return false;
    }

    // ---- Create the session --------------------------------------------------
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    // `persist_mode = "transient"` is the default for a screen cast:
    // the permission does not survive the session, which is the right
    // default for a remote-desktop *agent* (the user is told every
    // time).  `types` is left unset so the portal offers every source
    // kind — the *user* picks, which is the whole point.
    g_variant_builder_add(&options, "{sv}", "persist_mode",
                           g_variant_new_string("transient"));

    GVariant* reply = g_dbus_connection_call_sync(
        bus,
        kPortalBusName,
        kPortalObjectPath,
        kScreenCastIface,
        "CreateSession",
        g_variant_new("(a{sv}sa{sv})", &options, "", nullptr),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, // no timeout — the user may take a while
        nullptr,
        &error);

    if (reply == nullptr) {
        core::log::Logger::instance().error(
            "xdg-portal: the ScreenCast session request failed");
        if (error != nullptr) {
            core::log::Logger::instance().error(
                std::string("xdg-portal: ") + error->message);
            g_error_free(error);
        }
        g_object_unref(bus);
        return false;
    }

    const char* session_path = nullptr;
    g_variant_get(reply, "(&o)", &session_path);
    const std::string session = session_path != nullptr ? session_path : "";
    g_variant_unref(reply);

    // ---- Subscribe to the Response signal ---------------------------------
    //
    // The signal is emitted *on the session path*, so the subscription
    // names that path: subscribing to the whole interface would also
    // pick up the responses of every *other* portal session on the bus.
    //
    // The `Response` signal carries the request handle, which the
    // portal chooses, so the handler cannot filter by handle; the
    // session path is what distinguishes one request from another.
    const guint subscription = g_dbus_connection_signal_subscribe(
        bus,
        kPortalBusName,
        kScreenCastIface,
        "Response",
        session.c_str(),
        nullptr, // any argument 0
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_portal_response,
        &out,
        nullptr);

    // ---- Start the session ------------------------------------------------------
    GVariant* start_reply = g_dbus_connection_call_sync(
        bus,
        kPortalBusName,
        session.c_str(),
        kScreenCastIface,
        "Start",
        g_variant_new("(sa{sv}sa{sv})", session.c_str(), nullptr, "", nullptr),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);

    if (start_reply == nullptr) {
        core::log::Logger::instance().error(
            "xdg-portal: the ScreenCast session could not be started");
        if (error != nullptr) {
            core::log::Logger::instance().error(
                std::string("xdg-portal: ") + error->message);
            g_error_free(error);
        }
        g_dbus_connection_signal_unsubscribe(bus, subscription);
        g_object_unref(bus);
        return false;
    }
    g_variant_unref(start_reply);

    // ---- Wait for the answer ------------------------------------------------------
    //
    // The main context is pumped until the handler sets `answered`.
    // `g_main_context_iteration` with a timeout keeps this from
    // spinning forever if the portal never answers.
    while (!out.answered) {
        if (!g_main_context_iteration(nullptr, TRUE)) {
            break;
        }
    }

    g_dbus_connection_signal_unsubscribe(bus, subscription);
    g_object_unref(bus);

    if (!out.answered || out.code != 0 || out.node_id == kNoNodeId) {
        core::log::Logger::instance().error(
            "xdg-portal: the portal did not grant a usable source");
        return false;
    }
    return true;
}

/** True iff the SPA format is a packed 32-bit one the backend can
 *  convert to BGRA without a colour-space conversion. */
bool spa_format_is_packed32(spa_video_format format) {
    switch (format) {
        case SPA_VIDEO_FORMAT_BGRA:
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_RGBA:
        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_xRGB:
        case SPA_VIDEO_FORMAT_BGR:
        case SPA_VIDEO_FORMAT_RGB:
            return true;
        default:
            return false;
    }
}

/** True iff the packed format is already in BGRA byte order. */
bool spa_format_is_bgra_order(spa_video_format format) {
    return format == SPA_VIDEO_FORMAT_BGRA ||
           format == SPA_VIDEO_FORMAT_BGRx ||
           format == SPA_VIDEO_FORMAT_BGR;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The platform state (PIMPL)
// ---------------------------------------------------------------------------

/**
 * @brief The portal / PipeWire objects the device owns.
 *
 * Kept out of the public header so that the header stays free of the
 * PipeWire and GIO headers (and so that the class exists on every
 * platform).
 */
class PortalCaptureState {
public:
    /** The PipeWire node id the portal granted. */
    std::uint32_t node_id = kNoNodeId;

    /** The negotiated stream's size. */
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    /**
     * @brief The negotiated pixel format.
     *
     * PipeWire reports the format it settled on through the stream's
     * `param_changed` event, *not* through the buffers: a buffer's
     * `datas[0].type` is the memory type (Mem / DmaBuf), not the
     * layout.  So the format is captured once, when the stream is
     * configured, and every frame is then read against it.
     */
    spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN;
    bool have_format = false;

    /** The PipeWire stream.  Typed as `void*` so that this header (and
     *  the public one) stay free of the PipeWire headers. */
    void* stream = nullptr;

    /** The bounded frame queue the capture thread fills. */
    CaptureQueue queue_{2};

    /** The capture thread. */
    std::thread thread_;

    /** Set to false to ask the capture thread to stop. */
    std::atomic<bool> running_{false};

    ~PortalCaptureState() {
        stop_thread();
    }

    void stop_thread() noexcept {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }
};

// ---------------------------------------------------------------------------
// PortalCapture
// ---------------------------------------------------------------------------

PortalCapture::PortalCapture(plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

PortalCapture::~PortalCapture() {
    close();
}

bool PortalCapture::available() noexcept {
    return true;
}

bool PortalCapture::portal_available() noexcept {
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (bus == nullptr) {
        if (error != nullptr) g_error_free(error);
        return false;
    }

    // The portal must be *registered* on the bus, not merely reachable:
    // a session bus without the portal service is the normal state of
    // a plain X11 session, and the host needs to know that so it can
    // fall back to the X11 backend.
    GVariant* reply = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameHasOwner",
        g_variant_new("(s)", kPortalBusName),
        G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);

    bool owned = false;
    if (reply != nullptr) {
        g_variant_get(reply, "(b)", &owned);
        g_variant_unref(reply);
    } else if (error != nullptr) {
        g_error_free(error);
    }
    g_object_unref(bus);
    return owned;
}

plugins::Status PortalCapture::open() noexcept {
    if (open_) return plugins::kOk;

    if (!portal_available()) {
        core::log::Logger::instance().error(
            "xdg-portal: no desktop portal on this session");
        return plugins::kErrUnsupported;
    }

    // ---- Ask the user -------------------------------------------------------
    PortalResponse response;
    if (!request_screen_cast(response)) {
        // The user dismissed the picker, or the portal refused.  That is
        // a clean error, not a crash.
        return plugins::kErrNotReady;
    }

    // ---- Negotiate the format -------------------------------------------------
    cfg_.device_id = "portal:" + std::to_string(response.node_id);
    if (cfg_.format.width == 0) {
        cfg_.format.width = response.width != 0 ? response.width : 1920u;
    }
    if (cfg_.format.height == 0) {
        cfg_.format.height = response.height != 0 ? response.height : 1080u;
    }
    if (cfg_.format.fps == 0) cfg_.format.fps = 30;
    cfg_.format.tag = "bgra";
    // The frames come out of a shared-memory buffer, so the CPU path
    // needs a copy; the *zero-copy* hand-off (a DMABUF straight into
    // the encoder) is plan item B10.
    cfg_.format.zero_copy = false;
    // PipeWire does not report per-frame damage to the client.
    cfg_.format.has_dirty_rects = false;

    description_ = cfg_.device_id;

    // ---- The state -------------------------------------------------------------
    auto state = std::make_unique<PortalCaptureState>();
    state->node_id = response.node_id;
    state->width  = cfg_.format.width;
    state->height = cfg_.format.height;

    state->queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) state->queue_.set_depth(cfg_.queue_depth);

    const std::size_t bytes =
        static_cast<std::size_t>(cfg_.format.width) *
        static_cast<std::size_t>(cfg_.format.height) * 4u;
    scratch_.assign(bytes, 0u);

    frame_index_ = 0;
    state_ = std::move(state);
    state_->running_.store(true, std::memory_order_release);
    state_->thread_ = std::thread([this]() { capture_loop(); });

    open_ = true;
    return plugins::kOk;
}

void PortalCapture::close() noexcept {
    if (!open_) return;
    open_ = false;
    if (state_) {
        state_->stop_thread();
    }
    state_.reset();
    scratch_.clear();
    scratch_.shrink_to_fit();
    region_scratch_.clear();
    region_scratch_.shrink_to_fit();
}

std::vector<plugins::CaptureDevice>
PortalCapture::enumerate_devices() const noexcept {
    // The portal owns the source selection, so the engine cannot
    // enumerate what it was not given.  Before a ScreenCast request has
    // been answered the list is empty — and that is the *honest*
    // answer, not a limitation of this backend (see the header).
    return {};
}

std::vector<plugins::CaptureFormat>
PortalCapture::supported_formats(const std::string&) const noexcept {
    // The formats are what the *source* negotiates, and that is only
    // known once the portal has answered.  Before that there is
    // nothing to report.
    return {};
}

plugins::CaptureCapabilities PortalCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    c.backend = "portal-capture";
    c.zero_copy = false;
    c.dirty_rects = false;
    // The portal can ask the compositor to draw the pointer into the
    // stream, but it is not guaranteed — so the cursor layer stays on.
    c.cursor_composited = false;
    c.region_selection = true;
    // The source list comes from the portal, not from an enumeration.
    c.enumeration = false;
    c.max_pixels = static_cast<std::uint64_t>(state_ ? state_->width : 0u) *
                   static_cast<std::uint64_t>(state_ ? state_->height : 0u);
    return c;
}

plugins::Status PortalCapture::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!open_ || !state_) return plugins::kErrNotReady;
    if (!state_->queue_.pop(out)) {
        return plugins::kErrNotReady;
    }
    return plugins::kOk;
}

plugins::CaptureStats PortalCapture::stats() const noexcept {
    plugins::CaptureStats s;
    s.frames_captured = frames_captured_.load(std::memory_order_relaxed);
    s.frames_dropped  = frames_dropped_.load(std::memory_order_relaxed);
    s.device_errors   = device_errors_.load(std::memory_order_relaxed);
    s.bytes_copied    = bytes_copied_.load(std::memory_order_relaxed);
    return s;
}

void PortalCapture::capture_loop() noexcept {
    pw_init(nullptr, nullptr);

    pw_main_loop* main_loop = pw_main_loop_new(nullptr);
    if (main_loop == nullptr) {
        core::log::Logger::instance().error(
            "xdg-portal: the PipeWire main loop could not be created");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        pw_deinit();
        return;
    }
    pw_loop* loop = pw_main_loop_get_loop(main_loop);

    pw_context* context = pw_context_new(loop, nullptr, 0);
    pw_core* core = pw_context_connect(context, nullptr, 0);
    if (core == nullptr) {
        core::log::Logger::instance().error(
            "xdg-portal: the PipeWire server is not reachable");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        pw_main_loop_destroy(main_loop);
        pw_deinit();
        state_->running_.store(false, std::memory_order_release);
        return;
    }

    // ---- The stream ------------------------------------------------------------
    //
    // The stream asks the compositor for BGRA.  The `process` event is
    // where the frames arrive, and the `param_changed` event is where
    // the compositor says what it actually settled on.
    //
    // The events are attached through `pw_stream_add_listener`: the
    // stream is created with `pw_stream_new` (which takes only the
    // properties) and the listener carries both the event table and
    // the user data — which is the `PortalCapture` itself.
    static const pw_stream_events kStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .destroy = nullptr,
        .state_changed = nullptr,
        .control_info = nullptr,
        .io_changed = nullptr,
        .param_changed = [](void* data, uint32_t id, const spa_pod* param) {
            auto* owner = static_cast<PortalCapture*>(data);
            if (owner == nullptr) return;
            owner->on_format_changed(id, param);
        },
        .add_buffer = nullptr,
        .remove_buffer = nullptr,
        .process = [](void* data) {
            auto* owner = static_cast<PortalCapture*>(data);
            if (owner == nullptr) return;
            owner->on_pipewire_process(owner);
        },
        .drained = nullptr,
        .command = nullptr,
        .trigger_done = nullptr,
    };

    pw_stream* stream = pw_stream_new(core, "nimrtc-screen-capture", nullptr);
    if (stream == nullptr) {
        core::log::Logger::instance().error(
            "xdg-portal: the PipeWire stream could not be created");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        pw_core_disconnect(core);
        pw_main_loop_destroy(main_loop);
        pw_deinit();
        state_->running_.store(false, std::memory_order_release);
        return;
    }
    state_->stream = stream;

    spa_hook listener{};
    pw_stream_add_listener(stream, &listener, &kStreamEvents, this);

    // ---- The format -------------------------------------------------------------
    //
    // The backend asks for BGRA with the requested size; the compositor
    // may answer with a different size, and the device reports whatever
    // it actually got (the seam's contract: read `config()` back, do
    // not assume the request was granted).
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_video_info_raw raw = {};
    raw.format = SPA_VIDEO_FORMAT_BGRA;
    raw.size.width = static_cast<std::uint32_t>(cfg_.format.width);
    raw.size.height = static_cast<std::uint32_t>(cfg_.format.height);

    spa_pod* format_pod = spa_format_video_raw_build(
        &builder, SPA_PARAM_EnumFormat, &raw);
    const spa_pod* params[1] = { format_pod };

    const int rc = pw_stream_connect(stream, PW_DIRECTION_INPUT,
                                     state_->node_id,
                                     PW_STREAM_FLAG_AUTOCONNECT,
                                     params, 1);
    if (rc < 0) {
        core::log::Logger::instance().error(
            "xdg-portal: the PipeWire stream could not connect to the "
            "granted node");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        pw_stream_destroy(stream);
        pw_core_disconnect(core);
        pw_main_loop_destroy(main_loop);
        pw_deinit();
        state_->running_.store(false, std::memory_order_release);
        return;
    }

    pw_stream_set_active(stream, true);

    // ---- The loop -----------------------------------------------------------------
    //
    // The loop runs until `close()` asks it to stop.  The loop is
    // *iterated* rather than *run* so that the "should I stop?" test
    // happens on every pass: the loop's thread owns the PipeWire
    // objects, and the engine's tick thread only ever touches the
    // shared bounded queue.
    //
    // The iteration is bounded so that the loop wakes up regularly even
    // when no frame is arriving.
    while (state_->running_.load(std::memory_order_acquire)) {
        // 50 ms of idle wait per pass: the loop wakes often enough to
        // notice `close()`, and rarely enough not to spin.
        spa_loop_control_iterate(loop->control, 50);
    }

    pw_stream_set_active(stream, false);
    pw_stream_disconnect(stream);
    pw_stream_destroy(stream);
    state_->stream = nullptr;
    pw_core_disconnect(core);
    pw_main_loop_destroy(main_loop);
    pw_deinit();
}

void PortalCapture::on_format_changed(std::uint32_t id,
                                      const void* param) noexcept {
    if (id != SPA_PARAM_Format || param == nullptr) return;
    if (!state_) return;

    // The format the compositor settled on: the pixel layout *and* the
    // frame size.  A buffer carries only the memory type, so this is
    // the only place the layout is available.
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(static_cast<const spa_pod*>(param), &info) <
        0) {
        return;
    }

    if (!spa_format_is_packed32(info.format)) {
        // The portable layer speaks BGRA; a format it cannot convert
        // is reported rather than mis-converted.
        core::log::Logger::instance().error(
            "xdg-portal: the compositor chose a format the backend "
            "cannot convert to BGRA");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    state_->format = info.format;
    state_->have_format = true;
    state_->width = info.size.width;
    state_->height = info.size.height;
}

void PortalCapture::on_pipewire_process(void* owner) noexcept {
    // The frames arrive here, on the PipeWire loop's thread.
    auto* capture = static_cast<PortalCapture*>(owner);
    if (capture == nullptr || capture->state_ == nullptr) return;
    auto* stream = static_cast<pw_stream*>(capture->state_->stream);
    if (stream == nullptr) return;

    // The format has to be known before a frame can be read: the
    // buffer carries the memory, not the layout.
    if (!capture->state_->have_format) return;

    pw_buffer* buffer = pw_stream_dequeue_buffer(stream);
    if (buffer == nullptr) return;

    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const std::uint32_t w = capture->state_->width;
            const std::uint32_t h = capture->state_->height;
            if (w != 0 && h != 0) {
                // The chunk carries the row stride: the compositor's
                // rows are padded, and reading them as tightly packed
                // would shear the picture.
                const std::size_t src_stride =
                    data.chunk->stride > 0
                        ? static_cast<std::size_t>(data.chunk->stride)
                        : static_cast<std::size_t>(w) * 4u;

                capture->on_frame(
                    static_cast<const std::uint8_t*>(data.data),
                    src_stride,
                    w,
                    h,
                    spa_format_is_bgra_order(capture->state_->format));
            }
        }
    }

    pw_stream_queue_buffer(stream, buffer);
}

void PortalCapture::on_frame(const std::uint8_t* src,
                             std::size_t src_stride,
                             std::uint32_t w,
                             std::uint32_t h,
                             bool bgra) noexcept {
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4u;
    const std::size_t needed = row_bytes * static_cast<std::size_t>(h);
    if (scratch_.size() < needed) scratch_.assign(needed, 0u);

    if (bgra) {
        // Already BGRA: a straight copy, row by row.
        for (std::uint32_t row = 0; row < h; ++row) {
            std::memcpy(scratch_.data() +
                            static_cast<std::size_t>(row) * row_bytes,
                        src + static_cast<std::size_t>(row) * src_stride,
                        row_bytes);
        }
    } else {
        // RGB(A): the red and blue channels are swapped on the way
        // in, so the portable layer always sees BGRA.
        for (std::uint32_t row = 0; row < h; ++row) {
            const std::uint8_t* src_row =
                src + static_cast<std::size_t>(row) * src_stride;
            std::uint8_t* dst_row =
                scratch_.data() + static_cast<std::size_t>(row) * row_bytes;
            for (std::uint32_t col = 0; col < w; ++col) {
                const std::uint8_t* px = src_row + static_cast<std::size_t>(col) * 4u;
                std::uint8_t* dst = dst_row + static_cast<std::size_t>(col) * 4u;
                dst[0] = px[2]; // B
                dst[1] = px[1]; // G
                dst[2] = px[0]; // R
                dst[3] = 0xFFu;
            }
        }
    }
    bytes_copied_.fetch_add(static_cast<std::uint64_t>(needed),
                             std::memory_order_relaxed);

    // The dirty rectangles: PipeWire does not report damage to
    // the client, so the region is computed by comparing the frame
    // with the previous one (remote-desktop plan A7).  The comparison
    // happens *after* the region crop, because the consumer's frame is
    // the region, not the whole stream.
    plugins::CaptureDirtyRects dirty;

    // The region crop, exactly as the X11 and DXGI backends do it.
    const bool has_region =
        cfg_.region.width != 0 && cfg_.region.height != 0;
    const std::uint8_t* pixels = scratch_.data();
    std::uint32_t frame_w = w;
    std::uint32_t frame_h = h;
    std::int32_t stride = static_cast<std::int32_t>(row_bytes);

    if (has_region) {
        const std::uint32_t rx = std::min(cfg_.region.x, w);
        const std::uint32_t ry = std::min(cfg_.region.y, h);
        const std::uint32_t rw = std::min(cfg_.region.width, w - rx);
        const std::uint32_t rh = std::min(cfg_.region.height, h - ry);
        if (rw == 0 || rh == 0) return;
        const std::size_t region_row = static_cast<std::size_t>(rw) * 4u;
        const std::size_t region_bytes =
            region_row * static_cast<std::size_t>(rh);
        if (region_scratch_.size() != region_bytes) {
            region_scratch_.assign(region_bytes, 0u);
        }
        for (std::uint32_t row = 0; row < rh; ++row) {
            std::memcpy(region_scratch_.data() +
                            static_cast<std::size_t>(row) * region_row,
                        scratch_.data() +
                            (static_cast<std::size_t>(ry + row) * w + rx) * 4u,
                        region_row);
        }
        pixels = region_scratch_.data();
        frame_w = rw;
        frame_h = rh;
        stride = static_cast<std::int32_t>(region_row);
    }

    const std::int64_t ts = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const std::uint32_t seq = frame_index_++;
    const std::size_t frame_bytes =
        static_cast<std::size_t>(stride) * static_cast<std::size_t>(frame_h);

    // ---- The changed region (remote-desktop plan A7) ----------------------
    //
    // An unchanged frame is not worth queuing: the consumer would
    // encode the same picture again and spend the user's bandwidth on
    // it.  A static desktop therefore costs (almost) nothing.
    const bool identical = dirty_detector_.compare(
        pixels, frame_bytes, stride, frame_w, frame_h, 4, dirty);
    if (identical) {
        return;
    }

    const auto outcome = state_->queue_.push(
        pixels, frame_bytes, stride, frame_w, frame_h, ts, seq, dirty);
    switch (outcome) {
        case CaptureQueue::PushResult::kQueued:
            frames_captured_.fetch_add(1, std::memory_order_relaxed);
            break;
        case CaptureQueue::PushResult::kDroppedOldest:
            frames_captured_.fetch_add(1, std::memory_order_relaxed);
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            break;
        case CaptureQueue::PushResult::kRejected:
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    if (cb_) {
        plugins::CaptureFrame view;
        view.pixels = pixels;
        view.size = frame_bytes;
        view.stride = stride;
        view.width = frame_w;
        view.height = frame_h;
        view.capture_ts_us = ts;
        view.frame_seq = seq;
        view.dirty = dirty;
        view.has_gpu_handle = false;
        cb_(view);
    }
}

#else // !NIMRTC_PORTAL_CAPTURE

// ---------------------------------------------------------------------------
// The non-Linux stub
// ---------------------------------------------------------------------------

class PortalCaptureState {};

PortalCapture::PortalCapture(plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

PortalCapture::~PortalCapture() = default;

bool PortalCapture::available() noexcept {
    return false;
}

bool PortalCapture::portal_available() noexcept {
    return false;
}

plugins::Status PortalCapture::open() noexcept {
    // The XDG portal is a Linux / freedesktop.org API.  On any other
    // host this device simply does not exist.
    return plugins::kErrUnsupported;
}

void PortalCapture::close() noexcept {}

std::vector<plugins::CaptureDevice>
PortalCapture::enumerate_devices() const noexcept {
    return {};
}

std::vector<plugins::CaptureFormat>
PortalCapture::supported_formats(const std::string&) const noexcept {
    return {};
}

plugins::CaptureCapabilities PortalCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    c.backend = "portal-capture";
    // The stub describes the backend *as it would be* on a Linux host,
    // so a host that queries the capabilities before deciding where to
    // run gets the same answer on every platform.  What it cannot
    // promise on this host is the pixels — `open()` refuses.
    c.zero_copy = false;          // the frames arrive through PipeWire
    c.dirty_rects = false;        // PipeWire reports no changed regions
    c.cursor_composited = false;  // the pointer is not guaranteed
    c.region_selection = true;    // the portal can grant a region
    c.enumeration = false;        // the portal owns the source list
    return c;
}

void PortalCapture::capture_loop() noexcept {}

void PortalCapture::on_format_changed(std::uint32_t, const void*) noexcept {}

void PortalCapture::on_pipewire_process(void*) noexcept {}

void PortalCapture::on_frame(const std::uint8_t*,
                             std::size_t,
                             std::uint32_t,
                             std::uint32_t,
                             bool) noexcept {}

plugins::Status PortalCapture::pop_frame(plugins::CaptureFrame&) noexcept {
    return plugins::kErrUnsupported;
}

plugins::CaptureStats PortalCapture::stats() const noexcept {
    return plugins::CaptureStats{};
}

#endif // NIMRTC_PORTAL_CAPTURE

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_portal_capture() noexcept {
#if defined(NIMRTC_PORTAL_CAPTURE)
    static const PortalCaptureFactory kFactory{};
    core::PluginRegistry::instance().register_video_capture(kFactory.id(),
                                                            &kFactory);
#endif
}

} // namespace nimrtc::video_capture
