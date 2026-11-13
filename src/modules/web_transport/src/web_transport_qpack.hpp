/**
 * @file src/web_transport/src/web_transport_qpack.hpp
 * @brief QPACK header compression for HTTP/3 (RFC 9204).
 *
 * The NimRTC WebTransport server speaks HTTP/3 (RFC 9114) directly on top of
 * QUIC, so it needs a QPACK codec for the request and response header
 * sections.  This file provides:
 *
 *   - `QpackDecoder` — decodes an encoded field section (header block) into
 *     an ordered list of field lines, including the static table, Huffman
 *     coded string literals and the dynamic table.
 *   - `encode_field_section()` — encodes a field section using static table
 *     references only (the server never populates a dynamic table).
 *
 * The static table and the Huffman code are taken from the generated
 * `qpack_tables.hpp`, which mirrors RFC 9204 Appendix A and Appendix B.
 *
 * ## Reference
 *
 *   RFC 9204 — QPACK: Field Compression for HTTP
 *     §3.2  Dynamic Table
 *     §4.1  Primitives (prefixed integers, string literals)
 *     §4.3  Encoder Instructions
 *     §4.5  Field Line Representations
 */

#ifndef NIMRTC_WEB_TRANSPORT_QPACK_HPP
#define NIMRTC_WEB_TRANSPORT_QPACK_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nimrtc::web_transport::detail {

/**
 * @brief One decoded header field line.
 *
 * Field names are stored verbatim as they appeared on the wire; HTTP/3
 * requires the sender to use lowercase field names, so no case folding is
 * performed here.
 */
struct QpackHeaderField {
    std::string name;
    std::string value;
};

/**
 * @brief Outcome of decoding a QPACK string literal.
 *
 * The distinction between "the buffer does not hold the whole literal yet"
 * and "the literal is malformed" matters for the encoder stream, where an
 * instruction may be split across several QUIC segments: an incomplete
 * literal must be buffered, a malformed one is a connection error.
 */
enum class QpackStringStatus {
    /** @brief The literal was decoded successfully. */
    kOk,
    /** @brief More bytes are needed to decode the literal. */
    kIncomplete,
    /** @brief The literal is malformed (bad Huffman code or bad padding). */
    kInvalid,
};

/**
 * @brief RFC 9204 QPACK decoder.
 *
 * One instance is used per HTTP/3 connection: the dynamic table is a
 * connection-wide resource shared by every request stream.
 *
 * Thread-safety: the caller (the HTTP/3 handler) serialises all calls with
 * its own mutex; the class itself is not internally synchronised.
 */
class QpackDecoder {
public:
    /**
     * @brief Maximum dynamic table capacity this decoder accepts.
     *
     * The value is advertised to the peer via
     * SETTINGS_QPACK_MAX_TABLE_CAPACITY and bounds the memory a peer can
     * make the decoder allocate.
     */
    static constexpr std::uint32_t kMaxTableCapacity = 4096;

    QpackDecoder() = default;

    /**
     * @brief Decode an encoded field section.
     *
     * @param block  Raw QPACK header block (the HEADERS frame payload).
     * @param out    Receives the decoded field lines in wire order.
     * @return True on success.  On failure @p out is left empty.
     */
    [[nodiscard]] bool decode(std::span<const std::uint8_t> block,
                              std::vector<QpackHeaderField>& out) noexcept;

    /**
     * @brief Feed bytes received on the peer's QPACK encoder stream.
     *
     * The encoder stream is an unframed byte stream (RFC 9204 §4.3), so
     * instructions may be split across TCP/QUIC segments.  Incomplete
     * trailing data is buffered until the next call.
     */
    void feed_encoder_stream(std::span<const std::uint8_t> data) noexcept;

    /**
     * @brief True if a previously fed encoder stream or header block was
     *        malformed.
     *
     * A malformed encoder stream is a connection error of type
     * QPACK_ENCODER_STREAM_ERROR (RFC 9204 §6).
     */
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    /** @brief Clear the sticky error flag. */
    void clear_error() noexcept { failed_ = false; }

    /**
     * @brief Number of entries currently held by the dynamic table.
     */
    [[nodiscard]] std::size_t dynamic_table_size() const noexcept {
        return dynamic_table_.size();
    }

    // ---------------------------------------------------------------------
    // Primitives (RFC 9204 §4.1) — exposed for unit testing.
    // ---------------------------------------------------------------------

    /**
     * @brief Decode a prefixed integer (RFC 9204 §4.1.1).
     *
     * @param data  In/out cursor; advanced past the integer on success.
     * @param bits  Width of the prefix in bits (1..8).
     * @param value Receives the decoded value.
     * @return False when @p data does not hold a complete integer.
     */
    static bool decode_prefixed_int(std::span<const std::uint8_t>& data,
                                     unsigned bits,
                                     std::uint64_t& value) noexcept;

    /**
     * @brief Decode a string literal (RFC 9204 §4.1.2).
     *
     * @param data  In/out cursor; advanced past the literal on success.
     * @param bits  Width of the literal's prefix in bits (2..8).  The
     *              highest of those bits is the Huffman flag, the remaining
     *              bits carry the length.
     * @param out   Receives the decoded string.
     * @return False when @p data does not hold a complete, valid literal.
     */
    static bool decode_string(std::span<const std::uint8_t>& data,
                               unsigned bits,
                               std::string& out) noexcept;

    /**
     * @brief Decode a Huffman-coded string (RFC 9204 Appendix B).
     *
     * @return False when the input is not a valid Huffman string (bad
     *         padding or an embedded EOS symbol).
     */
    static bool huffman_decode(std::span<const std::uint8_t> data,
                               std::string& out) noexcept;

private:
    /** @brief One entry of the dynamic table. */
    struct DynamicEntry {
        std::string name;
        std::string value;
    };

    /** @brief Insert an entry into the dynamic table (RFC 9204 §3.2.2). */
    void insert_entry(std::string name, std::string value) noexcept;

    /** @brief Change the dynamic table capacity, evicting as needed. */
    void set_capacity(std::uint64_t capacity) noexcept;

    /** @brief Evict the oldest entry. */
    void evict_oldest() noexcept;

    /**
     * @brief Look up an entry by absolute index (RFC 9204 §3.2.4).
     * @return nullptr when the index is not (or no longer) in the table.
     */
    [[nodiscard]] const DynamicEntry* entry_by_absolute(
        std::uint64_t absolute_index) const noexcept;

    /** @brief Resolve a relative index against the current Base. */
    [[nodiscard]] const DynamicEntry* entry_by_relative(
        std::uint64_t relative_index, std::uint64_t base) const noexcept;

    // Dynamic table state.
    std::vector<DynamicEntry> dynamic_table_;  // oldest entry first
    std::uint64_t insert_count_ = 0;            // absolute index of next insert
    std::uint64_t dropped_count_ = 0;           // number of evicted entries
    std::uint64_t capacity_ = 0;                // capacity chosen by the peer
    std::uint64_t size_ = 0;                    // current size in bytes

    // Buffered bytes of the peer's encoder stream.
    std::vector<std::uint8_t> encoder_buffer_;

    // Sticky error flag.
    bool failed_ = false;
};

/**
 * @brief Encode a field section using static table references only.
 *
 * The server never inserts into a dynamic table, so the encoded section
 * always carries a Required Insert Count of 0 and a Base of 0.
 *
 * @param fields  Field lines to encode (pseudo-headers first).
 * @param out     Receives the encoded field section.
 * @return True on success.
 */
[[nodiscard]] bool encode_field_section(
    const std::vector<QpackHeaderField>& fields,
    std::vector<std::uint8_t>& out) noexcept;

}  // namespace nimrtc::web_transport::detail

#endif  // NIMRTC_WEB_TRANSPORT_QPACK_HPP
