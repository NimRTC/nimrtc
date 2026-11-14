/**
 * @file nimrtc/video_bitrate_control/factory.hpp
 * @brief The factory for the built-in video rate controller.
 *
 * @note Remote-desktop plan D1.  Interface is stable.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <string_view>

#include <nimrtc/plugins/video_bitrate_controller.hpp>

namespace nimrtc::video_bitrate_control {

/**
 * @brief The factory for the built-in controller.
 *
 * The factory registers under the id `"video-bitrate-control"`.
 */
class NIMRTC_API_STABLE BitrateControllerFactory final
    : public plugins::IVideoBitrateControllerFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override;
    [[nodiscard]] std::string_view display_name() const noexcept override;

    plugins::IVideoBitrateController* create(
        plugins::VideoBitrateControllerConfig cfg) const override;
};

/**
 * @brief Register the built-in controller with `core::PluginRegistry`.
 *
 * Idempotent.
 */
NIMRTC_API_STABLE void register_default_plugins() noexcept;

} // namespace nimrtc::video_bitrate_control
