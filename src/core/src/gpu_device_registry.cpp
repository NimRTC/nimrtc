/**
 * @file nimrtc/core/src/gpu_device_registry.cpp
 * @brief GpuDeviceRegistry out-of-line definitions.
 *
 * The singleton lives in this single translation unit for the same
 * reason `PluginRegistry`'s does (see `plugin_registry.cpp`): on
 * MSVC, a function-local static inside an inline function is *not*
 * COMDAT-folded, so every translation unit that included the header
 * would emit its own copy of the registry — and the capture plug-in and
 * the encoder plug-in would end up talking to two different registries.
 *
 * That would be a silent, intermittent failure: the zero-copy path
 * would work in a single-module test and fail in the full engine.
 */

#include <nimrtc/core/gpu_device_registry.hpp>

#include <algorithm>

namespace nimrtc::core {

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

GpuDeviceRegistry& GpuDeviceRegistry::instance() noexcept {
    static GpuDeviceRegistry inst;
    return inst;
}

// ---------------------------------------------------------------------------
// Producer side
// ---------------------------------------------------------------------------

std::size_t GpuDeviceRegistry::publish(std::string_view logical_name,
                                       void* native_device) noexcept {
    if (logical_name.empty() || native_device == nullptr) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : entries_) {
        if (kv.first != logical_name) continue;
        // The first publisher wins: the encoder has already been opened
        // against that device, and swapping it would invalidate the
        // session.  The count still goes up, so the lifetime
        // bookkeeping stays honest.
        ++kv.second.holders;
        return kv.second.holders;
    }
    entries_.emplace_back(std::string(logical_name),
                          Entry{native_device, 1});
    return 1;
}

// ---------------------------------------------------------------------------
// Consumer side
// ---------------------------------------------------------------------------

void* GpuDeviceRegistry::adopt(std::string_view logical_name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : entries_) {
        if (kv.first == logical_name) return kv.second.device;
    }
    return nullptr;
}

void* GpuDeviceRegistry::acquire(std::string_view logical_name) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : entries_) {
        if (kv.first != logical_name) continue;
        ++kv.second.holders;
        return kv.second.device;
    }
    return nullptr;
}

void GpuDeviceRegistry::release(std::string_view logical_name) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first != logical_name) continue;
        if (it->second.holders > 0) --it->second.holders;
        if (it->second.holders == 0) {
            // The last holder is gone: the device is forgotten.  The
            // native object belongs to whoever created it, and that
            // party destroys it through its own platform code.
            entries_.erase(it);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

bool GpuDeviceRegistry::has(std::string_view logical_name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const auto& kv) { return kv.first == logical_name; });
}

std::size_t GpuDeviceRegistry::holder_count(
        std::string_view logical_name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : entries_) {
        if (kv.first == logical_name) return kv.second.holders;
    }
    return 0;
}

std::vector<std::string> GpuDeviceRegistry::logical_names() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) out.push_back(kv.first);
    return out;
}

void GpuDeviceRegistry::clear_for_tests() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

} // namespace nimrtc::core
