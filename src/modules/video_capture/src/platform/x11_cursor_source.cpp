// modules/video_capture/src/platform/x11_cursor_source.cpp
// ----------------------------------------------------------------------------
// The X11 cursor source (remote-desktop plan A6).
//
// The Windows cursor source reads the pointer through `GetCursorInfo` /
// `GetIconInfo`; this is the Linux counterpart, and it uses the Xfixes
// extension's `XFixesGetCursorImage` — the one API that hands over the
// pointer's *bitmap* (already in the 32-bit BGRA shape the portable
// alpha compositor expects) as well as its position.
//
// See include/nimrtc/video_capture/x11_cursor_source.hpp.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/x11_cursor_source.hpp>

#include "x11_support.hpp"

#include <cstring>
#include <utility>

#if defined(__linux__) && defined(NIMRTC_HAVE_X11)
#  define NIMRTC_X11_CURSOR 1
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <X11/extensions/Xfixes.h>
// `Xlib.h` defines `Status` as a macro (`#define Status int`).  This
// file does not use `plugins::Status`, but the macro would still leak
// into the header the implementation includes, so it is dropped here.
#  undef Status
#endif

#include <nimrtc/core/log.hpp>

namespace nimrtc::video_capture {

#if defined(NIMRTC_X11_CURSOR)

struct X11CursorSource::Impl {
    Display* display = nullptr;
    int event_base = 0;
    int error_base = 0;
    bool has_xfixes = false;

    ~Impl() {
        if (display != nullptr) {
            XCloseDisplay(display);
            display = nullptr;
        }
    }
};

X11CursorSource::X11CursorSource() noexcept = default;

X11CursorSource::~X11CursorSource() = default;

bool X11CursorSource::has_live_cursor() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_) return false;
    if (!impl_->has_xfixes) return false;
    // A server that has gone away (the X connection died) has no
    // pointer to report.
    if (impl_->display == nullptr) return false;
    return XConnectionNumber(impl_->display) != -1;
}

void X11CursorSource::set_screen_origin(std::int32_t x, std::int32_t y) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    origin_x_ = x;
    origin_y_ = y;
}

CursorState X11CursorSource::query() noexcept {
    CursorState state;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_ || !impl_->has_xfixes || impl_->display == nullptr) {
        return state;
    }

    Display* display = impl_->display;

    // ---- The pointer's image and position --------------------------------
    //
    // `XFixesGetCursorImage` allocates the image with Xmalloc; the
    // caller must free it with XFree.  The image's pixel layout is the
    // server's native one, so it is converted into BGRA exactly like
    // the capture device's pixels are.
    XFixesCursorImage* image = XFixesGetCursorImage(display);
    if (image == nullptr) {
        return state;
    }

    const std::uint32_t w = static_cast<std::uint32_t>(image->width);
    const std::uint32_t h = static_cast<std::uint32_t>(image->height);
    // The Xfixes cursor image has no stride field: it is a packed
    // array of 32-bit pixels, one per pixel, with no row padding.
    const std::int32_t stride = static_cast<std::int32_t>(w) * 4;

    // The pointer's *x, y* (the hotspot) is in root-window
    // coordinates; subtract the captured screen's origin.
    state.x = static_cast<std::int32_t>(image->x) - origin_x_;
    state.y = static_cast<std::int32_t>(image->y) - origin_y_;
    state.visible = true;
    state.live = true;

    // ---- The bitmap ---------------------------------------------------------
    if (w > 0 && h > 0 && image->pixels != nullptr) {
        state.bgra.resize(static_cast<std::size_t>(w) *
                          static_cast<std::size_t>(h) * 4u);
        state.bitmap_width  = w;
        state.bitmap_height = h;
        state.bitmap_stride = stride;

        // Each pixel is a 32-bit **ARGB** value (the alpha in the top
        // byte), which is what the Xfixes cursor image documents.  The
        // portable compositor wants **BGRA**, so the channels are
        // reordered on the way out.
        const auto* src = reinterpret_cast<const std::uint8_t*>(image->pixels);
        for (std::uint32_t row = 0; row < h; ++row) {
            const std::uint8_t* src_row =
                src + static_cast<std::size_t>(row) *
                          static_cast<std::size_t>(stride);
            std::uint8_t* dst_row =
                state.bgra.data() + static_cast<std::size_t>(row) *
                                        static_cast<std::size_t>(w) * 4u;
            for (std::uint32_t col = 0; col < w; ++col) {
                std::uint32_t px;
                std::memcpy(&px, src_row + static_cast<std::size_t>(col) * 4u,
                            sizeof(px));
                dst_row[col * 4u + 0] = static_cast<std::uint8_t>(px & 0xFFu);         // B
                dst_row[col * 4u + 1] = static_cast<std::uint8_t>((px >> 8) & 0xFFu);  // G
                dst_row[col * 4u + 2] = static_cast<std::uint8_t>((px >> 16) & 0xFFu); // R
                dst_row[col * 4u + 3] = static_cast<std::uint8_t>((px >> 24) & 0xFFu); // A
            }
        }
    }

    XFree(image);
    return state;
}

std::unique_ptr<ICursorSource> make_x11_cursor_source() {
    auto source = std::make_unique<X11CursorSource>();
    auto impl = std::make_unique<X11CursorSource::Impl>();

    impl->display = XOpenDisplay(nullptr);
    if (impl->display == nullptr) {
        // No display: there is no pointer to read.  The source still
        // exists (so the caller need not null-check it) but it reports
        // "no pointer".
        source->impl_ = std::move(impl);
        return source;
    }

    // Xlib's default error handler calls `exit()`.  The Xfixes query
    // below can fail on a server that does not have the extension
    // enabled, and that must not take the process down.
    x11_detail::install_x11_error_handler();

    impl->has_xfixes = XFixesQueryExtension(impl->display,
                                            &impl->event_base,
                                            &impl->error_base) != 0;
    if (!impl->has_xfixes) {
        core::log::Logger::instance().warn(
            "x11-cursor: the X server has no Xfixes extension, so the "
            "pointer cannot be read");
    }

    source->impl_ = std::move(impl);
    return source;
}

#else // !NIMRTC_X11_CURSOR

struct X11CursorSource::Impl {};

X11CursorSource::X11CursorSource() noexcept = default;

X11CursorSource::~X11CursorSource() = default;

bool X11CursorSource::has_live_cursor() const noexcept {
    return false;
}

void X11CursorSource::set_screen_origin(std::int32_t x,
                                        std::int32_t y) noexcept {
    // The portable stub records the origin exactly as the X11
    // implementation does.  There is no pointer to report here, but
    // the field must be *written and read* on this configuration, or
    // the compiler is right that it is unused and Clang's
    // -Wunused-private-field (with -Werror) fails the build.
    // `[[maybe_unused]]` is not an alternative: GCC ignores that
    // attribute on a non-static data member and rejects the build
    // under -Werror=attributes instead.
    std::lock_guard<std::mutex> lock(mutex_);
    origin_x_ = x;
    origin_y_ = y;
}

CursorState X11CursorSource::query() noexcept {
    // The origin is read back here so that the field is read on this
    // configuration as well as written: a field that is only ever
    // written is still unused as far as the compiler is concerned.
    std::lock_guard<std::mutex> lock(mutex_);
    (void)origin_x_;
    (void)origin_y_;
    return CursorState{};
}

std::unique_ptr<ICursorSource> make_x11_cursor_source() {
    // No X11: there is no pointer to read.  A null source says exactly
    // that, and the caller's null check is the portable behaviour.
    return nullptr;
}

#endif // NIMRTC_X11_CURSOR

} // namespace nimrtc::video_capture
