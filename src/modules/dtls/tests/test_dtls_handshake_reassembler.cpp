// test_dtls_handshake_reassembler.cpp
//
// Direct unit tests for nimrtc::dtls::DtlsHandshakeReassembler — the
// fragment-reassembly shim that lets wolfSSL 5.9.2 (DTLS 1.2) accept
// Chrome 117+'s two-record ClientHello flight.
//
// Coverage:
//   1. Unfragmented record passes through (Result::kPassThrough).
//   2. Forward-order 2-fragment ClientHello reassembles to a single
//      record with the right body and handshake header.
//   3. Reverse-order 2-fragment ClientHello reassembles correctly
//      (out-of-order is allowed).
//   4. A single fragment (msg not yet complete) returns
//      kFragmentBuffered, the buffer is held internally, and the
//      2nd fragment completes the message.
//   5. Defensive sanity checks reject malformed input gracefully
//      (returns kPassThrough, never crashes).
//   6. gc_expired() evicts entries older than the configured TTL.
//   7. clear() drops all partial buffers.
//   8. Multiple concurrent msg_seqs in the buffer are independent.

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include <nimrtc/dtls/dtls_handshake_reassembler.hpp>

using nimrtc::dtls::DtlsHandshakeReassembler;
using Result = DtlsHandshakeReassembler::Result;

namespace {

// Build a DTLS record containing a Handshake message.
//
//   record = 13-byte DTLS header + payload
//   payload = 12-byte Handshake header + body
//
// `frag_off` and `frag_len` are written into the Handshake header as
// a 3-byte big-endian field each.  The DTLS record's length is the
// full payload length (so this helper can build both unfragmented
// records and the per-fragment records Chrome sends).
std::vector<std::uint8_t> make_record(std::uint8_t msg_type,
                                       std::uint32_t msg_len,
                                       std::uint16_t msg_seq,
                                       std::uint32_t frag_off,
                                       std::uint32_t frag_len,
                                       std::span<const std::uint8_t> body,
                                       std::uint8_t record_seq = 0,
                                       std::uint16_t epoch = 0,
                                       std::uint8_t ct = 22) {
    std::vector<std::uint8_t> rec;
    rec.reserve(13 + 12 + body.size());

    // DTLS record header
    rec.push_back(ct);
    rec.push_back(0xfe); rec.push_back(0xff);  // DTLS 1.2
    rec.push_back(static_cast<std::uint8_t>((epoch >> 8) & 0xff));
    rec.push_back(static_cast<std::uint8_t>(epoch & 0xff));
    for (int i = 5; i >= 0; --i) {
        rec.push_back(static_cast<std::uint8_t>((record_seq >> (8 * i)) & 0xff));
    }
    const std::uint16_t payload_len = static_cast<std::uint16_t>(12 + body.size());
    rec.push_back(static_cast<std::uint8_t>((payload_len >> 8) & 0xff));
    rec.push_back(static_cast<std::uint8_t>(payload_len & 0xff));

    // Handshake header
    rec.push_back(msg_type);
    rec.push_back(static_cast<std::uint8_t>((msg_len >> 16) & 0xff));
    rec.push_back(static_cast<std::uint8_t>((msg_len >> 8)  & 0xff));
    rec.push_back(static_cast<std::uint8_t>(msg_len & 0xff));
    rec.push_back(static_cast<std::uint8_t>((msg_seq >> 8) & 0xff));
    rec.push_back(static_cast<std::uint8_t>(msg_seq & 0xff));
    rec.push_back(static_cast<std::uint8_t>((frag_off >> 16) & 0xff));
    rec.push_back(static_cast<std::uint8_t>((frag_off >> 8)  & 0xff));
    rec.push_back(static_cast<std::uint8_t>(frag_off & 0xff));
    rec.push_back(static_cast<std::uint8_t>((frag_len >> 16) & 0xff));
    rec.push_back(static_cast<std::uint8_t>((frag_len >> 8)  & 0xff));
    rec.push_back(static_cast<std::uint8_t>(frag_len & 0xff));

    rec.insert(rec.end(), body.begin(), body.end());
    return rec;
}

// Build a body of `total` bytes filled with a deterministic pattern
// so we can verify reassembly preserved the bytes verbatim.
std::vector<std::uint8_t> make_body(std::size_t total) {
    std::vector<std::uint8_t> b(total);
    for (std::size_t i = 0; i < total; ++i) {
        b[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
    }
    return b;
}

}  // namespace

// 1. Unfragmented Handshake record passes through.
TEST(DtlsHandshakeReassembler, UnfragmentedPassThrough) {
    DtlsHandshakeReassembler r;
    auto body = make_body(100);
    auto rec = make_record(/*msg_type=*/1, /*msg_len=*/100, /*msg_seq=*/0,
                           /*frag_off=*/0, /*frag_len=*/100, body);

    std::vector<std::uint8_t> out;
    auto rc = r.try_reassemble(rec, out);
    EXPECT_EQ(rc, Result::kPassThrough);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(r.size(), 0u);
}

// 2. Forward-order 2-fragment ClientHello reassembles correctly.
TEST(DtlsHandshakeReassembler, ForwardOrderReassembles) {
    DtlsHandshakeReassembler r;
    auto body = make_body(1413);  // Chrome 117+ ClientHello size
    const std::uint32_t first_len = 1107;
    const std::uint32_t body_size = static_cast<std::uint32_t>(body.size());
    auto body_a = std::span<const std::uint8_t>(body.data(), first_len);
    auto body_b = std::span<const std::uint8_t>(body.data() + first_len,
                                                body.size() - first_len);

    auto rec0 = make_record(/*type=*/1, /*msg_len=*/body_size,
                            /*msg_seq=*/0,
                            /*frag_off=*/0, /*frag_len=*/first_len, body_a,
                            /*record_seq=*/0);
    auto rec1 = make_record(/*type=*/1, /*msg_len=*/body_size,
                            /*msg_seq=*/0,
                            /*frag_off=*/first_len,
                            /*frag_len=*/body_size - first_len, body_b,
                            /*record_seq=*/1);

    std::vector<std::uint8_t> out0, out1;
    EXPECT_EQ(r.try_reassemble(rec0, out0), Result::kFragmentBuffered);
    EXPECT_TRUE(out0.empty());
    EXPECT_EQ(r.size(), 1u);

    EXPECT_EQ(r.try_reassemble(rec1, out1), Result::kReassembled);
    EXPECT_EQ(r.size(), 0u);

    // The synthesised record's payload must contain the full body.
    ASSERT_GE(out1.size(), 13 + 12);
    const std::uint8_t* p = out1.data();
    EXPECT_EQ(p[0], 22u);  // Handshake
    const std::uint32_t msg_len = (std::uint32_t(p[14]) << 16) |
                                   (std::uint32_t(p[15]) << 8)  |
                                    std::uint32_t(p[16]);
    EXPECT_EQ(msg_len, body.size());
    const std::uint32_t frag_len = (std::uint32_t(p[22]) << 16) |
                                    (std::uint32_t(p[23]) << 8)  |
                                     std::uint32_t(p[24]);
    EXPECT_EQ(frag_len, body.size());
    const std::uint16_t frag_off = (std::uint16_t(p[19]) << 8) | p[20];
    // frag_off occupies 3 bytes; only 2 are within the rec header span
    // of 3 bytes starting at p[19].
    EXPECT_EQ(frag_off & 0xffff, 0u);

    // The body bytes must match the original.
    EXPECT_EQ(std::memcmp(p + 25, body.data(), body.size()), 0);
}

// 3. Reverse-order 2-fragment ClientHello reassembles correctly.
TEST(DtlsHandshakeReassembler, ReverseOrderReassembles) {
    DtlsHandshakeReassembler r;
    auto body = make_body(1413);
    const std::uint32_t first_len = 1107;
    const std::uint32_t body_size = static_cast<std::uint32_t>(body.size());
    auto body_a = std::span<const std::uint8_t>(body.data(), first_len);
    auto body_b = std::span<const std::uint8_t>(body.data() + first_len,
                                                body.size() - first_len);

    auto rec0 = make_record(1, body_size, 0, 0, first_len, body_a, 0);
    auto rec1 = make_record(1, body_size, 0, first_len,
                            body_size - first_len, body_b, 1);

    std::vector<std::uint8_t> out0, out1;
    // Send the SECOND fragment first.
    EXPECT_EQ(r.try_reassemble(rec1, out0), Result::kFragmentBuffered);
    EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(r.try_reassemble(rec0, out1), Result::kReassembled);
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(std::memcmp(out1.data() + 25, body.data(), body.size()), 0);
}

// 4. Single fragment is buffered; second completes the message.
TEST(DtlsHandshakeReassembler, SingleFragmentIsBuffered) {
    DtlsHandshakeReassembler r;
    auto body = make_body(200);
    const std::uint32_t half = 100;
    auto half_a = std::span<const std::uint8_t>(body.data(), half);

    auto rec0 = make_record(1, 200, 0, 0, half, half_a, 0);
    std::vector<std::uint8_t> out;
    EXPECT_EQ(r.try_reassemble(rec0, out), Result::kFragmentBuffered);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(r.size(), 1u);
}

// 5. Defensive: malformed input never crashes.
TEST(DtlsHandshakeReassembler, MalformedInputsRejectedGracefully) {
    DtlsHandshakeReassembler r;
    std::vector<std::uint8_t> out;

    // Too short for DTLS header.
    EXPECT_EQ(r.try_reassemble(std::span<const std::uint8_t>(
                                  reinterpret_cast<const std::uint8_t*>("\x16"),
                                  1),
                              out),
              Result::kPassThrough);
    // Wrong content type.
    auto alert_rec = make_record(/*type=*/1, 100, 0, 0, 100,
                                 std::span<const std::uint8_t>{});
    alert_rec[0] = 21;  // Alert, not Handshake
    EXPECT_EQ(r.try_reassemble(alert_rec, out), Result::kPassThrough);

    // frag_off beyond msg_len.
    auto bad = make_record(1, 100, 0, /*frag_off=*/200, /*frag_len=*/1,
                           std::span<const std::uint8_t>{});
    EXPECT_EQ(r.try_reassemble(bad, out), Result::kPassThrough);
}

// 6. gc_expired evicts stale entries.
TEST(DtlsHandshakeReassembler, GcExpiresStaleEntries) {
    DtlsHandshakeReassembler r;
    r.set_gc_ttl(std::chrono::milliseconds(50));
    auto body = make_body(200);
    const std::uint32_t half = 100;
    auto half_view = std::span<const std::uint8_t>(body.data(), half);
    auto rec0 = make_record(1, 200, 0, 0, half, half_view, 0);
    std::vector<std::uint8_t> out;
    ASSERT_EQ(r.try_reassemble(rec0, out), Result::kFragmentBuffered);
    EXPECT_EQ(r.size(), 1u);

    // Before TTL, no eviction.
    EXPECT_EQ(r.gc_expired(DtlsHandshakeReassembler::clock::now()), 0u);
    EXPECT_EQ(r.size(), 1u);

    // After TTL, eviction.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(r.gc_expired(DtlsHandshakeReassembler::clock::now()), 1u);
    EXPECT_EQ(r.size(), 0u);
}

// 7. clear() drops everything.
TEST(DtlsHandshakeReassembler, ClearDropsAll) {
    DtlsHandshakeReassembler r;
    auto body = make_body(200);
    const std::uint32_t half = 100;
    auto half_view = std::span<const std::uint8_t>(body.data(), half);
    auto rec0 = make_record(1, 200, 0, 0, half, half_view, 0);
    auto rec1 = make_record(1, 200, 1, 0, half, half_view, 1);  // different msg_seq
    std::vector<std::uint8_t> out;
    ASSERT_EQ(r.try_reassemble(rec0, out), Result::kFragmentBuffered);
    ASSERT_EQ(r.try_reassemble(rec1, out), Result::kFragmentBuffered);
    EXPECT_EQ(r.size(), 2u);
    r.clear();
    EXPECT_EQ(r.size(), 0u);
}

// 8. Different msg_seqs in the buffer are independent.
TEST(DtlsHandshakeReassembler, DifferentMsgSeqsIndependent) {
    DtlsHandshakeReassembler r;
    auto body = make_body(200);
    const std::uint32_t half = 100;
    auto half_view = std::span<const std::uint8_t>(body.data(), half);

    // msg_seq 0: only first fragment seen.
    auto rec0 = make_record(1, 200, 0, 0, half, half_view, 0);
    std::vector<std::uint8_t> out;
    EXPECT_EQ(r.try_reassemble(rec0, out), Result::kFragmentBuffered);
    EXPECT_EQ(r.size(), 1u);

    // msg_seq 0 second fragment — completes msg_seq 0.
    auto other_half = std::span<const std::uint8_t>(body.data() + half, half);
    auto rec0b = make_record(1, 200, 0, half, half, other_half, 1);
    EXPECT_EQ(r.try_reassemble(rec0b, out), Result::kReassembled);
    EXPECT_EQ(r.size(), 0u);
}
