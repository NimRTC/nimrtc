/**
 * @file nimrtc/plugins/registry.hpp
 * @brief Umbrella header: the plugin registry plus every `nimrtc/plugins/`
 *        seam interface.
 *
 * ## Why this header exists
 *
 * `<nimrtc/core/registry.hpp>` used to include all 13 `nimrtc/plugins/`
 * seam headers as a "backward-compatibility shim".  The effect was that
 * every translation unit that touched the registry also transitively
 * pulled in the whole plugin-interface surface, and — because the same
 * shim also included the DTLS / SCTP / raw_udp / transport-stack /
 * WebTransport factory headers — the whole OPTIONAL-module surface as
 * well.  That defeated the compile-time module trimming that the
 * `NIMRTC_MODULE_*` / `NIMRTC_PLUGINS_*` CMake options provide and made
 * the core layer depend on modules it must not know about
 * (ARCHITECTURE.md Layout Invariant 1).
 *
 * The registry header is now layered properly:
 *
 *   - `<nimrtc/core/registry.hpp>`  -- the registry itself.  Includes
 *     nothing but the core headers; the factory interfaces are
 *     forward-declared, which is all the registry API needs (it stores
 *     and hands out POINTERS).
 *   - `<nimrtc/plugins/registry.hpp>` (this file) -- the registry plus
 *     the 13 `nimrtc/plugins/` seam interfaces.  Include it when you
 *     want the convenience of the old one-header behaviour, or when
 *     you need the seam interfaces themselves.
 *   - the optional-module seams (`<nimrtc/dtls/dtls_session_factory.hpp>`,
 *     `<nimrtc/sctp/sctp_socket_factory.hpp>`,
 *     `<nimrtc/raw_udp/raw_udp_factory_iface.hpp>`,
 *     `<nimrtc/transport/transport_stack.hpp>`,
 *     `<nimrtc/web_transport/web_transport_session_factory.hpp>`) --
 *     included directly by the code that needs them.  They are NOT part
 *     of this umbrella on purpose: they live in optional modules, and
 *     including them here would re-introduce the coupling the split
 *     removed.
 *
 * ## Include-what-you-use
 *
 * Prefer including the specific seam header you use
 * (`<nimrtc/plugins/scheduler.hpp>`, `<nimrtc/plugins/bwe.hpp>`, ...)
 * over this umbrella.  This file exists for convenience and for
 * back-compatibility with code that relied on the old transitive
 * includes.
 *
 * @note Thread-safe for get_*() calls. register_*() is not thread-safe
 * and must be called before any engine is created.
 *
 * @note Stability classification: NIMRTC_API_STABLE.  This umbrella
 *       introduces NO new public symbols -- it only re-exports the
 *       headers listed below, and each of those keeps its own
 *       classification.  The umbrella itself is STABLE because the set
 *       of headers it pulls in is part of the published surface.
 */

#ifndef NIMRTC_PLUGINS_REGISTRY_HPP
#define NIMRTC_PLUGINS_REGISTRY_HPP

// Stability marker used in the @note below.
#include <nimrtc/core/api_version.hpp>

// The registry itself (core layer).  The factory interfaces below are
// forward-declared there; including the real headers here makes them
// complete for callers that need them.
#include <nimrtc/core/registry.hpp>

// The 13 nimrtc/plugins/ seam interfaces.
#include <nimrtc/plugins/transport.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/rtp.hpp>
#include <nimrtc/plugins/sdp.hpp>
#include <nimrtc/plugins/jb.hpp>
#include <nimrtc/plugins/audio3a.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_source.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/bwe.hpp>
#include <nimrtc/plugins/scheduler.hpp>
// v1.2.0 M1 (F2): the network-feedback collector seam.
#include <nimrtc/plugins/net_feedback.hpp>
// v1.2.0 (audio pipeline): the codec-side decoder and the pipeline-side
// receiver seams.
#include <nimrtc/plugins/audio_decoder.hpp>
#include <nimrtc/plugins/audio_receiver.hpp>

#endif // NIMRTC_PLUGINS_REGISTRY_HPP
