/**
 * @file src/dtls/src/dtls_wolfssl_session_iface.hpp
 * @brief Forwarder header — exposes the refactored DtlsSessionWolfSSL
 *        under the PAL Slice 4 seam module's include path.
 *
 * ## What lives here
 *
 * The wolfSSL-backed DTLS implementation is `DtlsSessionWolfSSL`,
 * defined in:
 *
 *   src/modules/dtls/include/nimrtc/dtls/dtls_wolfssl_session.hpp
 *
 * As part of PAL Slice 4, that class was refactored in place to
 * inherit `IDtlsSession` (the new seam).  This header is the seam
 * module's re-export point so consumers can write:
 *
 *   #include <nimrtc/dtls/dtls_wolfssl_session_iface.hpp>
 *
 * and pull in *both* the seam interface (IDtlsSession) and the
 * wolfSSL-backed concrete class — without having to remember which
 * sub-tree the wolfSSL implementation actually lives under.
 *
 * ## Why a separate file
 *
 * Keeps the seam module's public surface (`dtls_session_iface.hpp`,
 * `dtls_session_factory.hpp`, `dtls_plugin.hpp`,
 * `dtls_wolfssl_session_iface.hpp`) discoverable from a single
 * include root: `src/dtls/include/nimrtc/dtls/`.  A consumer that
 * wants to instantiate the default backend directly (rather than via
 * a factory) doesn't have to know about the `modules/` subtree.
 *
 * ## Slice 4 boundaries
 *
 *  - engine.cpp is NOT touched (Slice 8 will route through
 *    IDtlsSessionFactory).
 *  - The existing wolfSSL implementation is preserved verbatim;
 *    only the inheritance declaration changed (DtlsSessionWolfSSL
 *    now derives from IDtlsSession) and the seam methods
 *    (start/pump/on_handshake_complete/export_srtp_key_material)
 *    were added as override-flavoured delegating wrappers.
 *
 * @note P1 — seam-side forwarder added as part of PAL Slice 4
 *       (v0.11.0).
 */

#ifndef NIMRTC_DTLS_WOLFSSL_SESSION_IFACE_HPP
#define NIMRTC_DTLS_WOLFSSL_SESSION_IFACE_HPP

// Pull in the refactored wolfSSL-backed concrete class (which now
// inherits IDtlsSession).  Re-exported under the seam include root
// for consumer convenience — see the file header for rationale.
//
// The concrete header pulls in:
//   - nimrtc/dtls/dtls.hpp              (DtlsConfig / DtlsState / SrtpKeyingMaterial / ...)
//   - nimrtc/dtls/dtls_session_iface.hpp (IDtlsSession — the PAL Slice 4 seam)
#include <nimrtc/dtls/dtls_wolfssl_session.hpp>

// No additional declarations here on purpose: this file is a
// re-export point, not a parallel definition site.  Keeping it
// header-only avoids a duplicate-class-definition ODR violation
// across the two include roots.

#endif // NIMRTC_DTLS_WOLFSSL_SESSION_IFACE_HPP
