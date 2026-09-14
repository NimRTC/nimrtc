/**
 * @file src/modules/video_payload/src/vp8.cpp
 * @brief VP8 RTP payload packetizer / depacketizer (RFC 7741).
 */

#include <nimrtc/video_payload/vp8.hpp>

#include <cstring>

namespace nimrtc::video_payload::vp8 {

namespace {

constexpr std::uint8_t kDescByteBitX = 0x80;
constexpr std::uint8_t kDescByteBitN = 0x20;
constexpr std::uint8_t kDescByteBitS = 0x10;
constexpr std::uint8_t kPidMask      = 0x07;

constexpr std::uint8_t kExtBitI = 0x80;
constexpr std::uint8_t kExtBitL = 0x40;
constexpr std::uint8_t kExtBitT = 0x20;
constexpr std::uint8_t kExtBitK = 0x10;

} // namespace

std::size_t descriptor_size(const Descriptor& d) noexcept {
    if (!d.has_picture_id) return 1;        // base descriptor only
    return 2 + (d.picture_id_16bit ? 2 : 1); // ext byte + pic id
}

ParseResult parse(core::ByteSpan payload) noexcept {
    ParseResult r;
    if (payload.empty()) return r;

    const std::uint8_t b0 = payload[0];
    r.desc.partition_index    = b0 & kPidMask;
    r.desc.start_of_partition = (b0 & kDescByteBitS) != 0;
    r.desc.non_reference      = (b0 & kDescByteBitN) != 0;

    std::size_t pos = 1;
    if (b0 & kDescByteBitX) {
        if (pos >= payload.size()) return r;
        const std::uint8_t ext = payload[pos++];
        if (ext & kExtBitI) {
            r.desc.has_picture_id = true;
            if (pos >= payload.size()) return r;
            std::uint8_t pic = payload[pos++];
            if (pic & 0x80) {
                // M=1 → 16-bit picture ID follows.
                if (pos + 1 >= payload.size()) return r;
                const std::uint16_t pic16 =
                    (static_cast<std::uint16_t>(pic & 0x7F) << 8)
                  |  static_cast<std::uint16_t>(payload[pos]);
                r.desc.picture_id_16bit = true;
                r.desc.picture_id       = static_cast<std::int32_t>(pic16);
                pos += 1;
            } else {
                r.desc.picture_id = static_cast<std::int32_t>(pic & 0x7F);
            }
        }
        // We deliberately do not parse L (TL0PICIDX), T (TID), K (KEYIDX)
        // — Chrome interop doesn't require them and they're rarely used.
    }

    r.bitstream = core::ByteSpan{payload.data() + pos, payload.size() - pos};
    r.parsed_ok = true;
    return r;
}

std::size_t build_descriptor(const Descriptor& d,
                             core::MutableByteSpan output) noexcept {
    const std::size_t need = descriptor_size(d);
    if (output.size() < need) return 0;

    std::uint8_t b0 = static_cast<std::uint8_t>(d.partition_index & kPidMask);
    if (d.start_of_partition) b0 |= kDescByteBitS;
    if (d.non_reference)      b0 |= kDescByteBitN;
    if (d.has_picture_id)     b0 |= kDescByteBitX;
    output[0] = b0;

    if (!d.has_picture_id) return 1;

    output[1] = kExtBitI;          // I=1, L=T=K=0
    if (d.picture_id_16bit) {
        const std::uint16_t pid16 = static_cast<std::uint16_t>(d.picture_id) & 0x7FFFu;
        output[2] = static_cast<std::uint8_t>(0x80 | (pid16 >> 8));
        output[3] = static_cast<std::uint8_t>( pid16       & 0xFF);
        return 4;
    } else {
        output[2] = static_cast<std::uint8_t>(d.picture_id & 0x7F);
        return 3;
    }
}

std::size_t build(const Descriptor& d,
                  core::ByteSpan bitstream,
                  core::MutableByteSpan output) noexcept {
    const std::size_t desc_bytes = build_descriptor(d, output);
    if (desc_bytes == 0) return 0;
    if (output.size() < desc_bytes + bitstream.size()) return 0;
    if (!bitstream.empty()) {
        std::memcpy(output.data() + desc_bytes, bitstream.data(), bitstream.size());
    }
    return desc_bytes + bitstream.size();
}

} // namespace nimrtc::video_payload::vp8
