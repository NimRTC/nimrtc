// test_cert_dump.cpp — directly invoke DtlsSession::open() so that
// build_self_signed_cert() runs, then print the resulting local cert
// (fingerprint + hex of full DER) to stdout for offline analysis.
//
// Build: see scripts/diag_build.bat (or just link into nimrtc_dtls_test).
#include <gtest/gtest.h>

#include <cstdio>
#include <span>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/dtls/dtls_prf.hpp>

using namespace nimrtc;
using namespace nimrtc::dtls;

namespace {
inline std::span<const std::uint8_t> as_bytes(std::vector<std::uint8_t>& v) {
    return std::span<const std::uint8_t>(v.data(), v.data() + v.size());
}
} // namespace

TEST(DtlsCertDump, OpenAndPrintCert) {
    Config c;
    c.role = DtlsRole::Client;  // any role works; we just want open()
    DtlsSession s(c);
    auto rc = s.open();
    ASSERT_TRUE(rc.ok()) << rc.error().message();

    const auto& fp = s.local_fingerprint();
    std::printf("FINGERPRINT sha-256: %s\n", fp.hex_colon.c_str());

    // DtlsSession has no public local_cert_der accessor, so instead we
    // dump the SPKI digest (which is what Chrome sees in the cert) via
    // the fingerprint.  The full DER hex was already written to the log
    // by the "dtls: built self-signed cert der_len=... der_hex=..." line.
    std::fflush(stdout);
}
