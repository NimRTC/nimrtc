/**
 * @file nimrtc/core/src/pal_default_registrars.cpp
 * @brief PAL Slice 2: explicit self-registration table (PAL architecture §4 Slice 2).
 *
 * ## Purpose
 *
 * `kDefaultRegistrars[]` is the single authoritative list of every built-in
 * plugin's `register_default_plugins()` entry point.  It replaces the inline
 * statement-list body of `core::register_all_default_plugins()` with an
 * iterable array, enabling:
 *
 *   (a) compile-time completeness audits (a test can iterate the array and
 *       assert no two entries have the same factory id),
 *   (b) mechanical grep discoverability: `git grep 'kDefaultRegistrars'`
 *       surfaces the table in one hit,
 *   (c) future extension without editing `registry.hpp` — adding a new module
 *       requires only one new `&module::register_default_plugins` entry here.
 *
 * ## Layout constraint
 *
 * `src/core/` is normally an INTERFACE (header-only) library per Layout
 * Invariant 1 of `docs/architecture.md`.  This source file is the sole
 * exception: `pal_default_registrars.cpp` holds a static-initialised array
 * whose construction must run at program startup, which requires a compiled
 * translation unit.  `src/core/CMakeLists.txt` adds this file to a
 * non-INTERFACE target so it participates in the build.
 *
 * ## Adding a new plugin
 *
 * When a new built-in module `foo` is added:
 *
 *   1. Ensure `foo` declares `namespace nimrtc::foo { void
 *      register_default_plugins() noexcept; }` in its public header.
 *   2. Add `&nimrtc::foo::register_default_plugins` to `kDefaultRegistrars[]`
 *      below (keep the list alphabetical by namespace).
 *   3. If the module is conditional (behind a CMake `option`), guard the
 *      array entry with the same `#ifdef` that guards the existing call
 *      in `registry.hpp`.
 *
 * ## What is NOT registered here
 *
 * - `nimrtc::dtls::register_default_plugins()` — DTLS registration is
 *   currently seam-local (Slice 4, `test_only` slot).  Promoting it to
 *   `core::register_all_default_plugins()` is a Slice 7 / Slice 8 concern.
 */

#include <nimrtc/core/registry.hpp>

namespace nimrtc::core::detail {

// ---------------------------------------------------------------------------
// Type alias — all registration entry points share this signature.
// ---------------------------------------------------------------------------
using RegistrarFn = void(*)() noexcept;

// ---------------------------------------------------------------------------
// kDefaultRegistrars — iterable table of all built-in plugin registrars.
// ---------------------------------------------------------------------------
//
// Entries are listed in the same order as they appeared in the original
// inline body of `register_all_default_plugins()` in `registry.hpp`.
// Guarded entries mirror the `#ifdef` guards that protected the original calls.
//
// Alphabetical order by namespace is preferred for new additions.
constexpr RegistrarFn kDefaultRegistrars[] = {
    &nimrtc::audio3a::register_default_plugins,
    &nimrtc::bwe::register_default_plugins,
#ifdef NIMRTC_HAS_H264
    &nimrtc::h264::register_default_plugins,
#endif
    &nimrtc::ice::register_default_plugins,
    &nimrtc::jb::register_default_plugins,
#ifdef NIMRTC_HAS_OPUS
    &nimrtc::opus::register_default_plugins,
#endif
    &nimrtc::rtp::register_default_plugins,
    &nimrtc::sched::register_default_plugins,
    &nimrtc::sdp::register_default_plugins,
#ifdef NIMRTC_HAS_VIDEO_PIPELINE
    &nimrtc::video_pipeline::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_VIDEO_SINK
    &nimrtc::video_sink::register_default_plugins,
#endif
#ifdef NIMRTC_HAS_VIDEO_SOURCE
    &nimrtc::video_source::register_default_plugins,
#endif
};

// ---------------------------------------------------------------------------
// Iteration helpers — used by tests and by the rewritten
// `register_all_default_plugins()` in `registry.hpp`.
// ---------------------------------------------------------------------------

/** Number of entries in the default-registrar table. */
constexpr std::size_t kDefaultRegistrarCount =
    sizeof(kDefaultRegistrars) / sizeof(kDefaultRegistrars[0]);

} // namespace nimrtc::core::detail
