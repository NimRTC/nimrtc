// tcp_framing.hpp — RFC 6544 §3.2 stream framing for ICE-over-TCP.
//
// ICE over TCP runs over a byte stream, not datagrams: one recv() is not
// one STUN message.  RFC 6544 §3.2 frames every STUN message with a
// two-octet unsigned length prefix in network byte order.  The length is
// the size of the STUN message *including* its 20-byte STUN header (the
// RFC 4571 framing semantics RFC 6544 §3.2 adopts) and *excluding* the
// two-octet framing prefix itself.

#ifndef NIMRTC_ICE_TCP_FRAMING_HPP
#define NIMRTC_ICE_TCP_FRAMING_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nimrtc::ice {

/// Prepend the RFC 6544 §3.2 two-octet length prefix to a STUN message.
/// Returns the framed bytes, or an empty vector when the message is too
/// large to frame (length would not fit a 16-bit field).
std::vector<std::uint8_t> frame_message(std::span<const std::uint8_t> stun_message);

/// Consumes a byte stream and emits complete STUN messages.
///
/// Stateful by design: TCP is a stream, so a frame boundary rarely lines
/// up with a recv() boundary.  Feed every received chunk with push(); it
/// buffers the partial tail and hands back each complete message.
class TcpUnframer {
public:
    /// Feed received bytes.  Appends every complete STUN message to
    /// `out`.  Returns false on a framing error (a length that cannot be
    /// a STUN message), in which case the stream is no longer usable.
    bool push(std::span<const std::uint8_t> bytes,
              std::vector<std::vector<std::uint8_t>>& out);

    /// True when nothing is buffered.
    bool empty() const noexcept { return buffer_.empty(); }

    /// Drop all buffered bytes (start over on a fresh connection).
    void reset() noexcept { buffer_.clear(); }

private:
    std::vector<std::uint8_t> buffer_;
};

}  // namespace nimrtc::ice

#endif  // NIMRTC_ICE_TCP_FRAMING_HPP
