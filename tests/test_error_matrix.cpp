// ============================================================================
// RTP / RTCP error-path matrix test.
//
// Each case is a hand-crafted byte buffer that exercises one failure mode.
//   The Parser is required to reject malformed input with ProtocolError
//   instead of silently returning garbage.
//
// RTP group (Parser::parse of malformed RTP packets):
//   e01_too_small      - 11-byte packet (< 12 min)
//   e02_version        - V=1 packet
//   e03_csrc_overflow  - CC=15 but no room for 60 bytes of CSRC
//   e04_truncated_csrc - CC=2 but only space for CC=1
//   e05_truncated_ext  - X bit set, no ext header
//   e06_pad_oversize   - last byte = 50 padding bytes; only 17 bytes left
//
// RTCP group (parse_sr / parse_rr / parse_nack):
//   e07_sr_too_small   - 3-byte RTCP
//   e08_sr_version     - V=1 RTCP
//   e09_sr_wrong_pt    - RR packet fed to parse_sr (PT mismatch)
//   e10_sr_len_overshoot - length_words = 50 in a 28-byte packet
//   e11_sr_len_undershoot - length_words = 1 in SR (min is 6)
//   e12_rr_short_rb    - RC=2 but only 1 RB worth of data
//   e13_nack_wrong_fmt - NACK with FMT=2 (must be 1)
//   e14_nack_misaligned_fci - 17-byte NACK with 5 bytes of FCI
//   e15_nack_truncated - header + sender SSRC but no media SSRC
//
// "Recovery" cases (parser must not crash on adversarial input):
//   e16_zero_len       - 0-byte input
//   e17_length_max     - length_words = 0xFFFF in a tiny packet
// ============================================================================

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

inline void set_u8(std::vector<std::uint8_t>& v, std::size_t i,
                   std::uint8_t b) {
    if (i < v.size()) v[i] = b;
}
inline void set_u32_be(std::vector<std::uint8_t>& v, std::size_t i,
                       std::uint32_t x) {
    if (i + 4 > v.size()) return;
    v[i]     = std::uint8_t((x >> 24) & 0xFF);
    v[i + 1] = std::uint8_t((x >> 16) & 0xFF);
    v[i + 2] = std::uint8_t((x >> 8)  & 0xFF);
    v[i + 3] = std::uint8_t(x & 0xFF);
}
inline void set_u16_be(std::vector<std::uint8_t>& v, std::size_t i,
                       std::uint16_t x) {
    if (i + 2 > v.size()) return;
    v[i]     = std::uint8_t((x >> 8) & 0xFF);
    v[i + 1] = std::uint8_t(x & 0xFF);
}

struct Case {
    const char* name;
    std::vector<std::uint8_t> bytes;
    bool should_succeed;
    enum class Target { Rtp, Sr, Rr, Nack } target;
};

bool run_case(const Case& c, std::string& err_out) {
    nimrtc::rtp::Parser p;
    bool actual_ok = false;
    std::optional<nimrtc::core::Result<nimrtc::rtp::SenderReport>>   sr;
    std::optional<nimrtc::core::Result<nimrtc::rtp::ReceiverReport>> rr;
    std::optional<nimrtc::core::Result<nimrtc::rtp::NackPacket>>     nk;
    std::optional<nimrtc::core::Result<nimrtc::rtp::PacketView>>     pv;
    nimrtc::core::ByteSpan raw{c.bytes.data(), c.bytes.size()};
    switch (c.target) {
        case Case::Target::Rtp:  pv = p.parse(raw);      actual_ok = pv->ok(); break;
        case Case::Target::Sr:   sr = p.parse_sr(raw);   actual_ok = sr->ok(); break;
        case Case::Target::Rr:   rr = p.parse_rr(raw);   actual_ok = rr->ok(); break;
        case Case::Target::Nack: nk = p.parse_nack(raw); actual_ok = nk->ok(); break;
    }
    if (actual_ok != c.should_succeed) {
        err_out = std::string("expected ") +
                  (c.should_succeed ? "ok()" : "error") + ", got " +
                  (actual_ok ? "ok()" : "error");
        if (!actual_ok) {
            switch (c.target) {
                case Case::Target::Rtp:  err_out += " (" + (*pv).error().message() + ")"; break;
                case Case::Target::Sr:   err_out += " (" + (*sr).error().message() + ")"; break;
                case Case::Target::Rr:   err_out += " (" + (*rr).error().message() + ")"; break;
                case Case::Target::Nack: err_out += " (" + (*nk).error().message() + ")"; break;
            }
        }
        return false;
    }
    return true;
}

}  // namespace

int main() {
    // Baseline RTP packet: 18 bytes
    std::vector<std::uint8_t> rtp_baseline(18, 0);
    rtp_baseline[0] = 0x80;  rtp_baseline[1] = 0x60;
    set_u16_be(rtp_baseline, 2, 0x1234);
    set_u32_be(rtp_baseline, 4, 0x00000064);
    set_u32_be(rtp_baseline, 8, 0xCAFEBABE);
    set_u32_be(rtp_baseline, 12, 0x11111111);
    rtp_baseline[16] = 'p'; rtp_baseline[17] = 'p';

    // Baseline SR packet: 28 bytes
    std::vector<std::uint8_t> sr_baseline(28, 0);
    sr_baseline[0] = 0x80; sr_baseline[1] = 200;
    set_u16_be(sr_baseline, 2, 6);
    set_u32_be(sr_baseline, 4, 0xAABBCCDD);
    set_u32_be(sr_baseline, 8, 0x12345678);
    set_u32_be(sr_baseline, 12, 0x9ABCDEF0);
    set_u32_be(sr_baseline, 16, 0x00000100);
    set_u32_be(sr_baseline, 20, 0x00001000);
    set_u32_be(sr_baseline, 24, 0x000F4240);

    // Baseline RR packet: 32 bytes
    std::vector<std::uint8_t> rr_baseline(32, 0);
    rr_baseline[0] = 0x81; rr_baseline[1] = 201;
    set_u16_be(rr_baseline, 2, 7);
    set_u32_be(rr_baseline, 4, 0xF1F1F1F1);
    set_u32_be(rr_baseline, 8, 0xFEEDFACE);
    set_u32_be(rr_baseline, 24, 0xDEADBEEF);

    // Baseline NACK packet: 16 bytes
    std::vector<std::uint8_t> nack_baseline(16, 0);
    nack_baseline[0] = 0x81; nack_baseline[1] = 205;
    set_u16_be(nack_baseline, 2, 3);
    set_u32_be(nack_baseline, 4, 0x44444444);
    set_u32_be(nack_baseline, 8, 0x55555555);
    set_u16_be(nack_baseline, 12, 17);
    set_u16_be(nack_baseline, 14, 0xAAAA);

    std::vector<Case> cases;
    // RTP error cases
    { Case c; c.name = "e01_rtp_too_small";
      c.bytes = std::vector<std::uint8_t>(11, 0); c.bytes[0] = 0x80;
      c.should_succeed = false; c.target = Case::Target::Rtp; cases.push_back(std::move(c)); }
    { Case c; c.name = "e02_rtp_version";
      c.bytes = rtp_baseline; set_u8(c.bytes, 0, 0x40);
      c.should_succeed = false; c.target = Case::Target::Rtp; cases.push_back(std::move(c)); }
    { Case c; c.name = "e03_rtp_csrc_overshoot";
      c.bytes = std::vector<std::uint8_t>(16, 0);
      c.bytes[0] = 0x8F; c.bytes[1] = 0x60;
      c.should_succeed = false; c.target = Case::Target::Rtp; cases.push_back(std::move(c)); }
    { Case c; c.name = "e04_rtp_truncated_csrc";
      c.bytes = rtp_baseline; set_u8(c.bytes, 0, 0x82);
      c.should_succeed = false; c.target = Case::Target::Rtp; cases.push_back(std::move(c)); }
    { Case c; c.name = "e05_rtp_no_ext_header";
      c.bytes = rtp_baseline; set_u8(c.bytes, 0, 0x90);
      c.should_succeed = false; c.target = Case::Target::Rtp; cases.push_back(std::move(c)); }
    { Case c; c.name = "e06_rtp_pad_oversize";
      c.bytes = rtp_baseline;
      set_u8(c.bytes, 0, c.bytes[0] | 0x20);
      c.bytes.back() = 50;
      c.should_succeed = false; c.target = Case::Target::Rtp; cases.push_back(std::move(c)); }

    // RTCP error cases
    { Case c; c.name = "e07_rtcp_too_small";
      c.bytes = std::vector<std::uint8_t>(3, 0); c.bytes[0] = 0x80;
      c.should_succeed = false; c.target = Case::Target::Sr; cases.push_back(std::move(c)); }
    { Case c; c.name = "e08_rtcp_version";
      c.bytes = sr_baseline; set_u8(c.bytes, 0, 0x40);
      c.should_succeed = false; c.target = Case::Target::Sr; cases.push_back(std::move(c)); }
    { Case c; c.name = "e09_sr_wrong_pt";
      c.bytes = rr_baseline;
      c.should_succeed = false; c.target = Case::Target::Sr; cases.push_back(std::move(c)); }
    { Case c; c.name = "e10_sr_length_overshoot";
      c.bytes = sr_baseline; set_u16_be(c.bytes, 2, 50);
      c.should_succeed = false; c.target = Case::Target::Sr; cases.push_back(std::move(c)); }
    { Case c; c.name = "e11_sr_length_undershoot";
      c.bytes = sr_baseline; set_u16_be(c.bytes, 2, 1);
      c.should_succeed = false; c.target = Case::Target::Sr; cases.push_back(std::move(c)); }
    { Case c; c.name = "e12_rr_short_rb";
      c.bytes = rr_baseline; set_u8(c.bytes, 0, 0x82);
      c.should_succeed = false; c.target = Case::Target::Rr; cases.push_back(std::move(c)); }
    { Case c; c.name = "e13_nack_wrong_fmt";
      c.bytes = nack_baseline; set_u8(c.bytes, 0, 0x82);
      c.should_succeed = false; c.target = Case::Target::Nack; cases.push_back(std::move(c)); }
    { Case c; c.name = "e14_nack_misaligned_fci";
      c.bytes = std::vector<std::uint8_t>(17, 0);
      c.bytes[0] = 0x81; c.bytes[1] = 205;
      set_u16_be(c.bytes, 2, 3);
      c.bytes[16] = 0xAA;
      c.should_succeed = false; c.target = Case::Target::Nack; cases.push_back(std::move(c)); }
    { Case c; c.name = "e15_nack_truncated";
      c.bytes = std::vector<std::uint8_t>(8, 0);
      c.bytes[0] = 0x81; c.bytes[1] = 205;
      set_u16_be(c.bytes, 2, 1);
      c.should_succeed = false; c.target = Case::Target::Nack; cases.push_back(std::move(c)); }

    // Recovery cases
    { Case c; c.name = "e16_zero_len";
      c.bytes = {};
      c.should_succeed = false; c.target = Case::Target::Rtp; cases.push_back(std::move(c)); }
    { Case c; c.name = "e17_length_max";
      c.bytes = sr_baseline; set_u16_be(c.bytes, 2, 0xFFFF);
      c.should_succeed = false; c.target = Case::Target::Sr; cases.push_back(std::move(c)); }

    int passed = 0, failed = 0;
    for (const auto& c : cases) {
        std::string err;
        if (run_case(c, err)) {
            std::printf("[PASS] %s (%zu bytes)\n", c.name, c.bytes.size());
            ++passed;
        } else {
            std::printf("[FAIL] %s -- %s\n", c.name, err.c_str());
            ++failed;
        }
    }
    std::printf("\n[SUMMARY] passed=%d failed=%d total=%d\n",
                passed, failed, passed + failed);
    return failed == 0 ? 0 : 1;
}
