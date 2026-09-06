// SPDX-License-Identifier: MIT
//
// @file nimrtc/dtls/dtls_prf.hpp
// @brief TLS 1.2 PRF (P_SHA256, RFC 5246 §5) — extracted from dtls.cpp so it
//        can be unit-tested directly against RFC known-answer vectors.
//
// The PRF is shared between NimRTC's DTLS handshake (`dtls.cpp`) and the
// Known-Answer-Test executable (`tests/test_dtls_prf_kat.cpp`).  Keeping it
// in a single translation unit prevents the test from drifting out of sync
// with the live implementation.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace nimrtc::dtls {

// ---------------------------------------------------------------------------
// PRF primitives (RFC 5246 §5)
// ---------------------------------------------------------------------------

/** P_hash(secret, label || seed) — HMAC-SHA-256 chained expansion.
 *  This is the building block of the TLS 1.2 PRF.
 *
 *  @param secret  the input secret (e.g. pre_master_secret, master_secret)
 *  @param label   the protocol label ("master secret", "key expansion", …)
 *  @param seed1   first seed chunk (e.g. ClientHello.random)
 *  @param seed2   second seed chunk (e.g. ServerHello.random); may be empty
 *  @param out_len number of bytes to produce (truncated to this length)
 *  @return exactly `out_len` bytes on success, empty vector on HMAC failure
 */
std::vector<std::uint8_t>
tls_prf_p_sha256(std::span<const std::uint8_t> secret,
                 std::span<const std::uint8_t> label,
                 std::span<const std::uint8_t> seed1,
                 std::span<const std::uint8_t> seed2,
                 std::size_t out_len) noexcept;

/** Convenience wrapper for the TLS 1.2 single-seed PRF form:
 *      PRF(secret, label, seed) = P_hash(secret, label || seed)
 *  Used by master_secret, key_block, EXTRACTOR-dtls_srtp, and verify_data
 *  computations.
 */
std::vector<std::uint8_t>
tls12_prf(std::span<const std::uint8_t> secret,
          std::string_view label,
          std::span<const std::uint8_t> seed,
          std::size_t out_len) noexcept;

// ---------------------------------------------------------------------------
// HMAC-SHA-256 helper (exposed so the KAT test can drive it directly).
// ---------------------------------------------------------------------------

/** HMAC-SHA-256(key, data) per RFC 2104 / FIPS 198-1.
 *  @return 32-byte MAC, or empty vector on failure. */
std::vector<std::uint8_t>
dtls_hmac_sha256(std::span<const std::uint8_t> key,
                 std::span<const std::uint8_t> data) noexcept;

} // namespace nimrtc::dtls
