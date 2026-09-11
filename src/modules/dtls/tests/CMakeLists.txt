# modules/dtls/tests/CMakeLists.txt
# =============================================================================
# DTLS module unit tests.
# =============================================================================
# As of the wolfSSL-only migration, all DtlsSession tests live in
# src/modules/dtls/tests/ via the public DtlsSession class (which
# delegates to DtlsSessionWolfSSL).  The pre-migration BCrypt-direct
# tests have been folded into this directory; nothing under
# tests/ at the project root is BCrypt-specific any more — the
# Windows-only sanity-check files (test_dtls_gcm_aead.cpp /
# test_dtls_inproc.cpp / test_dtls_x25519.cpp) are governed by
# tests/CMakeLists.txt at the repo root and gated on _WIN32.
#
#