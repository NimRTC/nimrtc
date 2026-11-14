// modules/video_capture/src/platform/linux/x11_cursor.cpp
// ----------------------------------------------------------------------------
// The X11 cursor source (remote-desktop plan A6).
//
// The X11 capture path (plan A5) uses `XGetImage`, which returns the
// root window's pixels *without* the pointer — the pointer is drawn by
// the X server on top of the framebuffer, not into it.  So the X11 path
// needs the cursor layer, and the modern way to get the pointer's
// bitmap is **XFixes**:
//
//   `XFixesGetCursorImage` returns the cursor's image in the server's
//   own pixel format, together with the hotspot and the atom name of
//   the cursor.
//
// The image is in whatever visual the server uses (usually 32-bit
// ARGB, sometimes 24-bit), so it is converted to the compositor's BGRA
// layout here.  The conversion is the only interesting part: the
// portable compositor must not have to know about X11 pixel layouts.
//
// ## The monochrome cursor
//
// A monochrome cursor is sent by the server as a 1-bit-per-pixel image
// whose pixels are 0 (drawn) or 1 (transparent).  The convention is
// handled here, so the compositor always receives a 32-bit BGRA image
// with a meaningful alpha.
//
// ## The coordinates
//
// The X11 cursor's position is in **root-window** coordinates, which is
// what a root-window capture frame uses, so no translation is needed
// here.  A capture of a single window would need one; the source does
// not guess.
//
// @note Remote-desktop plan A6.  Platform adapter — the portable half
//       lives in `src/cursor_compositor.cpp`.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/cursor_source.hpp>

#include <cstring>
#include <vector>

#if defined(__linux__)

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

namespace nimrtc::video_capture {

namespace {

/**
 * @brief The X11 (XFixes) cursor source.
 *
 * It keeps the X display open for its whole life (opening one per
 * frame would be far too expensive).
 */
class X11CursorSource final : public ICursorSource {
  public:
    X11CursorSource() = default;

    ~X11CursorSource() override {
        if (display_ != nullptr) {
            ::XCloseDisplay(display_);
        }
    }

    X11CursorSource(const X11CursorSource&) = delete;
    X11CursorSource& operator=(const X11CursorSource&) = delete;

    [[nodiscard]] std::string_view id() const noexcept override { return "x11-cursor"; }

    [[nodiscard]] CursorSourceStatus poll(CursorSample& out) noexcept override {
        if (!ensure_display()) return CursorSourceStatus::kUnavailable;

        // The pointer's position: a root-window query, which is the
        // coordinate space a root-window capture frame uses.
        ::Window root = 0;
        ::Window child = 0;
        int root_x = 0;
        int root_y = 0;
        int win_x = 0;
        int win_y = 0;
        unsigned int mask = 0;
        if (!::XQueryPointer(display_, root_default_, &root, &child, &root_x, &root_y, &win_x,
                             &win_y, &mask)) {
            return CursorSourceStatus::kUnavailable;
        }
        out.screen_x = root_x;
        out.screen_y = root_y;
        out.hidden = false;

        // The cursor's image.  XFixes hands out a *copy*, so it is
        // freed right after the conversion.
        XFixesCursorImage* image = ::XFixesGetCursorImage(display_);
        if (image == nullptr) return CursorSourceStatus::kUnavailable;

        const bool converted = convert(image, out.image);
        ::XFree(image);

        if (!converted) return CursorSourceStatus::kUnavailable;
        return CursorSourceStatus::kOk;
    }

  private:
    bool ensure_display() noexcept {
        if (display_ != nullptr) return true;

        display_ = ::XOpenDisplay(nullptr);
        if (display_ == nullptr) return false;

        int event_base = 0;
        int error_base = 0;
        if (!::XFixesQueryExtension(display_, &event_base, &error_base)) {
            // The XFixes extension is not present: an old server.  The
            // cursor is then unavailable, and the pinned fallback takes
            // over.
            ::XCloseDisplay(display_);
            display_ = nullptr;
            return false;
        }

        root_default_ = ::DefaultRootWindow(display_);
        screen_number_ = ::DefaultScreen(display_);
        return true;
    }

    /**
     * @brief Convert an XFixes cursor image into the BGRA image.
     */
    static bool convert(XFixesCursorImage* image, CursorImage& out) noexcept {
        if (image == nullptr || image->width <= 0 || image->height <= 0) return false;

        const int width = image->width;
        const int height = image->height;
        const std::size_t pixels_per_row = static_cast<std::size_t>(image->width) *
                                          static_cast<std::size_t>(image->pixels_per_row);
        const std::size_t stride = pixels_per_row * 4u;

        std::vector<std::uint8_t> pixels(stride * static_cast<std::size_t>(height));

        // The image's depth says how the pixels are laid out.  A
        // depth of 1 is a monochrome cursor, whose pixels are 0 or 1
        // and whose "alpha" is implied.
        const bool monochrome = (image->depth == 1);

        for (int y = 0; y < height; ++y) {
            const std::uint8_t* src_row =
                image->pixels + static_cast<std::size_t>(y) * pixels_per_row;
            std::uint8_t* dst_row = pixels.data() + static_cast<std::size_t>(y) * stride;

            for (int x = 0; x < width; ++x) {
                std::uint8_t* dst = dst_row + static_cast<std::size_t>(x) * 4u;

                if (monochrome) {
                    // One bit per pixel: the byte's high bit is the
                    // left-most pixel on a big-endian server and the
                    // low bit on a little-endian one.  The pixel value
                    // is 0 (drawn) or 1 (transparent).
                    const std::size_t byte_index = static_cast<std::size_t>(x) / 8u;
                    if (byte_index >= pixels_per_row) continue;
                    const std::uint8_t byte = src_row[byte_index];
                    const int shift = (x % 8);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
                    const std::uint8_t bit =
                        static_cast<std::uint8_t>((byte >> (7 - shift)) & 0x01u);
#else
                    const std::uint8_t bit = static_cast<std::uint8_t>((byte >> shift) & 0x01u);
#endif
                    // In the monochrome convention a 1 means "the
                    // background shows through".
                    dst[0] = dst[1] = dst[2] = 0x00;
                    dst[3] = (bit != 0) ? 0x00 : 0xFF;
                    continue;
                }

                // A 32-bit ARGB image.
                const std::uint8_t* src = src_row + static_cast<std::size_t>(x) * 4u;
                dst[0] = src[0];  // B
                dst[1] = src[1];  // G
                dst[2] = src[2];  // R
                dst[3] = src[3];  // A
            }
        }

        out.width = static_cast<std::uint32_t>(width);
        out.height = static_cast<std::uint32_t>(height);
        out.hotspot_x = image->xhot;
        out.hotspot_y = image->yhot;
        out.stride = static_cast<std::int32_t>(stride);
        out.pixels = std::move(pixels);
        return out.valid();
    }

    ::Display* display_ = nullptr;
    ::Window root_default_ = 0;
    int screen_number_ = -1;
};

} // anonymous namespace

std::shared_ptr<ICursorSource> make_x11_cursor_source() noexcept {
    return std::make_shared<X11CursorSource>();
}

} // namespace nimrtc::video_capture

#endif // defined(__linux__)
