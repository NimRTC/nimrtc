// dtls_handshake_reassembler.hpp
//
// DTLS Handshake-layer fragment reassembler — see
// src/modules/dtls/src/dtls_wolfssl_session.cpp for context.
//
// ## Why this exists
//
// wolfSSL 5.9.2's DTLS 1.2 path does NOT reassemble fragmented
// Handshake records (the `WOLFSSL_DTLS_CH_FRAG` compile-time switch
// is gated on `WOLFSSL_DTLS13 && WOLFSSL_HAVE_MLKEM` per
// wolfssl/wolfcrypt/settings.h:4710-4713 and our build pins to
// DTLS 1.2 explicitly).  Chrome 117+ always splits its first
// ClientHello flight across two records (~1100 B + ~300 B), so
// without this shim wolfSSL silently drops the fragments and never
// produces a ServerHello.  The shim intercepts every Handshake
// record coming through feed_inbound(), buffers fragments keyed by
// msg_seq, and synthesises a fresh unfragmented DTLS record once
// the full body has been received.
//
// ## Threading
//
// Single-threaded use only — the owning DtlsSession serialises all
// DTLS I/O on the engine thread, and the reassembler must be fed
// strictly in-order with respect to fragment reception (out-of-order
// IS allowed, but interleaving with another handshake is not).
//
// ## Stability
//
// UNSTABLE — internal helper to the v0.12.0 Chrome interop fix
// (PR https://github.com/NimRTC/nimrtc/pull/19).  Exposed via
// the public include path only because DtlsSession holds it
// by-value; consumers should not depend on the type directly.
// See ADR-015 §"internal-helpers".

#pragma once

#include <nimrtc/core/api_version.hpp>  // NIMRTC_API_UNSTABLE

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <vector>

namespace nimrtc::dtls {

class NIMRTC_API_UNSTABLE DtlsHandshakeReassembler {
public:
    // TTL for a partial fragment buffer before it is garbage-collected.
    // Default: 5 s.  RFC 6347 §4.2.4 sets the maximum DTLS retransmit
    // interval at 60 s; 5 s is well inside that envelope and roughly
    // matches the time a normal WebRTC ICE+DTLS handshake takes to
    // fail over after the peer crashes.  Configurable via
    // set_gc_ttl() so the engine can tighten it for unit tests.
    using clock = std::chrono::steady_clock;

    DtlsHandshakeReassembler() = default;

    void set_gc_ttl(std::chrono::milliseconds ttl) { gc_ttl_ = ttl; }
    std::size_t size() const { return bufs_.size(); }

    void clear() {
        for (auto& [_, buf] : bufs_) {
            buf.received.clear();
            buf.body.clear();
        }
        bufs_.clear();
    }

    // Drop partial messages older than `gc_ttl_` (measured from the
    // most recent fragment).  Returns the number of entries evicted.
    std::size_t gc_expired(clock::time_point now) {
        std::size_t evicted = 0;
        for (auto it = bufs_.begin(); it != bufs_.end(); ) {
            if (now - it->second.last_seen >= gc_ttl_) {
                it = bufs_.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        return evicted;
    }

    // Result of feeding one DTLS record into the reassembler.
    enum class Result : int {
        kPassThrough      = 0,  // not a fragment — feed the original
        kFragmentBuffered = 1,  // fragment stored, message not yet
                                // complete — drop the original
        kReassembled      = 2,  // message complete — `out_reassembled`
                                // holds a fresh unfragmented record;
                                // feed IT instead of the original
    };

    // Feed a DTLS record.  See the enum above for return semantics.
    //
    // Pre-conditions:
    //  * `record` is one or more complete DTLS records (the caller is
    //    expected to have already framed them).  Only the first record
    //    in the span is examined.
    //  * `out_reassembled` is cleared on entry and only populated on
    //    `kReassembled`.
    Result try_reassemble(std::span<const std::uint8_t> record,
                          std::vector<std::uint8_t>& out_reassembled) {
        out_reassembled.clear();
        if (record.size() < 13) return Result::kPassThrough;
        if (record[0] != 22)    return Result::kPassThrough;

        // Parse DTLS record header.
        const std::uint16_t epoch = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(record[3]) << 8) | record[4]);
        std::uint64_t seq = 0;
        for (int i = 0; i < 6; ++i) seq = (seq << 8) | record[5 + i];
        const std::uint16_t rec_len = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(record[11]) << 8) | record[12]);
        if (record.size() < 13u + rec_len) return Result::kPassThrough;
        const auto payload = record.subspan(13, rec_len);
        if (payload.size() < 12) return Result::kPassThrough;

        // Parse Handshake header.
        const std::uint8_t  msg_type = payload[0];
        const std::uint32_t msg_len = (static_cast<std::uint32_t>(payload[1]) << 16) |
                                       (static_cast<std::uint32_t>(payload[2]) << 8)  |
                                        static_cast<std::uint32_t>(payload[3]);
        const std::uint16_t msg_seq = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(payload[4]) << 8) | payload[5]);
        const std::uint32_t frag_off = (static_cast<std::uint32_t>(payload[6]) << 16) |
                                        (static_cast<std::uint32_t>(payload[7]) << 8)  |
                                         static_cast<std::uint32_t>(payload[8]);
        const std::uint32_t frag_len = (static_cast<std::uint32_t>(payload[9]) << 16) |
                                        (static_cast<std::uint32_t>(payload[10]) << 8)  |
                                         static_cast<std::uint32_t>(payload[11]);

        // Unfragmented: pass through unchanged.
        if (frag_off == 0 && frag_len == msg_len &&
            payload.size() == 12u + msg_len) {
            return Result::kPassThrough;
        }

        // Defensive sanity checks.
        if (msg_len == 0 || msg_len > 16384)   return Result::kPassThrough;
        if (frag_off >= msg_len)               return Result::kPassThrough;
        if (frag_len >  msg_len)               return Result::kPassThrough;
        if (frag_off + frag_len > msg_len)     return Result::kPassThrough;
        if (payload.size() < 12u + frag_len)   return Result::kPassThrough;

        // Find or create the per-msg_seq buffer.
        auto it = bufs_.find(msg_seq);
        if (it == bufs_.end()) {
            if (payload.size() != 12u + frag_len) {
                return Result::kPassThrough;   // mismatched first fragment
            }
            FragmentBuf buf;
            buf.msg_type     = msg_type;
            buf.msg_seq      = msg_seq;
            buf.total_length = msg_len;
            buf.body.assign(msg_len, 0);
            buf.received.assign(msg_len, false);
            buf.epoch        = epoch;
            buf.first_seq    = seq;
            buf.last_seen    = clock::now();
            auto [ins, _] = bufs_.emplace(msg_seq, std::move(buf));
            it = ins;
        } else {
            auto& buf = it->second;
            if (buf.msg_type     != msg_type) return Result::kPassThrough;
            if (buf.total_length != msg_len)  return Result::kPassThrough;
            if (buf.epoch        != epoch)    return Result::kPassThrough;
            buf.last_seen = clock::now();
        }
        auto& buf = it->second;

        // Copy fragment data into the body buffer.
        std::memcpy(buf.body.data() + frag_off,
                    payload.data() + 12,
                    frag_len);
        for (std::uint32_t i = 0; i < frag_len; ++i) {
            buf.received[frag_off + i] = true;
        }

        // Check completeness.
        bool complete = true;
        for (bool b : buf.received) {
            if (!b) { complete = false; break; }
        }
        if (!complete) return Result::kFragmentBuffered;

        // Build the synthesised unfragmented DTLS record.
        const std::uint32_t hs_total = buf.total_length;
        out_reassembled.resize(13 + 12 + hs_total);
        auto* p = out_reassembled.data();
        p[0] = 22;                                // Handshake
        p[1] = record[1]; p[2] = record[2];       // version
        p[3] = static_cast<std::uint8_t>((buf.epoch >> 8) & 0xff);
        p[4] = static_cast<std::uint8_t>(buf.epoch & 0xff);
        // Synthesised sequence: 1 above first fragment's seq.
        // Local-only space — never leaves the reassembler.
        const std::uint64_t new_seq = buf.first_seq + 1;
        for (int i = 0; i < 6; ++i) {
            p[5 + i] = static_cast<std::uint8_t>((new_seq >> (8 * (5 - i))) & 0xff);
        }
        const std::uint16_t new_rec_len = static_cast<std::uint16_t>(12 + hs_total);
        p[11] = static_cast<std::uint8_t>((new_rec_len >> 8) & 0xff);
        p[12] = static_cast<std::uint8_t>(new_rec_len & 0xff);
        p[13] = buf.msg_type;
        p[14] = static_cast<std::uint8_t>((hs_total >> 16) & 0xff);
        p[15] = static_cast<std::uint8_t>((hs_total >> 8)  & 0xff);
        p[16] = static_cast<std::uint8_t>(hs_total & 0xff);
        p[17] = static_cast<std::uint8_t>((buf.msg_seq >> 8) & 0xff);
        p[18] = static_cast<std::uint8_t>(buf.msg_seq & 0xff);
        p[19] = p[20] = p[21] = 0;                 // frag_off = 0
        p[22] = static_cast<std::uint8_t>((hs_total >> 16) & 0xff);
        p[23] = static_cast<std::uint8_t>((hs_total >> 8)  & 0xff);
        p[24] = static_cast<std::uint8_t>(hs_total & 0xff);
        std::memcpy(p + 25, buf.body.data(), hs_total);

        bufs_.erase(it);
        return Result::kReassembled;
    }

private:
    struct FragmentBuf {
        std::uint8_t          msg_type     = 0;
        std::uint16_t         msg_seq      = 0;
        std::uint32_t         total_length = 0;
        std::vector<std::uint8_t> body;
        std::vector<bool>     received;
        std::uint16_t         epoch        = 0;
        std::uint64_t         first_seq    = 0;
        clock::time_point     last_seen{};
    };
    std::map<std::uint16_t, FragmentBuf> bufs_;
    std::chrono::milliseconds gc_ttl_{5000};
};

} // namespace nimrtc::dtls
