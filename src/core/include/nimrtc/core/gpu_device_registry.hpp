/**
 * @file nimrtc/core/gpu_device_registry.hpp
 * @brief The per-process GPU device registry — the zero-copy rendezvous
 *        (remote-desktop plan B10).
 *
 * ## Why this exists
 *
 * The zero-copy path has a shape that does not fit any of the existing
 * seams:
 *
 * @code
 *   capture device ──► GPU surface ──► hardware encoder
 *   (DXGI / WGC)                        (NVENC / QSV / VAAPI)
 * @endcode
 *
 * The producer and the consumer are two *independent* plug-ins.  They
 * do not know about each other, they are not constructed by the same
 * factory, and — crucially — they usually do not even live on the same
 * GPU *device*, even when they are on the same physical GPU.
 *
 * That last point is the whole problem.  A D3D11 surface belongs to the
 * `ID3D11Device` that created it.  An NVENC encode session is opened
 * against one specific `ID3D11Device`.  Registering a texture with a
 * session that was opened against a *different* device fails — the
 * driver refuses, because the two devices do not share a resource
 * namespace.
 *
 * So the producer and the consumer must agree on **one** device.  The
 * producer (the capture device) creates the device, because it has to
 * create the surface in the first place; the consumer (the encoder)
 * has to *adopt* that device instead of creating its own.
 *
 * This registry is that rendezvous.  It maps a *logical* GPU device key
 * onto the concrete native device, and hands out a shared, refcounted
 * handle so both ends use the same object.
 *
 * ## The shape
 *
 * The registry is deliberately *not* a plug-in seam: it is a process-
 * wide fact ("which GPU device is the screen-capture device?"), and
 * the project rule is that a process-wide singleton lives in
 * `nimrtc::core::` (AGENTS.md L6).
 *
 * It is also deliberately *opaque*: the registry never interprets the
 * native device pointer.  It only:
 *
 *   - remembers which logical key maps to which native device,
 *   - hands the device out to whoever asks for that key, and
 *   - counts the users so the device can be released when the last one
 *     goes away.
 *
 * The *interpretation* ("this is an `ID3D11Device*`, and to share it
 * with another device you need a shared handle") belongs to the
 * platform layer — the Windows D3D11 code in the capture and codec
 * modules — not here.
 *
 * ## Lifetime
 *
 * A device is created by whoever gets there first, and destroyed when
 * the last holder releases it.  The registry holds a *reference* to the
 * device (it does not own it); the holder that created it owns the
 * native object and destroys it when the count reaches zero.
 *
 * @note Remote-desktop plan B10 / RD-4.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::core {

// ---------------------------------------------------------------------------
// GpuDeviceRegistry
// ---------------------------------------------------------------------------

/**
 * @brief The process-wide map from a *logical* GPU device to the concrete
 *        native device both ends of the zero-copy path must share.
 *
 * A "logical device" is a stable, human-meaningful name for a GPU
 * context: "screen-capture" (the device the capture plug-in created),
 * "encoder" (the device the hardware encoder opened its session
 * against), or a host-defined name.  The registry is what lets the
 * second of those *adopt* the first one's device instead of creating
 * its own.
 *
 * Typical use:
 *
 * @code
 *   // The capture plug-in (first to arrive) creates the device:
 *   auto* dev = GpuDeviceRegistry::instance().acquire("screen-capture",
 *                                                      native_device);
 *
 *   // The encoder plug-in (second to arrive) adopts it:
 *   void* shared = GpuDeviceRegistry::instance().adopt("screen-capture");
 *   if (shared == nullptr) {
 *       // No capture device published one — open an encode session
 *       // against a private device instead.
 *   }
 * @endcode
 *
 * Thread safety: all methods are internally synchronised.  The capture
 * plug-in and the encoder plug-in are initialised from different
 * threads on different platforms (the capture thread owns the capture
 * device; the engine's tick thread owns the codec), so the registry has
 * to tolerate concurrent access.
 */
class NIMRTC_API_STABLE GpuDeviceRegistry {
public:
    /** The process-wide registry. */
    static GpuDeviceRegistry& instance() noexcept;

    // ---- Producer side ----------------------------------------------------

    /**
     * Publish a native device under a logical name.
     *
     * The *first* publisher for a name wins: a second publish under the
     * same name does **not** replace the device — the encoder has
     * already been opened against the first one, and swapping it out
     * would invalidate the session.  The second publisher gets its own
     * reference count so the lifetime bookkeeping stays honest.
     *
     * @param logical_name  The logical name ("screen-capture", ...).
     * @param native_device The concrete device pointer (opaque here).
     * @return The number of holders after this call.
     */
    std::size_t publish(std::string_view logical_name,
                        void* native_device) noexcept;

    // ---- Consumer side ----------------------------------------------------

    /**
     * The native device published under @p logical_name, or nullptr.
     *
     * The caller does **not** own the returned pointer: the registry
     * keeps it alive for as long as any holder exists.  A caller that
     * wants to keep using the device past the session should call
     * `acquire()` instead.
     */
    [[nodiscard]] void* adopt(std::string_view logical_name) const noexcept;

    /**
     * Take a reference to the device published under @p logical_name.
     *
     * @return The native device pointer, or nullptr when nothing is
     *         published under that name.
     */
    void* acquire(std::string_view logical_name) noexcept;

    /**
     * Release one reference taken by `acquire()` (or by `publish()`).
     *
     * When the last holder releases it, the registry *forgets* the
     * device.  The native object itself is owned by whoever created it
     * (the publisher), which destroys it when its own count reaches
     * zero.
     */
    void release(std::string_view logical_name) noexcept;

    // ---- Introspection ----------------------------------------------------

    /** True iff a device is published under @p logical_name. */
    [[nodiscard]] bool has(std::string_view logical_name) const noexcept;

    /** The number of holders of the device under @p logical_name. */
    [[nodiscard]] std::size_t holder_count(
        std::string_view logical_name) const noexcept;

    /** The logical names that currently have a device. */
    [[nodiscard]] std::vector<std::string> logical_names() const noexcept;

    /** Forget every device.  Test-only. */
    void clear_for_tests() noexcept;

private:
    GpuDeviceRegistry() = default;
    GpuDeviceRegistry(const GpuDeviceRegistry&)            = delete;
    GpuDeviceRegistry& operator=(const GpuDeviceRegistry&) = delete;

    struct Entry {
        void*       device = nullptr;
        std::size_t holders = 0;
    };

    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, Entry>> entries_;
};

} // namespace nimrtc::core
