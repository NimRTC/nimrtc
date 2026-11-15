/**
 * @file src/modules/rd_input_inject/src/stub_injector.hpp
 * @brief The stub backend — the "this platform has no injector" answer.
 *
 * ## Why a stub and not a link error
 *
 * `create_input_injector()` is documented to always return an injector.
 * A host that asks for one on a platform NimRTC does not support gets
 * a backend that is *available() == false* and explains why — rather
 * than a link error at build time, or a null pointer at run time.
 *
 * The stub also has a real job on the platforms that *do* have a
 * backend: it is the fallback when the platform backend is compiled in
 * but not usable (the macOS accessibility permission was never
 * granted, the Linux host has neither XTEST nor uinput).  In that case
 * the stub carries the platform backend's reason.
 *
 * The stub is **portable** — no OS header, no OS call — so it is
 * compiled and unit-tested on every CI leg.
 */

#pragma once

#include <string>
#include <utility>

#include <nimrtc/rd_input_inject/input_injector.hpp>

namespace nimrtc::rd_input_inject::detail {

/** The stub backend. */
class StubInjector final : public InputInjector {
public:
    explicit StubInjector(std::string reason) : reason_(std::move(reason)) {}

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "stub";
    }

    [[nodiscard]] bool available() const noexcept override { return false; }

    [[nodiscard]] std::string unavailable_reason() const noexcept override {
        return reason_;
    }

    InjectionResult inject(const rd_input::InputEvent& event) override {
        (void)event;
        // The stub refuses everything: it never reaches an OS, so
        // "injected" would be a lie.
        ++stats_.refused;
        return InjectionResult::kRefused;
    }

    std::size_t release_all() override {
        // Nothing is ever down, so nothing is ever released.
        return 0;
    }

    [[nodiscard]] InjectionStats stats() const noexcept override {
        return stats_;
    }

    [[nodiscard]] std::size_t keys_down() const noexcept override { return 0; }
    [[nodiscard]] std::size_t buttons_down() const noexcept override { return 0; }
    [[nodiscard]] std::int32_t pointer_x() const noexcept override { return 0; }
    [[nodiscard]] std::int32_t pointer_y() const noexcept override { return 0; }

private:
    std::string reason_;
    InjectionStats stats_{};
};

} // namespace nimrtc::rd_input_inject::detail
