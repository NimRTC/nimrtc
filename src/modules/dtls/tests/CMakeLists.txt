# modules/dtls/tests/CMakeLists.txt
# =============================================================================
# DTLS module unit tests.
# =============================================================================
# All DtlsSession (BCrypt) tests were removed as part of the wolfSSL-only
# migration (dtls.cpp deletion).  BCrypt-direct tests (GCM/INPROC/X25519)
# that do NOT depend on DtlsSession class are retained as standalone targets.
#