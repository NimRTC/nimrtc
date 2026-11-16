// src/modules/rd_input/src/ref_frame_binder.cpp
// ----------------------------------------------------------------------------
// The input <-> frame binding, module half (remote-desktop plan G5).
// ----------------------------------------------------------------------------

#include "nimrtc/rd_input/ref_frame_binder.hpp"

namespace nimrtc::rd_input {

RefFrameBinder::RefFrameBinder(Provider provider) : provider_{std::move(provider)} {}

bool RefFrameBinder::stamp(InputEvent& event) const {
    const auto id = current();
    event.ref_frame = id;
    return id.has_value();
}

InputEvent RefFrameBinder::stamped(InputEvent event) const {
    stamp(event);
    return event;
}

std::optional<std::uint64_t> RefFrameBinder::current() const {
    if (!provider_) {
        return std::nullopt;
    }
    return provider_();
}

bool RefFrameBinder::has_provider() const noexcept {
    return static_cast<bool>(provider_);
}

} // namespace nimrtc::rd_input
