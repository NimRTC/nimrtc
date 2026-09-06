/**
 * @file src/modules/video_payload/src/vp9.cpp
 * @brief VP9 RTP payload packetizer / depacketizer (RFC 9559).
 */

#include <nimrtc/video_payload/vp9.hpp>

#include <cstring>

namespace nimrtc::video_payload::vp9 {

namespace {

constexpr std::uint8_t kBitZ = 0x80;
constexpr std::uint8_t kBitY = 0x40;
constexpr std::uint8_t kBitF = 0x20;
constexpr std::uint8_t kIdMask = 0x0C;
constexpr std::uint8_t kBitN  = 0x02;
constexpr std::uint8_t kBitD  = 0x01;

} // namespace

std::size_t descriptor_size(const Descriptor& d) noexcept {
    std::size_t sz = 1;
    if (d.has_picture_id) sz += d.picture_id_16bit ? 2 : 1;
    if (d.has_layer_info) sz += 1;
    return sz;
}

ParseResult parse(core::ByteSpan payload) noexcept {
    ParseResult r;
    if (payload.empty()) return r;

    const std::uint8_t b0 = payload[0];
    r.desc.has_picture_id   = (b0 & kBitZ) != 0;
    r.desc.has_layer_info   = (b0 & kBitY) != 0;
    r.desc.flexible_mode    = (b0 & kBitF) != 0;
    r.desc.non_reference    = (b0 & kBitN) != 0;
    r.desc.inter_predicted  = (b0 & kBitD) != 0;
    (void)(b0 & kIdMask);    // 2-bit ID is reserved in modern VP9 RTP; ignore

    std::size_t pos = 1;
    if (r.desc.has_picture_id) {
        if (pos >= payload.size()) return r;
        std::uint8_t pic = payload[pos++];
        if (pic & 0x80) {
            // M=1 → 16-bit picture ID follows.
            if (pos >= payload.size()) return r;
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
    if (r.desc.has_layer_info) {
        if (pos >= payload.size()) return r;
        const std::uint8_t layer = payload[pos++];
        r.desc.temporal_id = (layer >> 5) & 0x07;
        r.desc.spatial_id  = (layer >> 3) & 0x03;
        // D bit, U bit — ignored for non-SVC streams.
    }

    r.bitstream = core::ByteSpan{payload.data() + pos, payload.size() - pos};
    r.parsed_ok = true;
    return r;
}

std::size_t build_descriptor(const Descriptor& d,
                             core::MutableByteSpan output) noexcept {
    const std::size_t need = descriptor_size(d);
    if (output.size() < need) return 0;

    std::uint8_t b0 = 0;
    if (d.has_picture_id)  b0 |= kBitZ;
    if (d.has_layer_info)  b0 |= kBitY;
    if (d.flexible_mode)   b0 |= kBitF;
    if (d.non_reference)   b0 |= kBitN;
    if (d.inter_predicted) b0 |= kBitD;
    output[0] = b0;

    std::size_t pos = 1;
    if (d.has_picture_id) {
        if (d.picture_id_16bit) {
            const std::uint16_t pid16 =
                static_cast<std::uint16_t>(d.picture_id) & 0x7FFFu;
            output[pos + 0] = static_cast<std::uint8_t>(0x80 | (pid16 >> 8));
            output[pos + 1] = static_cast<std::uint8_t>( pid16       & 0xFF);
            pos += 2;
        } else {
            output[pos] = static_cast<std::uint8_t>(d.picture_id & 0x7F);
            pos += 1;
        }
    }
    if (d.has_layer_info) {
        output[pos] = static_cast<std::uint8_t>(
            ((d.temporal_id & 0x07) << 5)
          | ((d.spatial_id  & 0x03) << 3));
        pos += 1;
    }
    return pos;
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

} // namespace nimrtc::video_payload::vp9
