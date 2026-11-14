/**
 * @file src/modules/video_sink/src/egl_renderer.cpp
 * @brief The Linux EGL / Wayland / X11 presentation sink.
 *
 * See include/nimrtc/video_sink/egl_renderer.hpp for the contract.
 *
 * ## The structure of this file
 *
 *   - `WaylandState` / `X11State` / `EglState` — the window-system
 *     plumbing.  They live in an anonymous namespace so that the Wayland
 *     and X11 **C callbacks** (which take a `void*`) can reach them.
 *   - `EglRenderer::Impl` — the presenter core and the counters.
 *   - `EglRenderer` — the `plugins::IVideoSink` face.
 *
 * ## Why the presenter core is shared
 *
 * The mailbox, the presentation clock and the drop accounting are *the
 * same* on Linux as on Windows and macOS.  They therefore live in the
 * portable `PresenterCore` (`presenter_core.hpp`) and are unit-tested on
 * every platform.  This file only supplies the `present()` callback — the
 * half that touches EGL / Wayland / X11.
 *
 * ## The optional dependencies
 *
 * Every window-system dependency is **optional**.  CMake probes for each
 * of them and defines the corresponding macro:
 *
 *   - `NIMRTC_VIDEO_SINK_HAS_EGL`       — `libEGL` + `libGLESv2`
 *   - `NIMRTC_VIDEO_SINK_HAS_WAYLAND`   — `libwayland-client` (+ `wayland-egl`)
 *   - `NIMRTC_VIDEO_SINK_HAS_X11`       — `libX11`
 *   - `NIMRTC_VIDEO_SINK_HAS_XCB_SHM`   — `libxcb-shm`
 *   - `NIMRTC_VIDEO_SINK_HAS_XDG_SHELL` — the `xdg-shell` protocol headers
 *   - `NIMRTC_VIDEO_SINK_HAS_DMABUF`    — the `linux-dmabuf` protocol headers
 *
 * A missing dependency never breaks the build: the corresponding path is
 * simply not compiled, and `open()` reports `kErrUnsupported` when no
 * usable path is left.
 *
 * @note Remote-desktop plan E1 / E4 / E5 / E8.
 */

#include <nimrtc/video_sink/egl_renderer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_sink/presenter_core.hpp>
#include <nimrtc/video_sink/scale_to_fit.hpp>

#if defined(__linux__)

    #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
        #include <EGL/egl.h>
        #include <EGL/eglext.h>
        #include <GLES2/gl2.h>
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)
        #include <wayland-client.h>
        #include <wayland-egl.h>
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_X11)
        #include <X11/Xlib.h>
        #include <X11/Xutil.h>
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_XCB_SHM)
        #include <xcb/shm.h>
        #include <xcb/xcb.h>
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_XDG_SHELL)
        #include <xdg-shell-client-protocol.h>
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_DMABUF)
        #include <linux-dmabuf-v1-client-protocol.h>
    #endif

#endif  // __linux__

namespace nimrtc::video_sink {

namespace {

using Clock = std::chrono::steady_clock;

/** The default presentation rate when the configuration names none. */
constexpr std::uint32_t kDefaultPresentFps = 30;

#if defined(__linux__) && defined(NIMRTC_VIDEO_SINK_HAS_EGL)

/**
 * The GLES2 shader used to blit the decoded picture into the surface.
 *
 * It is compiled at runtime so the whole adapter stays in one
 * translation unit — no build-system shader compilation step.
 *
 * The destination rectangle is the scale-to-fit rectangle (plan E7), so a
 * 16:9 picture in a 4:3 window is letterboxed rather than stretched.
 */
constexpr const char* kVertexShader = R"GLSL(
attribute vec2 a_pos;
attribute vec2 a_uv;
uniform vec4 u_dest;   // x, y, w, h in normalised target coordinates
varying vec2 v_uv;
void main() {
    // The unit square is mapped onto the destination rectangle.
    vec2 p = u_dest.xy + a_pos * u_dest.zw;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    v_uv = a_uv;
}
)GLSL";

constexpr const char* kFragmentShader = R"GLSL(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
void main() {
    gl_FragColor = texture2D(u_tex, v_uv);
}
)GLSL";

#endif  // __linux__ && EGL

// ===========================================================================
// The window-system state
// ===========================================================================
//
// These live in an anonymous namespace so that the Wayland / X11 C
// callbacks (which receive a `void*` context) can reach them without the
// `Impl` type being visible outside the class.

#if defined(__linux__)

#if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)

/** The Wayland connection and its globals. */
struct WaylandState {
    struct wl_display*    display    = nullptr;
    struct wl_compositor* compositor = nullptr;
    struct wl_shm*        shm        = nullptr;
    struct wl_surface*    surface    = nullptr;
    struct wl_egl_window* egl_window = nullptr;

    // The `wl_shm` CPU path.
    struct wl_buffer*   buffer    = nullptr;
    struct wl_shm_pool* shm_pool  = nullptr;
    void*               shm_data  = nullptr;
    std::uint32_t       shm_w     = 0;
    std::uint32_t       shm_h     = 0;

    /** True when the compositor advertises the zero-copy route. */
    bool has_dmabuf = false;

    /** True when the surface was supplied by the host (not created here). */
    bool surface_owned = false;

    #if defined(NIMRTC_VIDEO_SINK_HAS_XDG_SHELL)
    struct xdg_wm_base*  wm_base  = nullptr;
    struct xdg_surface*  xdg_surface = nullptr;
    struct xdg_toplevel* xdg_toplevel = nullptr;
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_DMABUF)
    struct zwp_linux_dmabuf_v1* dmabuf = nullptr;
    #endif
};

    #if defined(NIMRTC_VIDEO_SINK_HAS_XDG_SHELL)
void handle_xdg_wm_base_ping(void* /*data*/, struct xdg_wm_base* wm_base,
                             uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

const struct xdg_wm_base_listener kXdgWmBaseListener = {
    handle_xdg_wm_base_ping,
};
    #endif

void handle_wl_shm_format(void* /*data*/, struct wl_shm* /*shm*/,
                          uint32_t /*format*/) {
    // The format list is informational for the `wl_shm` path (the picture
    // is always ARGB8888 / XRGB8888).
}

const struct wl_shm_listener kWlShmListener = {
    handle_wl_shm_format,
};

    #if defined(NIMRTC_VIDEO_SINK_HAS_XDG_SHELL)
void handle_xdg_surface_configure(void* /*data*/, struct xdg_surface* surface,
                                  uint32_t serial) {
    // The compositor asks for a configure acknowledgement before the
    // surface may be drawn into.
    xdg_surface_ack_configure(surface, serial);
}

const struct xdg_surface_listener kXdgSurfaceListener = {
    handle_xdg_surface_configure,
};
    #endif

void handle_wl_registry(void* data, struct wl_registry* registry,
                        uint32_t name, const char* interface,
                        uint32_t version) {
    auto* wl = static_cast<WaylandState*>(data);
    if (wl == nullptr) return;

    #if defined(NIMRTC_VIDEO_SINK_HAS_XDG_SHELL)
    if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wl->wm_base = static_cast<struct xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface,
                             std::min<uint32_t>(version, 3u)));
        xdg_wm_base_add_listener(wl->wm_base, &kXdgWmBaseListener, wl);
        return;
    }
    #endif
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        wl->compositor = static_cast<struct wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             std::min<uint32_t>(version, 4u)));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        wl->shm = static_cast<struct wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1));
        wl_shm_add_listener(wl->shm, &kWlShmListener, wl);
    }
    #if defined(NIMRTC_VIDEO_SINK_HAS_DMABUF)
    else if (std::strcmp(zwp_linux_dmabuf_v1_interface.name) == 0) {
        // The compositor advertises the zero-copy route: a DMA-BUF file
        // descriptor can be handed over directly.
        wl->dmabuf = static_cast<struct zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name,
                             &zwp_linux_dmabuf_v1_interface, 3));
        wl->has_dmabuf = (wl->dmabuf != nullptr);
    }
    #endif
}

const struct wl_registry_listener kWlRegistryListener = {
    handle_wl_registry,
};

#endif  // WAYLAND

#if defined(NIMRTC_VIDEO_SINK_HAS_X11)

/** The X11 connection and its drawable. */
struct X11State {
    Display* display = nullptr;
    Window   window  = 0;
    GC       gc      = nullptr;
    XImage*  image   = nullptr;

    #if defined(NIMRTC_VIDEO_SINK_HAS_XCB_SHM)
    xcb_connection_t*  xcb_conn   = nullptr;
    xcb_shm_segment_t* xcb_shm    = nullptr;
    xcb_shmid_t        xcb_shm_id = 0;
    void*              xcb_shm_data = nullptr;
    std::uint32_t      xcb_shm_w  = 0;
    std::uint32_t      xcb_shm_h  = 0;
    #endif
};

#endif  // X11

#if defined(NIMRTC_VIDEO_SINK_HAS_EGL)

/** The EGL display / context / surface and the GLES2 objects. */
struct EglState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig  config  = nullptr;

    GLuint program = 0;
    GLuint texture = 0;
    GLuint vbo     = 0;
    std::uint32_t tex_w = 0;
    std::uint32_t tex_h = 0;

    std::uint32_t target_w = 0;
    std::uint32_t target_h = 0;
};

#endif  // EGL

/** Which window-system path is in use. */
enum class Backend {
    kNone,
    kWaylandEgl,
    kWaylandShm,
    kX11Egl,
    kX11PutImage,
};

#endif  // __linux__

} // namespace

// ===========================================================================
// EglRenderer::Impl
// ===========================================================================

struct EglRenderer::Impl {
    explicit Impl(plugins::VideoSinkConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~Impl() { stop_presenter(); }

    plugins::VideoSinkConfig cfg_;

    /// Guards `cfg_`.
    ///
    ///  The configuration is written by `set_config()` (the engine calls
    ///  it when the viewer resizes the window) and read by `config()`.
    mutable std::mutex cfg_mutex_;

    // ---- Counters (atomic — `stats()` may be read from any thread) ----
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> format_conversions_{0};
    std::atomic<std::uint64_t> render_errors_{0};
    std::atomic<std::int64_t>  last_render_age_us_{0};
    std::atomic<bool>          running_{false};

    /** The most recent frame's metadata (for `current_frame_info()`). */
    mutable std::mutex      info_mutex_;
    plugins::VideoFrameInfo last_info_{};

    /** The portable presenter (mailbox + clock + accounting). */
    std::unique_ptr<PresenterCore> presenter_;

#if defined(__linux__)
    Backend backend_ = Backend::kNone;

    #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)
    std::unique_ptr<WaylandState> wl_;
    #endif
    #if defined(NIMRTC_VIDEO_SINK_HAS_X11)
    std::unique_ptr<X11State> x11_;
    #endif
    #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
    std::unique_ptr<EglState> egl_;
    #endif
#endif

    /** The presentation cadence, in frames per second. */
    std::uint32_t target_fps() const noexcept {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        if (cfg_.target_fps > 0) return cfg_.target_fps;
        if (cfg_.max_fps > 0) return cfg_.max_fps;
        return kDefaultPresentFps;
    }

    // ---- The presenter ---------------------------------------------------

    void start_presenter();
    void stop_presenter();

    /** Present one frame from the mailbox. */
    bool present_one(const PresentedFrame& frame);

#if defined(__linux__)
    /** Select and initialise the window-system backend. */
    bool open_backend();

    /** Tear the window-system backend down. */
    void close_backend();

    #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)
    /** Connect to the Wayland display and bind the globals. */
    bool init_wayland();
    void destroy_wayland();

    /** Present the BGRA picture through `wl_shm`. */
    bool present_via_wl_shm(const PresentedFrame& frame);
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_X11)
    /** Open the X11 display and create the drawable. */
    bool init_x11();
    void destroy_x11();

    /** Present the BGRA picture through `XPutImage`. */
    bool present_via_x11(const PresentedFrame& frame);
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
    /** Initialise the EGL display / context / surface. */
    bool init_egl();
    void destroy_egl();

    /** Build the GLES2 program (lazily). */
    bool ensure_gl_program();

    /** Upload the BGRA picture into the GL texture. */
    bool upload_bgra(const PresentedFrame& frame);

    /** Draw the GL texture into the EGL surface. */
    bool draw_with_egl(const PresentedFrame& frame);
    #endif
#endif
};
// ===========================================================================
// The presenter thread
// ===========================================================================

void EglRenderer::Impl::start_presenter() {
    if (running_.exchange(true)) return;
    presenter_ = std::make_unique<PresenterCore>(
        target_fps(),
        [this](PresentedFrame& frame) { return present_one(frame); });
    presenter_->start();
}

void EglRenderer::Impl::stop_presenter() {
    if (presenter_) {
        presenter_->stop();
        presenter_.reset();
    }
    running_.store(false);
}

bool EglRenderer::Impl::present_one(const PresentedFrame& frame) {
    if (frame.pixels.empty() || frame.width == 0 || frame.height == 0) {
        return false;
    }
#if defined(__linux__)
    switch (backend_) {
        #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
        case Backend::kWaylandEgl:
        case Backend::kX11Egl:
            return draw_with_egl(frame);
        #endif
        #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)
        case Backend::kWaylandShm:
            return present_via_wl_shm(frame);
        #endif
        #if defined(NIMRTC_VIDEO_SINK_HAS_X11)
        case Backend::kX11PutImage:
            return present_via_x11(frame);
        #endif
        default:
            return false;
    }
#else
    (void)frame;
    return true;
#endif
}

// ===========================================================================
// The Linux implementation
// ===========================================================================

#if defined(__linux__)

bool EglRenderer::Impl::open_backend() {
    #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)
    // The Wayland path is preferred: it is the modern Linux desktop.
    if (init_wayland()) {
        #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
        if (init_egl()) {
            backend_ = Backend::kWaylandEgl;
            NIMRTC_LOG_INFO("egl renderer: Wayland + EGL surface");
            return true;
        }
        #endif
        // The CPU path: `wl_shm`.
        backend_ = Backend::kWaylandShm;
        NIMRTC_LOG_INFO("egl renderer: Wayland + wl_shm surface ("
                        << (wl_ && wl_->has_dmabuf ? "dmabuf advertised"
                                                  : "no dmabuf")
                        << ")");
        return true;
    }
    #endif

    #if defined(NIMRTC_VIDEO_SINK_HAS_X11)
    // The X11 fallback.
    if (init_x11()) {
        #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
        if (init_egl()) {
            backend_ = Backend::kX11Egl;
            NIMRTC_LOG_INFO("egl renderer: X11 + EGL surface");
            return true;
        }
        #endif
        backend_ = Backend::kX11PutImage;
        NIMRTC_LOG_INFO("egl renderer: X11 + XPutImage surface");
        return true;
    }
    #endif

    NIMRTC_LOG_ERROR("egl renderer: no usable window-system backend "
                     "(neither Wayland nor X11 is available)");
    return false;
}

void EglRenderer::Impl::close_backend() {
    #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
    destroy_egl();
    #endif
    #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)
    destroy_wayland();
    #endif
    #if defined(NIMRTC_VIDEO_SINK_HAS_X11)
    destroy_x11();
    #endif
    backend_ = Backend::kNone;
}

// ---------------------------------------------------------------------------
// Wayland
// ---------------------------------------------------------------------------

    #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)

bool EglRenderer::Impl::init_wayland() {
    wl_ = std::make_unique<WaylandState>();

    wl_->display = wl_display_connect(nullptr);
    if (wl_->display == nullptr) {
        NIMRTC_LOG_DEBUG("egl renderer: wl_display_connect failed");
        wl_.reset();
        return false;
    }

    struct wl_registry* registry = wl_display_get_registry(wl_->display);
    if (registry == nullptr) {
        wl_display_disconnect(wl_->display);
        wl_.reset();
        return false;
    }
    wl_registry_add_listener(registry, &kWlRegistryListener, wl_.get());

    // Two round-trips: the first binds the globals, the second delivers
    // the initial events (the `wl_shm` format list, the `xdg_wm_base`
    // ping, ...).
    wl_display_roundtrip(wl_->display);
    wl_display_roundtrip(wl_->display);

    if (wl_->compositor == nullptr) {
        NIMRTC_LOG_DEBUG("egl renderer: no wl_compositor");
        destroy_wayland();
        return false;
    }

    // The surface.  A host may hand one over (`native_window`);
    // otherwise one is created here.
    if (cfg_.native_window != nullptr) {
        wl_->surface =
            static_cast<struct wl_surface*>(cfg_.native_window);
    } else {
        wl_->surface = wl_compositor_create_surface(wl_->compositor);
        wl_->surface_owned = true;
        #if defined(NIMRTC_VIDEO_SINK_HAS_XDG_SHELL)
        if (wl_->wm_base != nullptr) {
            wl_->xdg_surface =
                xdg_wm_base_get_xdg_surface(wl_->wm_base, wl_->surface);
            xdg_surface_add_listener(wl_->xdg_surface, &kXdgSurfaceListener,
                                     wl_.get());
            wl_->xdg_toplevel =
                xdg_surface_get_toplevel(wl_->xdg_surface);
            xdg_toplevel_set_title(wl_->xdg_toplevel, "NimRTC");
            xdg_surface_commit(wl_->xdg_surface);
        }
        #endif
    }
    if (wl_->surface == nullptr) {
        destroy_wayland();
        return false;
    }

    wl_display_commit(wl_->display);
    NIMRTC_LOG_INFO("egl renderer: connected to the Wayland display");
    return true;
}

void EglRenderer::Impl::destroy_wayland() {
    if (!wl_) return;

    #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
    if (wl_->egl_window != nullptr) {
        wl_egl_window_destroy(wl_->egl_window);
        wl_->egl_window = nullptr;
    }
    #endif
    if (wl_->buffer != nullptr) {
        wl_buffer_destroy(wl_->buffer);
        wl_->buffer = nullptr;
    }
    if (wl_->shm_pool != nullptr) {
        wl_shm_pool_destroy(wl_->shm_pool);
        wl_->shm_pool = nullptr;
    }
    #if defined(NIMRTC_VIDEO_SINK_HAS_XDG_SHELL)
    if (wl_->xdg_toplevel != nullptr) {
        xdg_toplevel_destroy(wl_->xdg_toplevel);
        wl_->xdg_toplevel = nullptr;
    }
    if (wl_->xdg_surface != nullptr) {
        xdg_surface_destroy(wl_->xdg_surface);
        wl_->xdg_surface = nullptr;
    }
    if (wl_->wm_base != nullptr) {
        xdg_wm_base_destroy(wl_->wm_base);
        wl_->wm_base = nullptr;
    }
    #endif
    if (wl_->surface != nullptr && wl_->surface_owned) {
        wl_surface_destroy(wl_->surface);
    }
    wl_->surface = nullptr;
    if (wl_->shm != nullptr) {
        wl_shm_destroy(wl_->shm);
        wl_->shm = nullptr;
    }
    if (wl_->compositor != nullptr) {
        wl_compositor_destroy(wl_->compositor);
        wl_->compositor = nullptr;
    }
    #if defined(NIMRTC_VIDEO_SINK_HAS_DMABUF)
    if (wl_->dmabuf != nullptr) {
        zwp_linux_dmabuf_v1_destroy(wl_->dmabuf);
        wl_->dmabuf = nullptr;
    }
    #endif
    if (wl_->display != nullptr) {
        wl_display_disconnect(wl_->display);
        wl_->display = nullptr;
    }
    wl_.reset();
}

bool EglRenderer::Impl::present_via_wl_shm(const PresentedFrame& frame) {
    if (!wl_ || wl_->surface == nullptr || wl_->shm == nullptr) return false;

    const std::uint32_t w = frame.width;
    const std::uint32_t h = frame.height;
    const std::int32_t  stride = static_cast<std::int32_t>(w * 4u);
    const std::size_t   size  = static_cast<std::size_t>(stride) * h;

    // The pool is re-created when the geometry changes.
    if (wl_->buffer == nullptr || wl_->shm_w != w || wl_->shm_h != h) {
        if (wl_->buffer != nullptr) {
            wl_buffer_destroy(wl_->buffer);
            wl_->buffer = nullptr;
        }
        if (wl_->shm_pool != nullptr) {
            wl_shm_pool_destroy(wl_->shm_pool);
            wl_->shm_pool = nullptr;
        }
        const std::int32_t pool_size = static_cast<std::int32_t>(size);
        wl_->shm_pool = wl_shm_create_pool(wl_->shm, pool_size);
        if (wl_->shm_pool == nullptr) return false;
        wl_->shm_data = wl_shm_pool_create_buffer(wl_->shm_pool, 0, pool_size);
        if (wl_->shm_data == nullptr) return false;
        wl_->buffer = wl_shm_pool_create_buffer(wl_->shm_pool, 0, pool_size);
        if (wl_->buffer == nullptr) return false;
        wl_->shm_w = w;
        wl_->shm_h = h;
    }

    // Copy the picture into the shared-memory buffer.
    auto* dst = static_cast<std::uint8_t*>(wl_->shm_data);
    for (std::uint32_t row = 0; row < h; ++row) {
        std::memcpy(dst + static_cast<std::size_t>(row) * stride,
                    frame.pixels.data() +
                        static_cast<std::ptrdiff_t>(row) * stride,
                    static_cast<std::size_t>(stride));
    }

    wl_surface_attach(wl_->surface, wl_->buffer, 0, 0);
    wl_surface_damage(wl_->surface, 0, 0, static_cast<std::int32_t>(w),
                      static_cast<std::int32_t>(h));
    wl_surface_commit(wl_->surface);
    return true;
}

    #endif  // WAYLAND

// ---------------------------------------------------------------------------
// X11
// ---------------------------------------------------------------------------

    #if defined(NIMRTC_VIDEO_SINK_HAS_X11)

bool EglRenderer::Impl::init_x11() {
    x11_ = std::make_unique<X11State>();

    x11_->display = XOpenDisplay(nullptr);
    if (x11_->display == nullptr) {
        NIMRTC_LOG_DEBUG("egl renderer: XOpenDisplay failed");
        x11_.reset();
        return false;
    }

    const int screen = DefaultScreen(x11_->display);
    if (cfg_.native_window != nullptr) {
        x11_->window = static_cast<Window>(cfg_.native_window);
    } else {
        x11_->window = XCreateSimpleWindow(
            x11_->display, RootWindow(x11_->display, screen), 0, 0, 1280, 720,
            0, BlackPixel(x11_->display, screen),
            BlackPixel(x11_->display, screen));
        XStoreName(x11_->display, x11_->window, "NimRTC");
        XMapWindow(x11_->display, x11_->window);
    }
    x11_->gc = XCreateGC(x11_->display, x11_->window, 0, nullptr);
    XFlush(x11_->display);

    XWindowAttributes attrs{};
    if (XGetWindowAttributes(x11_->display, x11_->window, &attrs)) {
        #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)
        if (egl_) {
            egl_->target_w = static_cast<std::uint32_t>(std::max(attrs.width, 1));
            egl_->target_h = static_cast<std::uint32_t>(std::max(attrs.height, 1));
        }
        #endif
    }

    NIMRTC_LOG_INFO("egl renderer: X11 display opened");
    return true;
}

void EglRenderer::Impl::destroy_x11() {
    if (!x11_) return;
    if (x11_->image != nullptr) {
        XDestroyImage(x11_->image);
        x11_->image = nullptr;
    }
    if (x11_->gc != nullptr) {
        XFreeGC(x11_->display, x11_->gc);
        x11_->gc = nullptr;
    }
    if (x11_->display != nullptr) {
        XCloseDisplay(x11_->display);
        x11_->display = nullptr;
    }
    x11_.reset();
}

bool EglRenderer::Impl::present_via_x11(const PresentedFrame& frame) {
    if (!x11_ || x11_->display == nullptr || x11_->window == 0) return false;

    const std::uint32_t w = frame.width;
    const std::uint32_t h = frame.height;
    const std::int32_t  stride = static_cast<std::int32_t>(w * 4u);

    // The image is re-created when the geometry changes.
    if (x11_->image == nullptr ||
        static_cast<std::uint32_t>(x11_->image->width) != w ||
        static_cast<std::uint32_t>(x11_->image->height) != h) {
        if (x11_->image != nullptr) {
            XDestroyImage(x11_->image);
            x11_->image = nullptr;
        }
        const int screen = DefaultScreen(x11_->display);
        x11_->image = XCreateImage(
            x11_->display, DefaultVisual(x11_->display, screen),
            DefaultDepth(x11_->display, screen), ZPixmap, 0, nullptr,
            static_cast<unsigned int>(w), static_cast<unsigned int>(h), 32,
            stride);
        if (x11_->image == nullptr) return false;
    }

    // Copy the picture into the image.  X11 wants the pixels in the
    // native byte order; on a little-endian host that is exactly the
    // BGRA order the sink carries.
    for (std::uint32_t row = 0; row < h; ++row) {
        auto* dst = static_cast<std::uint8_t*>(x11_->image->data) +
                    static_cast<std::ptrdiff_t>(row) * x11_->image->bytes_per_line;
        const std::uint8_t* src =
            frame.pixels.data() + static_cast<std::ptrdiff_t>(row) * stride;
        std::memcpy(dst, src, static_cast<std::size_t>(stride));
    }

    XPutImage(x11_->display, x11_->window, x11_->gc_, x11_->image, 0, 0, 0, 0,
              static_cast<unsigned int>(w), static_cast<unsigned int>(h));
    XFlush(x11_->display);
    return true;
}

    #endif  // X11

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------

    #if defined(NIMRTC_VIDEO_SINK_HAS_EGL)

bool EglRenderer::Impl::init_egl() {
    egl_ = std::make_unique<EglState>();

    const bool wayland = (wl_ != nullptr);

    // ---- The display ------------------------------------------------------
    if (wayland) {
        #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)
        wl_->egl_window = wl_egl_window_create(wl_->surface, 1280, 720);
        if (wl_->egl_window == nullptr) {
            egl_.reset();
            return false;
        }
        egl_->display = eglGetDisplay(
            reinterpret_cast<EGLNativeDisplayType>(wl_->display));
        #else
        egl_.reset();
        return false;
        #endif
    } else {
        #if defined(NIMRTC_VIDEO_SINK_HAS_X11)
        // The X11 platform display.  `eglGetPlatformDisplayEXT` is the
        // correct entry point; `eglGetDisplay(EGL_DEFAULT_DISPLAY)` is
        // the fallback for a driver that does not export the extension.
        auto get_platform_display =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (get_platform_display != nullptr && x11_ && x11_->display) {
            egl_->display = get_platform_display(
                EGL_PLATFORM_X11_EXT,
                reinterpret_cast<void*>(x11_->display), nullptr);
        }
        if (egl_->display == EGL_NO_DISPLAY) {
            egl_->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }
        #else
        egl_->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        #endif
    }
    if (egl_->display == EGL_NO_DISPLAY) {
        egl_.reset();
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(egl_->display, &major, &minor)) {
        egl_.reset();
        return false;
    }

    // ---- The config -------------------------------------------------------
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint    num_configs = 0;
    if (!eglChooseConfig(egl_->display, config_attribs, &config, 1,
                         &num_configs) ||
        num_configs == 0) {
        destroy_egl();
        return false;
    }
    egl_->config = config;

    // ---- The context ------------------------------------------------------
    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_->context = eglCreateContext(egl_->display, egl_->config,
                                      EGL_NO_CONTEXT, context_attribs);
    if (egl_->context == EGL_NO_CONTEXT) {
        destroy_egl();
        return false;
    }

    // ---- The surface ------------------------------------------------------
    if (wayland) {
        #if defined(NIMRTC_VIDEO_SINK_HAS_WAYLAND)
        egl_->surface = eglCreateWindowSurface(
            egl_->display, egl_->config,
            reinterpret_cast<EGLNativeWindowType>(wl_->egl_window), nullptr);
        #endif
    } else {
        #if defined(NIMRTC_VIDEO_SINK_HAS_X11)
        egl_->surface = eglCreateWindowSurface(
            egl_->display, egl_->config,
            reinterpret_cast<EGLNativeWindowType>(x11_->window), nullptr);
        #endif
    }
    if (egl_->surface == EGL_NO_SURFACE) {
        destroy_egl();
        return false;
    }

    if (!eglMakeCurrent(egl_->display, egl_->surface, egl_->surface,
                        egl_->context)) {
        destroy_egl();
        return false;
    }

    // ---- The target size --------------------------------------------------
    EGLint surface_w = 0;
    EGLint surface_h = 0;
    eglQuerySurface(egl_->display, egl_->surface, EGL_WIDTH, &surface_w);
    eglQuerySurface(egl_->display, egl_->surface, EGL_HEIGHT, &surface_h);
    egl_->target_w = static_cast<std::uint32_t>(std::max<EGLint>(surface_w, 1));
    egl_->target_h = static_cast<std::uint32_t>(std::max<EGLint>(surface_h, 1));

    NIMRTC_LOG_INFO("egl renderer: EGL surface created ("
                    << egl_->target_w << "x" << egl_->target_h << ")");
    return true;
}

void EglRenderer::Impl::destroy_egl() {
    if (!egl_) return;
    if (egl_->display == EGL_NO_DISPLAY) {
        egl_.reset();
        return;
    }
    eglMakeCurrent(egl_->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (egl_->program != 0) {
        glDeleteProgram(egl_->program);
        egl_->program = 0;
    }
    if (egl_->texture != 0) {
        glDeleteTextures(1, &egl_->texture);
        egl_->texture = 0;
    }
    if (egl_->vbo != 0) {
        glDeleteBuffers(1, &egl_->vbo);
        egl_->vbo = 0;
    }
    if (egl_->surface != EGL_NO_SURFACE) {
        eglDestroySurface(egl_->display, egl_->surface);
        egl_->surface = EGL_NO_SURFACE;
    }
    if (egl_->context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_->display, egl_->context);
        egl_->context = EGL_NO_CONTEXT;
    }
    eglTerminate(egl_->display);
    egl_->display = EGL_NO_DISPLAY;
    egl_.reset();
}

bool EglRenderer::Impl::ensure_gl_program() {
    if (!egl_) return false;
    if (egl_->program != 0) return true;

    auto compile = [](GLenum type, const char* src) -> GLuint {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok == 0) {
            char log[512] = {0};
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            NIMRTC_LOG_ERROR("egl renderer: shader compilation failed ("
                             << log << ")");
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    if (vertex == 0) return false;
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (fragment == 0) {
        glDeleteShader(vertex);
        return false;
    }

    egl_->program = glCreateProgram();
    glAttachShader(egl_->program, vertex);
    glAttachShader(egl_->program, fragment);
    glLinkProgram(egl_->program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = 0;
    glGetProgramiv(egl_->program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        char log[512] = {0};
        glGetProgramInfoLog(egl_->program, sizeof(log) - 1, nullptr, log);
        NIMRTC_LOG_ERROR("egl renderer: program link failed (" << log << ")");
        glDeleteProgram(egl_->program);
        egl_->program = 0;
        return false;
    }

    // The full-screen quad: x, y, u, v.
    const GLfloat quad[] = {
        0.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
    };
    glGenBuffers(1, &egl_->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, egl_->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glGenTextures(1, &egl_->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

bool EglRenderer::Impl::upload_bgra(const PresentedFrame& frame) {
    if (!egl_ || egl_->texture == 0) return false;

    if (egl_->tex_w != frame.width || egl_->tex_h != frame.height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     static_cast<GLsizei>(frame.width),
                     static_cast<GLsizei>(frame.height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        egl_->tex_w = frame.width;
        egl_->tex_h = frame.height;
    }

    // BGRA -> RGBA: the byte order differs, so the upload is done row by
    // row through a small swizzle buffer.
    const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * 4u;
    std::vector<std::uint8_t> row(row_bytes);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const std::uint8_t* src =
            frame.pixels.data() + static_cast<std::ptrdiff_t>(y) * row_bytes;
        for (std::size_t i = 0; i + 3 < row_bytes; i += 4) {
            row[i + 0] = src[i + 2];
            row[i + 1] = src[i + 1];
            row[i + 2] = src[i + 0];
            row[i + 3] = src[i + 3];
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(y),
                        static_cast<GLsizei>(frame.width), 1, GL_RGBA,
                        GL_UNSIGNED_BYTE, row.data());
    }
    return true;
}

bool EglRenderer::Impl::draw_with_egl(const PresentedFrame& frame) {
    if (!egl_ || egl_->display == EGL_NO_DISPLAY ||
        egl_->context == EGL_NO_CONTEXT) {
        return false;
    }
    if (!eglMakeCurrent(egl_->display, egl_->surface, egl_->surface,
                        egl_->context)) {
        return false;
    }
    if (!ensure_gl_program()) return false;
    if (!upload_bgra(frame)) return false;

    // ---- The scale-to-fit rectangle (plan E7) ------------------------------
    const ScaleRect dest = compute_scale_to_fit(
        static_cast<std::int32_t>(frame.width),
        static_cast<std::int32_t>(frame.height),
        static_cast<std::int32_t>(egl_->target_w),
        static_cast<std::int32_t>(egl_->target_h),
        ScaleMode::kLetterbox);
    if (dest.w <= 0 || dest.h <= 0) return false;

    glViewport(0, 0, static_cast<GLsizei>(egl_->target_w),
               static_cast<GLsizei>(egl_->target_h));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(egl_->program);

    const GLint a_pos = glGetAttribLocation(egl_->program, "a_pos");
    const GLint a_uv  = glGetAttribLocation(egl_->program, "a_uv");
    glBindBuffer(GL_ARRAY_BUFFER, egl_->vbo);
    if (a_pos >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(a_pos));
        glVertexAttribPointer(static_cast<GLuint>(a_pos), 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), nullptr);
    }
    if (a_uv >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(a_uv));
        glVertexAttribPointer(
            static_cast<GLuint>(a_uv), 2, GL_FLOAT, GL_FALSE,
            4 * sizeof(GLfloat),
            reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
    }

    const GLfloat dest_rect[4] = {
        static_cast<GLfloat>(dest.x) / static_cast<GLfloat>(egl_->target_w),
        static_cast<GLfloat>(dest.y) / static_cast<GLfloat>(egl_->target_h),
        static_cast<GLfloat>(dest.w) / static_cast<GLfloat>(egl_->target_w),
        static_cast<GLfloat>(dest.h) / static_cast<GLfloat>(egl_->target_h),
    };
    const GLint u_dest = glGetUniformLocation(egl_->program, "u_dest");
    if (u_dest >= 0) {
        glUniform4f(u_dest, dest_rect[0], dest_rect[1], dest_rect[2],
                    dest_rect[3]);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, egl_->texture);
    const GLint u_tex = glGetUniformLocation(egl_->program, "u_tex");
    if (u_tex >= 0) glUniform1i(u_tex, 0);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    // `eglSwapBuffers` is the present: it hands the back buffer to the
    // compositor.  The *pacing* is the presentation clock, not the
    // compositor's readiness — the swap itself never blocks.
    eglSwapBuffers(egl_->display, egl_->surface);
    return true;
}

    #endif  // EGL

#endif  // __linux__

// ===========================================================================
// EglRenderer
// ===========================================================================

EglRenderer::EglRenderer(plugins::VideoSinkConfig cfg) noexcept
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

EglRenderer::~EglRenderer() {
    if (impl_) impl_->stop_presenter();
}

const char* EglRenderer::name() const noexcept {
    return "nimrtc::video_sink::EglRenderer";
}

plugins::Status EglRenderer::open() noexcept {
#if defined(__linux__)
    if (!impl_->open_backend()) {
        impl_->close_backend();
        return plugins::kErrUnsupported;
    }
    impl_->start_presenter();
    return plugins::kOk;
#else
    // The seam still exists on a non-Linux build so a configuration that
    // names it resolves — but the renderer reports honestly that it
    // cannot run here rather than pretending to present.
    NIMRTC_LOG_ERROR("egl renderer: this build has no EGL / Wayland / X11 support");
    return plugins::kErrUnsupported;
#endif
}

void EglRenderer::close() noexcept {
    impl_->stop_presenter();
#if defined(__linux__)
    impl_->close_backend();
#endif
}

plugins::Status EglRenderer::render(
    const plugins::VideoSourceFrame& frame) noexcept {
    if (!impl_ || !impl_->running_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (frame.width == 0 || frame.height == 0 || !frame.plane_y) {
        impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kErrInvalidParam;
    }

    // The source frame's format.  A packed BGRA frame is uploaded as-is;
    // an I420 / NV12 frame is converted to BGRA first (the display
    // surface's format).
    const bool source_is_bgra =
        frame.format == plugins::VideoPixelFormat::kBGRA ||
        frame.format == plugins::VideoPixelFormat::kUnknown;

    std::vector<std::uint8_t> bgra;
    const std::uint8_t* src = frame.plane_y;
    std::int32_t src_stride = frame.stride_y > 0
        ? frame.stride_y
        : static_cast<std::int32_t>(frame.width * 4u);

    if (!source_is_bgra) {
        const auto layout = video_frame::compute_layout(
            plugins::VideoPixelFormat::kBGRA, frame.width, frame.height);
        bgra.resize(layout.buffer_size);
        if (!frame.plane_u || !frame.plane_v) {
            impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
            return plugins::kErrInvalidParam;
        }
        const std::int32_t stride_c = frame.stride_u > 0
            ? frame.stride_u
            : static_cast<std::int32_t>((frame.width + 1u) / 2u);
        video_frame::convert_i420_to_bgra(
            frame.width, frame.height,
            frame.plane_y,
            frame.stride_y > 0 ? frame.stride_y
                              : static_cast<std::int32_t>(frame.width),
            frame.plane_u, stride_c,
            frame.plane_v, stride_c,
            bgra.data(), layout.strides[0]);
        impl_->format_conversions_.fetch_add(1, std::memory_order_relaxed);
        src = bgra.data();
        src_stride = layout.strides[0];
    }

    // ---- Post the frame to the mailbox (latest-wins) ----------------------
    PresentedFrame presented;
    presented.pixels.assign(
        src, src + static_cast<std::ptrdiff_t>(src_stride) * frame.height);
    presented.width         = frame.width;
    presented.height        = frame.height;
    presented.stride        = static_cast<std::uint32_t>(src_stride);
    presented.capture_ts_us = frame.capture_ts_us;
    presented.info.capture_ts_us = frame.capture_ts_us;
    presented.info.frame_seq     = frame.frame_seq;
    presented.info.rtp_timestamp = frame.rtp_timestamp;

    const plugins::Status rc =
        impl_->presenter_ ? impl_->presenter_->submit(std::move(presented))
                          : plugins::kErrNotReady;
    if (rc != plugins::kOk) {
        impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }

    impl_->frames_rendered_.fetch_add(1, std::memory_order_relaxed);

    const auto now_us = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
    impl_->last_render_age_us_.store(now_us - frame.capture_ts_us,
                                      std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(impl_->info_mutex_);
        impl_->last_info_.capture_ts_us = frame.capture_ts_us;
        impl_->last_info_.frame_seq     = frame.frame_seq;
        impl_->last_info_.rtp_timestamp = frame.rtp_timestamp;
        impl_->last_info_.render_time_us = now_us;
    }
    return plugins::kOk;
}

plugins::Status EglRenderer::render(
    const plugins::GpuBuffer& surface) noexcept {
#if defined(__linux__)
    if (!impl_ || !impl_->running_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (!surface.has_gpu_handle()) return plugins::kErrInvalidParam;

    // The zero-copy route: a DMA-BUF file descriptor.  The compositor
    // takes the descriptor directly, so the picture never enters host
    // memory.
    const auto backend = surface.handle().backend;
    if (backend != plugins::GpuBackend::kLinuxVaapiDRM &&
        backend != plugins::GpuBackend::kLinuxVaapiSurface) {
        return plugins::kErrUnsupported;
    }
    const int fd = surface.handle().native_fd;
    if (fd < 0) return plugins::kErrInvalidParam;

    #if defined(NIMRTC_VIDEO_SINK_HAS_DMABUF)
    if (!impl_->wl_ || !impl_->wl_->has_dmabuf ||
        impl_->wl_->dmabuf == nullptr) {
        // The compositor does not advertise the zero-copy route.
        return plugins::kErrUnsupported;
    }

    // The import: the DMA-BUF descriptor is handed to the compositor's
    // `linux-dmabuf` implementation, which binds the fd into its own
    // GPU memory.
    auto* dmabuf = impl_->wl_->dmabuf;
    struct zwp_linux_dmabuf_v1_params* params =
        zwp_linux_dmabuf_v1_create_params(dmabuf);
    zwp_linux_dmabuf_v1_params_add_fd(params, fd, 0);
    zwp_linux_dmabuf_v1_params_set_offset(params, 0);
    zwp_linux_dmabuf_v1_params_set_stride(params,
                                          static_cast<std::int32_t>(
                                              surface.handle().native_pitch_bytes));
    zwp_linux_dmabuf_v1_params_set_height(params, surface.height());
    zwp_linux_dmabuf_v1_params_set_format(params,
                                           ZWP_LINUX_DMABUF_V1_FORMAT_ARGB8888);
    zwp_linux_dmabuf_v1_params_add_plane(params, 0, fd, 0,
                                          static_cast<std::int32_t>(
                                              surface.handle().native_pitch_bytes),
                                          surface.height(),
                                          ZWP_LINUX_DMABUF_V1_FORMAT_ARGB8888);
    zwp_linux_dmabuf_v1_create_buffer(dmabuf, params);
    zwp_linux_dmabuf_v1_params_destroy(params);

    impl_->frames_rendered_.fetch_add(1, std::memory_order_relaxed);
    impl_->frames_presented_.fetch_add(1, std::memory_order_relaxed);
    return plugins::kOk;
    #else
    (void)fd;
    return plugins::kErrUnsupported;
    #endif
#else
    (void)surface;
    return plugins::kErrUnsupported;
#endif
}

plugins::Status EglRenderer::render(
    const plugins::GpuBuffer& surface,
    const plugins::VideoFrameInfo& info) noexcept {
    const plugins::Status rc = render(surface);
    if (rc != plugins::kOk) return rc;
    std::lock_guard<std::mutex> lock(impl_->info_mutex_);
    impl_->last_info_.capture_ts_us = info.capture_ts_us;
    impl_->last_info_.frame_seq     = info.frame_seq;
    impl_->last_info_.rtp_timestamp = info.rtp_timestamp;
    impl_->last_info_.render_time_us = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
    return plugins::kOk;
}

bool EglRenderer::supports_gpu_render() const noexcept {
    return true;
}

bool EglRenderer::supports_cpu_render() const noexcept {
    return true;
}

plugins::VideoSinkStats EglRenderer::stats() const noexcept {
    plugins::VideoSinkStats s;
    s.frames_rendered = impl_->frames_rendered_.load(std::memory_order_relaxed);
    s.frames_dropped  = impl_->frames_dropped_.load(std::memory_order_relaxed);
    s.format_conversions =
        impl_->format_conversions_.load(std::memory_order_relaxed);
    s.render_errors   = impl_->render_errors_.load(std::memory_order_relaxed);
    s.last_render_age_us =
        static_cast<std::uint64_t>(impl_->last_render_age_us_.load(
            std::memory_order_relaxed));
    if (impl_->presenter_) {
        const auto core = impl_->presenter_->stats();
        s.frames_dropped_present = core.frames_dropped_present;
        s.frames_presented       = core.frames_presented;
        s.present_fps            = core.present_fps;
        s.presenter_queue_depth  = core.presenter_queue_depth;
    }
    return s;
}

plugins::VideoSinkConfig EglRenderer::config() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->cfg_mutex_);
    return impl_->cfg_;
}

plugins::Status EglRenderer::set_config(plugins::VideoSinkConfig cfg) noexcept {
    if (!impl_) return plugins::kErrNotReady;

    // The presentation geometry is a runtime property (plan E7): the
    // viewer resizes the window and the picture follows.  The EGL
    // surface's own size is the window's, so the *draw* rectangle
    // follows the new configuration; the surface is resized by the
    // compositor when the window is.
    std::lock_guard<std::mutex> lock(impl_->cfg_mutex_);
    impl_->cfg_ = std::move(cfg);

#if defined(__linux__) && defined(NIMRTC_VIDEO_SINK_HAS_EGL)
    // The EGL draw target follows the new configuration.
    if (impl_->egl_) {
        if (impl_->cfg_.width > 0) impl_->egl_->target_w = impl_->cfg_.width;
        if (impl_->cfg_.height > 0) impl_->egl_->target_h = impl_->cfg_.height;
    }
#endif
    return plugins::kOk;
}

const plugins::VideoFrameInfo* EglRenderer::current_frame_info() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->info_mutex_);
    return &impl_->last_info_;
}

bool EglRenderer::is_hardware_accelerated() const noexcept {
    return true;
}

std::string_view EglRenderer::hardware_backend() const noexcept {
    return "egl-gles2";
}

// ===========================================================================
// The factory
// ===========================================================================

std::string_view EglRendererFactory::id() const noexcept {
    return "egl";
}

std::string_view EglRendererFactory::display_name() const noexcept {
    return "Linux EGL / Wayland / X11 renderer";
}

plugins::IVideoSink* EglRendererFactory::create(
    plugins::VideoSinkConfig cfg) const {
    return new EglRenderer(std::move(cfg));
}

} // namespace nimrtc::video_sink
