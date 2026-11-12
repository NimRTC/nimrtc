/**
 * @file src/modules/video_payload/src/av1.cpp
 * @brief AV1 RTP payload packetizer / depacketizer
 *        (draft-ietf-payload-av1 / aomediacodec av1-rtp-spec v1.0.0).
 */

#include <nimrtc/video_payload/av1.hpp>

#include <algorithm>
#include <cstring>

namespace nimrtc::video_payload::av1 {

namespace {

// The OBU header bit layout (AV1 spec §5.3.1):
//
//    0 1 2 3 4 5 6 7
//   +-+-+-+-+-+-+-+-+
//   |1| type  |X|S|-|
//   +-+-+-+-+-+-+-+-+
//
//   bit 7     : obu_forbidden_bit (always 0)
//   bits 6..3 : obu_type
//   bit 2     : obu_extension_flag
//   bit 1     : obu_has_size_field
//   bit 0     : obu_reserved_1bit
constexpr std::uint8_t kObuTypeShift   = 3;
constexpr std::uint8_t kObuTypeMask    = 0x0F;
constexpr std::uint8_t kObuExtFlag     = 0x04;
constexpr std::uint8_t kObuHasSizeFlag = 0x02;

// Aggregation header bits (av1-rtp-spec §4.4).
constexpr std::uint8_t kAggZMask  = 0x80;
constexpr std::uint8_t kAggYMask  = 0x40;
constexpr std::uint8_t kAggWMask  = 0x30;
constexpr std::uint8_t kAggWShift = 4;
constexpr std::uint8_t kAggNMask  = 0x08;

/** The maximum number of OBU elements whose count can be stated in the
 *  W field.  Beyond that the count must be 0 ("each element carries a
 *  length field"). */
constexpr std::uint8_t kMaxExplicitElementCount = 3;

/** The Annex-B start code NimRTC writes in front of every OBU. */
constexpr std::uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

} // namespace

// ---------------------------------------------------------------------------
// The OBU header
// ---------------------------------------------------------------------------

std::optional<ObuHeader> parse_obu_header(core::ByteSpan payload) noexcept {
    if (payload.empty()) return std::nullopt;

    ObuHeader h;
    const std::uint8_t b0 = payload[0];
    h.type          = static_cast<ObuType>((b0 >> kObuTypeShift) & kObuTypeMask);
    h.has_extension = (b0 & kObuExtFlag) != 0;
    h.has_size      = (b0 & kObuHasSizeFlag) != 0;

    std::size_t pos = 1;
    if (h.has_extension) {
        if (payload.size() < 2) return std::nullopt;
        h.temporal_id = static_cast<std::uint8_t>((payload[1] >> 5) & 0x07);
        h.spatial_id  = static_cast<std::uint8_t>((payload[1] >> 3) & 0x03);
        pos = 2;
    }
    if (h.has_size) {
        // The `obu_size` field is LEB128-encoded, so its length depends on
        // the value.  Walk it.
        std::size_t p = pos;
        while (p < payload.size() && (payload[p] & 0x80) != 0) ++p;
        if (p >= payload.size()) return std::nullopt;   // truncated LEB128
        ++p;
        pos = p;
    }
    h.header_size = pos;
    return h;
}

ObuType peek_obu_type(core::ByteSpan payload) noexcept {
    if (payload.empty()) return ObuType::kReserved1;
    return static_cast<ObuType>((payload[0] >> kObuTypeShift) & kObuTypeMask);
}

std::size_t leb128_size(std::size_t size) noexcept {
    std::size_t n = 1;
    while (size >= 0x80) {
        size >>= 7;
        ++n;
    }
    return n;
}

std::size_t write_leb128(std::size_t size, core::MutableByteSpan output) noexcept {
    std::size_t pos = 0;
    do {
        if (pos >= output.size()) return 0;
        std::uint8_t byte = static_cast<std::uint8_t>(size & 0x7F);
        size >>= 7;
        if (size != 0) byte |= 0x80;
        output[pos++] = byte;
    } while (size != 0);
    return pos;
}

bool read_leb128(core::ByteSpan payload,
                 std::size_t& pos,
                 std::size_t& value) noexcept {
    value = 0;
    std::size_t shift = 0;
    for (int i = 0; i < 8; ++i) {
        if (pos >= payload.size()) return false;
        const std::uint8_t byte = payload[pos++];
        value |= static_cast<std::size_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;   // more than 8 bytes — not a legal LEB128 value
}

// ---------------------------------------------------------------------------
// The AV1 aggregation header
// ---------------------------------------------------------------------------

std::optional<AggregationHeader> parse_aggregation_header(
    core::ByteSpan payload) noexcept {
    if (payload.empty()) return std::nullopt;

    AggregationHeader h;
    const std::uint8_t b = payload[0];
    h.starts_with_fragment = (b & kAggZMask) != 0;
    h.ends_with_fragment   = (b & kAggYMask) != 0;
    h.element_count        = static_cast<std::uint8_t>((b & kAggWMask) >> kAggWShift);
    h.new_sequence         = (b & kAggNMask) != 0;
    return h;
}

std::size_t write_aggregation_header(const AggregationHeader& h,
                                     core::MutableByteSpan output) noexcept {
    if (output.empty()) return 0;

    std::uint8_t b = 0;
    if (h.starts_with_fragment) b |= kAggZMask;
    if (h.ends_with_fragment)   b |= kAggYMask;
    if (h.element_count > 0 && h.element_count <= kMaxExplicitElementCount) {
        b |= static_cast<std::uint8_t>((h.element_count & 0x03) << kAggWShift);
    }
    if (h.new_sequence) b |= kAggNMask;
    output[0] = b;
    return 1;
}

// ---------------------------------------------------------------------------
// OBU elements
// ---------------------------------------------------------------------------

bool parse_elements(core::ByteSpan payload,
                    std::vector<ObuElement>& elements) noexcept {
    elements.clear();
    if (payload.empty()) return false;

    auto agg = parse_aggregation_header(payload);
    if (!agg) return false;

    std::size_t pos = 1;

    // W = 0 means "every element carries a length field"; W = 1..3 means
    // "the last element does not" — its length is derived from the
    // remaining payload bytes.
    const bool every_element_has_length = (agg->element_count == 0);
    const std::uint8_t declared = agg->element_count;

    // The number of elements is known exactly when W != 0.  With W == 0
    // the elements are discovered by walking the length fields, so the
    // count is unknown up front.
    std::uint8_t seen = 0;

    while (pos < payload.size()) {
        const bool is_last_declared =
            every_element_has_length ? false : (seen + 1 >= declared);
        const bool has_length = every_element_has_length || !is_last_declared;

        std::size_t len = 0;
        if (has_length) {
            if (!read_leb128(payload, pos, len)) return false;
        } else {
            len = payload.size() - pos;
        }
        if (len == 0 || pos + len > payload.size()) return false;

        ObuElement e;
        e.data = core::ByteSpan{payload.data() + pos, len};

        // Which elements carry an OBU header?
        //
        //   - element 0 carries one iff Z == 0 (it starts an OBU);
        //   - the last element carries one iff Y == 0 (the OBU ends
        //     here);
        //   - any element in between always carries one (it starts an
        //     OBU that ends in a later packet).
        const bool is_first = (seen == 0);
        const bool is_last  = every_element_has_length
                                ? (pos + len >= payload.size())
                                : is_last_declared;
        if (is_first) {
            e.carries_header = !agg->starts_with_fragment;
        } else if (is_last) {
            e.carries_header = !agg->ends_with_fragment;
        } else {
            e.carries_header = true;
        }
        e.is_last_fragment = !agg->ends_with_fragment;

        if (e.carries_header) {
            auto hdr = parse_obu_header(e.data);
            e.type = hdr ? hdr->type : ObuType::kReserved1;
        }

        elements.push_back(e);
        pos += len;
        ++seen;

        if (!every_element_has_length && seen >= declared) break;
    }
    return !elements.empty();
}

// ---------------------------------------------------------------------------
// Annex-B OBU framing
// ---------------------------------------------------------------------------

bool parse_annex_b_obus(core::ByteSpan bs, std::vector<Obu>& obus) noexcept {
    obus.clear();
    if (bs.size() < 2) return false;

    // Locate the start codes (three- or four-byte).
    auto find_start = [&](std::size_t from, std::size_t& prefix_len) -> std::size_t {
        for (std::size_t j = from; j + 3 < bs.size(); ++j) {
            if (bs[j] == 0 && bs[j + 1] == 0 && bs[j + 2] == 0 && bs[j + 3] == 1) {
                prefix_len = 4;
                return j;
            }
            if (j + 2 < bs.size() && bs[j] == 0 && bs[j + 1] == 0 && bs[j + 2] == 1) {
                prefix_len = 3;
                return j;
            }
        }
        return bs.size();
    };

    std::size_t prefix = 0;
    std::size_t start = find_start(0, prefix);
    if (start == bs.size()) return false;

    std::size_t cursor = start + prefix;
    while (cursor < bs.size()) {
        std::size_t next_prefix = 0;
        const std::size_t next = find_start(cursor, next_prefix);
        const std::size_t end = (next == bs.size()) ? bs.size() : next;
        if (end > cursor) {
            Obu o;
            o.data = core::ByteSpan{bs.data() + cursor, end - cursor};
            o.type = peek_obu_type(o.data);
            obus.push_back(o);
        }
        if (next == bs.size()) break;
        cursor = next + next_prefix;
    }
    return !obus.empty();
}

bool contains_sequence_header(core::ByteSpan bs) noexcept {
    std::vector<Obu> obus;
    if (!parse_annex_b_obus(bs, obus)) return false;
    for (const auto& o : obus) {
        if (o.type == ObuType::kSequenceHeader) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Packetization
// ---------------------------------------------------------------------------

bool packetize(const std::vector<Obu>& obus,
               std::size_t mtu,
               bool new_sequence,
               std::vector<Packet>& packets) noexcept {
    packets.clear();
    if (mtu < 16) return false;

    // ---- Build the list of "elements" to carry ----
    //
    // Each element is either a whole OBU or a fragment of one.  The
    // OBU header always travels with the *first* fragment.
    struct Element {
        ObuType              type = ObuType::kReserved1;
        core::ByteSpan      data{};         // the OBU bytes (header + payload)
        std::size_t         header_size = 0;  // bytes of the OBU header
    };

    std::vector<Element> elements;
    for (const auto& o : obus) {
        if (o.data.empty()) continue;
        auto hdr = parse_obu_header(o.data);
        Element e;
        e.type        = hdr ? hdr->type : peek_obu_type(o.data);
        e.data        = o.data;
        e.header_size = hdr ? hdr->header_size : 1;
        elements.push_back(e);
    }
    if (elements.empty()) return false;

    // ---- Pack the elements into packets ----
    //
    // The strategy NimRTC uses:
    //
    //   * Small OBUs are *aggregated*: as many as fit into one packet,
    //     each preceded by its LEB128 length field.  A packet with more
    //     than one element uses the W = 0 form (every element carries a
    //     length field) because the W field can only express a count of
    //     1..3 and the aggregation-header accounting has to stay
    //     unambiguous for the depacketizer.
    //   * A packet with exactly one element uses the W = 1 form (no
    //     length field at all) — the cheapest encoding, and the one
    //     every browser's depacketizer accepts.
    //   * An OBU too large for one packet is *fragmented*: the first
    //     fragment carries the OBU header (Z = 0, Y = 1), the following
    //     ones carry payload only (Z = 1).
    //
    // The last element of a fragmented OBU is *not* special-cased: the
    // fragment that completes it sets Y = 0, so the depacketizer knows
    // the OBU ends there.

    std::size_t idx = 0;
    bool first_packet = true;

    while (idx < elements.size()) {
        const Element& e = elements[idx];

        // ---- Does the whole element fit? ----
        //
        // The single-element form (W = 1) has no length field, so the
        // element needs `1 + e.data.size()` bytes.
        const std::size_t overhead = 1;                       // aggregation header
        const std::size_t len_needed = overhead + e.data.size();

        if (len_needed <= mtu) {
            // ---- Try to aggregate with the following OBUs ----
            //
            // Every aggregated element costs its own LEB128 length field
            // plus its bytes.  Only the *first* element may drop its
            // length field — the W = 1 form — and that form allows only
            // one element per packet.  So an aggregated packet uses the
            // W = 0 form, where *every* element carries a length field.
            //
            // The trade-off is one byte (the length field of the first
            // element) against the per-packet overhead of the RTP /
            // SRTP headers — worth it whenever two OBUs fit.
            std::vector<const Element*> group;
            std::size_t cost = overhead;

            // The first element always fits (checked above).  Does a
            // *second* one fit alongside it?
            if (idx + 1 < elements.size()) {
                const Element& next = elements[idx + 1];
                const std::size_t first_cost =
                    leb128_size(e.data.size()) + e.data.size();
                const std::size_t next_cost =
                    leb128_size(next.data.size()) + next.data.size();
                if (cost + first_cost + next_cost <= mtu) {
                    group.push_back(&e);
                    cost += first_cost;
                    group.push_back(&next);
                    cost += next_cost;

                    // Try to add a third element.
                    if (idx + 2 < elements.size()) {
                        const Element& third = elements[idx + 2];
                        const std::size_t third_cost =
                            leb128_size(third.data.size()) + third.data.size();
                        if (cost + third_cost <= mtu) {
                            group.push_back(&third);
                            cost += third_cost;
                        }
                    }
                }
            }

            if (group.empty()) {
                // ---- Single element, W = 1 (no length field) ----
                Packet p;
                p.buffer.resize(len_needed);

                AggregationHeader agg;
                agg.starts_with_fragment = false;
                agg.ends_with_fragment   = false;
                agg.element_count        = 1;
                agg.new_sequence         = first_packet && new_sequence;
                write_aggregation_header(agg, core::MutableByteSpan{p.buffer.data(),
                                                                     p.buffer.size()});
                std::memcpy(p.buffer.data() + 1, e.data.data(), e.data.size());

                p.payload = core::ByteSpan{p.buffer.data(), p.buffer.size()};
                packets.push_back(std::move(p));

                ++idx;
                first_packet = false;
                continue;
            }

            // ---- Aggregated elements, W = 0 (every element has a
            //      LEB128 length field) ----
            Packet p;
            p.buffer.resize(cost);

            AggregationHeader agg;
            agg.starts_with_fragment = false;
            agg.ends_with_fragment   = false;
            agg.element_count        = 0;   // "each element has a length field"
            agg.new_sequence         = first_packet && new_sequence;
            write_aggregation_header(agg, core::MutableByteSpan{p.buffer.data(),
                                                                 p.buffer.size()});

            std::size_t wpos = overhead;
            for (const Element* ge : group) {
                const std::size_t n = write_leb128(
                    ge->data.size(),
                    core::MutableByteSpan{p.buffer.data() + wpos,
                                          p.buffer.size() - wpos});
                if (n == 0) return false;
                wpos += n;
                std::memcpy(p.buffer.data() + wpos, ge->data.data(), ge->data.size());
                wpos += ge->data.size();
            }
            p.buffer.resize(wpos);

            p.payload = core::ByteSpan{p.buffer.data(), p.buffer.size()};
            packets.push_back(std::move(p));

            idx += group.size();
            first_packet = false;
            continue;
        }

        // ---- Fragment it ----
        //
        // The first fragment carries the OBU header plus as much payload
        // as fits; the following fragments carry payload only.
        //
        // The last fragment's length is derived from the packet length
        // (W = 1, no length field), which is why every fragment packet
        // here has exactly one element.
        const std::size_t first_capacity = mtu - overhead - e.header_size;
        if (first_capacity == 0) return false;

        // The OBU payload starts after the header; `offset` indexes into
        // the payload, not into the whole OBU.
        const std::size_t payload_len = e.data.size() - e.header_size;
        std::size_t offset = 0;
        bool first_fragment = true;
        while (offset < payload_len) {
            const std::size_t remaining = payload_len - offset;
            std::size_t capacity = first_fragment ? first_capacity
                                                  : (mtu - overhead);
            if (capacity == 0) return false;

            const bool last = remaining <= capacity;
            const std::size_t take = last ? remaining : capacity;

            Packet p;
            // The first fragment additionally carries the OBU header, so
            // its buffer is larger by `header_size`.
            p.buffer.resize(overhead + (first_fragment ? e.header_size : 0) + take);

            AggregationHeader agg;
            agg.starts_with_fragment = !first_fragment;
            agg.ends_with_fragment   = !last;
            agg.element_count        = 1;
            agg.new_sequence         = first_packet && first_fragment && new_sequence;
            write_aggregation_header(agg, core::MutableByteSpan{p.buffer.data(),
                                                                 p.buffer.size()});

            if (first_fragment) {
                // The header travels with the first fragment.
                std::memcpy(p.buffer.data() + 1, e.data.data(), e.header_size);
                std::memcpy(p.buffer.data() + 1 + e.header_size,
                            e.data.data() + e.header_size + offset, take);
            } else {
                std::memcpy(p.buffer.data() + 1,
                            e.data.data() + e.header_size + offset, take);
            }

            p.payload = core::ByteSpan{p.buffer.data(), p.buffer.size()};
            packets.push_back(std::move(p));

            offset += take;
            first_fragment = false;
            first_packet = false;
        }
        ++idx;
    }

    // Only the *last* packet of the frame carries the marker bit.
    for (std::size_t i = 0; i + 1 < packets.size(); ++i) {
        packets[i].marker = false;
    }
    if (!packets.empty()) packets.back().marker = true;

    return !packets.empty();
}

// ---------------------------------------------------------------------------
// Reassembly
// ---------------------------------------------------------------------------

bool ObuReassembler::push(core::ByteSpan payload,
                           std::vector<std::uint8_t>& annex_b) noexcept {
    std::vector<ObuElement> elements;
    if (!parse_elements(payload, elements)) return false;

    for (const auto& e : elements) {
        if (e.carries_header) {
            // A new OBU starts here.  A pending one that was never
            // completed (a lost packet) is dropped — the decoder cannot
            // use a truncated OBU anyway, and keeping it would corrupt
            // every OBU that follows.
            pending_.clear();
            pending_open_ = true;
            pending_.insert(pending_.end(), e.data.begin(), e.data.end());
            if (e.type == ObuType::kSequenceHeader) saw_sequence_header_ = true;
        } else {
            if (!pending_open_) {
                // A continuation without a start: the stream is broken
                // here.  Drop it and resynchronise on the next OBU.
                continue;
            }
            pending_.insert(pending_.end(), e.data.begin(), e.data.end());
        }

        if (!e.is_last_fragment) continue;   // the OBU continues in the next packet

        // The OBU is complete: emit it in Annex-B framing.
        annex_b.insert(annex_b.end(), kAnnexBStartCode,
                       kAnnexBStartCode + sizeof(kAnnexBStartCode));
        annex_b.insert(annex_b.end(), pending_.begin(), pending_.end());
        pending_.clear();
        pending_open_ = false;
    }
    return true;
}

void ObuReassembler::reset() noexcept {
    pending_.clear();
    pending_open_ = false;
    saw_sequence_header_ = false;
}

} // namespace nimrtc::video_payload::av1
