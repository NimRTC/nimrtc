/**
 * @file src/web_transport/src/web_transport_qpack.cpp
 * @brief QPACK header compression for HTTP/3 (RFC 9204).
 *
 * See web_transport_qpack.hpp for the interface description.
 */

#include "web_transport_qpack.hpp"

#include <algorithm>
#include <cstring>

#include "qpack_tables.hpp"

namespace nimrtc::web_transport::detail {

namespace {

namespace tables = qpack_tables;

/** @brief Number of entries in the RFC 9204 static table. */
constexpr std::size_t kStaticTableSize =
    sizeof(tables::kStaticTable) / sizeof(tables::kStaticTable[0]);

/** @brief Size in bytes of a dynamic table entry (RFC 9204 §3.2.1). */
constexpr std::uint64_t kEntryOverhead = 32;

// The Required Insert Count reconstruction in QpackDecoder::decode() divides
// by 2 * (capacity / entry overhead), so the advertised capacity must allow
// at least one entry.  Asserting it here keeps that division safe without a
// runtime check on a compile-time-constant value (which MSVC would report as
// C4127 "conditional expression is constant").
static_assert(QpackDecoder::kMaxTableCapacity >= kEntryOverhead,
              "the dynamic table must hold at least one entry");

/**
 * @brief Append a prefixed integer (RFC 9204 §4.1.1).
 *
 * @param out      Output buffer.
 * @param bits     Width of the prefix in bits (1..8).
 * @param prefix   Bits already present in the first byte (e.g. 0xC0).
 * @param value    Value to encode.
 */
void encode_prefixed_int(std::vector<std::uint8_t>& out,
                         unsigned bits,
                         std::uint8_t prefix,
                         std::uint64_t value) {
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    if (value < mask) {
        out.push_back(static_cast<std::uint8_t>(prefix | value));
        return;
    }
    out.push_back(static_cast<std::uint8_t>(prefix | mask));
    std::uint64_t rest = value - mask;
    while (rest >= 128) {
        out.push_back(static_cast<std::uint8_t>((rest % 128) + 128));
        rest /= 128;
    }
    out.push_back(static_cast<std::uint8_t>(rest));
}

/**
 * @brief Append an uncompressed string literal (RFC 9204 §4.1.2).
 *
 * The Huffman flag is left clear, which is always legal.
 */
void encode_string_literal(std::vector<std::uint8_t>& out,
                            const std::string& value) {
    encode_prefixed_int(out, 7, 0x00, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

/**
 * @brief Look up an exact (name, value) pair in the static table.
 * @return The static table index, or -1 when there is no match.
 */
int find_static_entry(const std::string& name, const std::string& value) {
    for (std::size_t i = 0; i < kStaticTableSize; ++i) {
        if (name == tables::kStaticTable[i].name &&
            value == tables::kStaticTable[i].value) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/**
 * @brief Look up a field name in the static table.
 * @return The static table index, or -1 when there is no match.
 */
int find_static_name(const std::string& name) {
    for (std::size_t i = 0; i < kStaticTableSize; ++i) {
        if (name == tables::kStaticTable[i].name) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

// ===========================================================================
// QpackDecoder — primitives
// ===========================================================================

bool QpackDecoder::decode_prefixed_int(std::span<const std::uint8_t>& data,
                                        unsigned bits,
                                        std::uint64_t& value) noexcept {
    if (bits == 0 || bits > 8) return false;
    if (data.empty()) return false;

    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    std::uint64_t val = data[0] & mask;
    if (val < mask) {
        value = val;
        data = data.subspan(1);
        return true;
    }

    // The prefix was exhausted: the remainder follows as a QUIC-style
    // variable-length integer (7 bits per byte, high bit = continuation).
    std::uint64_t extra = 0;
    unsigned shift = 0;
    std::size_t i = 1;
    while (i < data.size()) {
        const std::uint8_t b = data[i++];
        extra |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            value = mask + extra;
            data = data.subspan(i);
            return true;
        }
        shift += 7;
        if (shift > 56) return false;  // absurdly long encoding
    }
    return false;  // truncated
}

bool QpackDecoder::decode_string(std::span<const std::uint8_t>& data,
                                  unsigned bits,
                                  std::string& out) noexcept {
    if (bits < 2 || bits > 8) return false;
    if (data.empty()) return false;

    const bool huffman = (data[0] & (1u << (bits - 1))) != 0;

    std::uint64_t length = 0;
    if (!decode_prefixed_int(data, bits - 1, length)) return false;
    if (length > data.size()) return false;

    const std::span<const std::uint8_t> raw = data.first(
        static_cast<std::size_t>(length));
    data = data.subspan(static_cast<std::size_t>(length));

    out.clear();
    if (!huffman) {
        out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
        return true;
    }
    return huffman_decode(raw, out);
}

bool QpackDecoder::huffman_decode(std::span<const std::uint8_t> data,
                                  std::string& out) noexcept {
    out.clear();

    std::uint64_t code = 0;
    unsigned len = 0;

    for (const std::uint8_t byte : data) {
        for (int bit = 7; bit >= 0; --bit) {
            code = (code << 1) | ((byte >> bit) & 1u);
            ++len;
            if (len > static_cast<unsigned>(tables::kHuffmanMaxBits)) {
                return false;
            }

            const std::uint16_t count = tables::kHuffmanCount[len];
            if (count == 0) continue;

            const std::uint32_t first = tables::kHuffmanFirstCode[len];
            if (code < first || code >= first + count) continue;

            const std::uint16_t symbol = tables::kHuffmanSymbols[
                tables::kHuffmanSymbolStart[len] +
                static_cast<std::uint16_t>(code - first)];

            if (symbol == 256) {
                // EOS must not appear in the encoded string.
                return false;
            }
            out.push_back(static_cast<char>(symbol));
            code = 0;
            len = 0;
        }
    }

    // Any remaining bits must be the EOS prefix (all ones) and shorter
    // than one byte.
    if (len > 7) return false;
    if (len > 0 && code != ((std::uint64_t{1} << len) - 1)) return false;

    return true;
}

// ===========================================================================
// QpackDecoder — dynamic table
// ===========================================================================

void QpackDecoder::evict_oldest() noexcept {
    if (dynamic_table_.empty()) return;
    const DynamicEntry& e = dynamic_table_.front();
    size_ -= e.name.size() + e.value.size() + kEntryOverhead;
    dynamic_table_.erase(dynamic_table_.begin());
    ++dropped_count_;
}

void QpackDecoder::set_capacity(std::uint64_t capacity) noexcept {
    capacity_ = capacity;
    while (size_ > capacity_ && !dynamic_table_.empty()) {
        evict_oldest();
    }
}

void QpackDecoder::insert_entry(std::string name, std::string value) noexcept {
    const std::uint64_t entry_size =
        name.size() + value.size() + kEntryOverhead;

    if (capacity_ == 0 || entry_size > capacity_) {
        // The encoder must not insert into an empty table, and an entry
        // larger than the capacity can never be stored.
        failed_ = true;
        return;
    }

    while (size_ + entry_size > capacity_ && !dynamic_table_.empty()) {
        evict_oldest();
    }

    size_ += entry_size;
    dynamic_table_.push_back(DynamicEntry{std::move(name), std::move(value)});
    ++insert_count_;
}

const QpackDecoder::DynamicEntry* QpackDecoder::entry_by_absolute(
    std::uint64_t absolute_index) const noexcept {
    if (absolute_index < dropped_count_) return nullptr;
    const std::uint64_t offset = absolute_index - dropped_count_;
    if (offset >= dynamic_table_.size()) return nullptr;
    return &dynamic_table_[static_cast<std::size_t>(offset)];
}

const QpackDecoder::DynamicEntry* QpackDecoder::entry_by_relative(
    std::uint64_t relative_index, std::uint64_t base) const noexcept {
    if (relative_index >= base) return nullptr;
    return entry_by_absolute(base - 1 - relative_index);
}

// ===========================================================================
// QpackDecoder — field section decoding
// ===========================================================================

bool QpackDecoder::decode(std::span<const std::uint8_t> block,
                           std::vector<QpackHeaderField>& out) noexcept {
    out.clear();

    std::span<const std::uint8_t> rem = block;

    // ---- Encoded field section prefix (RFC 9204 §4.5.1) ----
    std::uint64_t encoded_insert_count = 0;
    if (!decode_prefixed_int(rem, 8, encoded_insert_count)) return false;

    if (rem.empty()) return false;
    const bool negative_base = (rem[0] & 0x40) != 0;

    std::uint64_t delta_base = 0;
    if (!decode_prefixed_int(rem, 7, delta_base)) return false;

    // Reconstruct the Required Insert Count (RFC 9204 §4.5.1.1).
    const std::uint64_t max_entries = kMaxTableCapacity / kEntryOverhead;
    std::uint64_t required_insert_count = 0;
    if (encoded_insert_count != 0) {
        const std::uint64_t full_range = 2 * max_entries;
        if (encoded_insert_count > full_range) return false;

        const std::uint64_t max_value = insert_count_ + max_entries;
        const std::uint64_t max_wrapped = (max_value / full_range) * full_range;
        std::uint64_t ric = max_wrapped + encoded_insert_count - 1;
        if (ric > max_value) {
            if (ric <= full_range) return false;
            ric -= full_range;
        }
        if (ric == 0) return false;
        required_insert_count = ric;
    }

    // Base (RFC 9204 §4.5.1.2).
    std::uint64_t base = 0;
    if (negative_base) {
        if (required_insert_count <= delta_base) return false;
        base = required_insert_count - delta_base - 1;
    } else {
        base = required_insert_count + delta_base;
    }

    // ---- Field line representations (RFC 9204 §4.5.2 - §4.5.6) ----
    while (!rem.empty()) {
        const std::uint8_t first = rem[0];
        std::string name;
        std::string value;

        if ((first & 0x80) != 0) {
            // Indexed Field Line: 1 T Index(6+)
            const bool is_static = (first & 0x40) != 0;
            std::uint64_t index = 0;
            if (!decode_prefixed_int(rem, 6, index)) return false;

            if (is_static) {
                if (index >= kStaticTableSize) return false;
                name  = tables::kStaticTable[index].name;
                value = tables::kStaticTable[index].value;
            } else {
                const DynamicEntry* e = entry_by_relative(index, base);
                if (e == nullptr) return false;
                name  = e->name;
                value = e->value;
            }
        } else if ((first & 0xC0) == 0x40) {
            // Literal Field Line with Name Reference: 01 N T Name Index(4+)
            const bool is_static = (first & 0x10) != 0;
            std::uint64_t index = 0;
            if (!decode_prefixed_int(rem, 4, index)) return false;

            if (is_static) {
                if (index >= kStaticTableSize) return false;
                name = tables::kStaticTable[index].name;
            } else {
                const DynamicEntry* e = entry_by_relative(index, base);
                if (e == nullptr) return false;
                name = e->name;
            }
            if (!decode_string(rem, 8, value)) return false;
        } else if ((first & 0xE0) == 0x20) {
            // Literal Field Line with Literal Name: 001 N H NameLen(3+)
            if (!decode_string(rem, 4, name)) return false;
            if (!decode_string(rem, 8, value)) return false;
        } else if ((first & 0xF0) == 0x10) {
            // Indexed Field Line with Post-Base Index: 0001 Index(4+)
            std::uint64_t index = 0;
            if (!decode_prefixed_int(rem, 4, index)) return false;
            const DynamicEntry* e = entry_by_absolute(base + index);
            if (e == nullptr) return false;
            name  = e->name;
            value = e->value;
        } else {
            // Literal Field Line with Post-Base Name Reference:
            // 0000 N NameIdx(3+)
            std::uint64_t index = 0;
            if (!decode_prefixed_int(rem, 3, index)) return false;
            const DynamicEntry* e = entry_by_absolute(base + index);
            if (e == nullptr) return false;
            name = e->name;
            if (!decode_string(rem, 8, value)) return false;
        }

        out.push_back(QpackHeaderField{std::move(name), std::move(value)});
    }

    return true;
}

// ===========================================================================
// QpackDecoder — encoder stream (RFC 9204 §4.3)
// ===========================================================================

void QpackDecoder::feed_encoder_stream(
    std::span<const std::uint8_t> data) noexcept {
    encoder_buffer_.insert(encoder_buffer_.end(), data.begin(), data.end());

    std::span<const std::uint8_t> rem{encoder_buffer_.data(),
                                       encoder_buffer_.size()};

    while (!rem.empty()) {
        const std::uint8_t first = rem[0];
        std::span<const std::uint8_t> next = rem;
        bool complete = false;

        if ((first & 0x80) != 0) {
            // Insert with Name Reference: 1 T Name Index(6+)
            const bool is_static = (first & 0x40) != 0;
            std::uint64_t index = 0;
            if (!decode_prefixed_int(next, 6, index)) return;  // need more
            std::string value;
            if (!decode_string(next, 8, value)) return;  // need more

            std::string name;
            if (is_static) {
                if (index >= kStaticTableSize) {
                    failed_ = true;
                    return;
                }
                name = tables::kStaticTable[index].name;
            } else {
                if (index >= insert_count_) {
                    failed_ = true;
                    return;
                }
                const DynamicEntry* e =
                    entry_by_absolute(insert_count_ - 1 - index);
                if (e == nullptr) {
                    failed_ = true;
                    return;
                }
                name = e->name;
            }
            insert_entry(std::move(name), std::move(value));
            complete = true;
        } else if ((first & 0xC0) == 0x40) {
            // Insert with Literal Name: 01 H NameLen(5+)
            std::string name;
            std::string value;
            if (!decode_string(next, 6, name)) return;  // need more
            if (!decode_string(next, 8, value)) return;  // need more
            insert_entry(std::move(name), std::move(value));
            complete = true;
        } else if ((first & 0xE0) == 0x20) {
            // Set Dynamic Table Capacity: 001 Capacity(5+)
            std::uint64_t capacity = 0;
            if (!decode_prefixed_int(next, 5, capacity)) return;  // need more
            if (capacity > kMaxTableCapacity) {
                failed_ = true;
                return;
            }
            set_capacity(capacity);
            complete = true;
        } else {
            // Duplicate: 000 Index(5+)
            std::uint64_t index = 0;
            if (!decode_prefixed_int(next, 5, index)) return;  // need more
            if (index >= insert_count_) {
                failed_ = true;
                return;
            }
            const DynamicEntry* e =
                entry_by_absolute(insert_count_ - 1 - index);
            if (e == nullptr) {
                failed_ = true;
                return;
            }
            insert_entry(e->name, e->value);
            complete = true;
        }

        if (!complete) return;
        rem = next;
    }

    encoder_buffer_.clear();
}

// ===========================================================================
// Field section encoding (static table only)
// ===========================================================================

bool encode_field_section(const std::vector<QpackHeaderField>& fields,
                           std::vector<std::uint8_t>& out) noexcept {
    out.clear();

    // Header block prefix: Required Insert Count = 0, Base = 0.
    out.push_back(0x00);
    out.push_back(0x00);

    for (const QpackHeaderField& field : fields) {
        const int exact = find_static_entry(field.name, field.value);
        if (exact >= 0) {
            // Indexed Field Line with Static Table Index: 1 T=1 Index(6+)
            encode_prefixed_int(out, 6, 0xC0,
                                static_cast<std::uint64_t>(exact));
            continue;
        }

        const int name_only = find_static_name(field.name);
        if (name_only >= 0) {
            // Literal Field Line with Name Reference: 01 N=0 T=1 Index(4+)
            encode_prefixed_int(out, 4, 0x50,
                                static_cast<std::uint64_t>(name_only));
            encode_string_literal(out, field.value);
            continue;
        }

        // Literal Field Line with Literal Name: 001 N=0 H=0 NameLen(3+)
        encode_prefixed_int(out, 3, 0x20,
                            static_cast<std::uint64_t>(field.name.size()));
        out.insert(out.end(), field.name.begin(), field.name.end());
        encode_string_literal(out, field.value);
    }

    return true;
}

}  // namespace nimrtc::web_transport::detail
