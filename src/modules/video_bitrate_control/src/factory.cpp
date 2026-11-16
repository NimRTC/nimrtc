/**
 * @file src/factory.cpp
 * @brief The factory for the built-in video rate controller.
 *
 * Remote-desktop plan D1.
 */

#include <nimrtc/video_bitrate_control/factory.hpp>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_bitrate_control/controller.hpp>

namespace nimrtc::video_bitrate_control {

std::string_view BitrateControllerFactory::id() const noexcept {
    return "video-bitrate-control";
}

std::string_view BitrateControllerFactory::display_name() const noexcept {
    return "NimRTC video bitrate controller (the resolution / frame-rate / "
           "bitrate ladder)";
}

plugins::IVideoBitrateController* BitrateControllerFactory::create(
        plugins::VideoBitrateControllerConfig cfg) const {
    return new BitrateController(std::move(cfg));
}

namespace {

const BitrateControllerFactory kFactory;

} // namespace

void register_default_plugins() noexcept {
    static bool done = false;
    if (done) return;
    done = true;
    core::PluginRegistry::instance().register_video_bitrate_controller(
        kFactory.id(), &kFactory);
}

} // namespace nimrtc::video_bitrate_control
