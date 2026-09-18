/**
 * @file nimrtc/plugins/base.hpp
 * @brief Base types for plugin interfaces.
 *
 * All plugin interfaces use these shared types. They live here so
 * implementations and consumers share a common vocabulary without
 * depending on any concrete module.
 *
 * @note P0 scaffold. Types are stable; no binary layout until P1.
 *
 * Buffer types are unified with nimrtc::core::ByteSpan / MutableByteSpan
 * so the plugin layer and module layer use identical types throughout.
 */

#ifndef NIMRTC_PLUGINS_BASE_HPP
#define NIMRTC_PLUGINS_BASE_HPP

#include <cstdint>
#include <cstring>   // memset, memcmp
#include <functional>   // Slice 8: std::function for OnDatagramCb / OnSctpRecvCb
#include <optional>

// Stable buffer types shared by plugin interfaces and concrete modules.
#include <nimrtc/core/bytes.hpp>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Result / Status
// ---------------------------------------------------------------------------

/** Opaque status code, zero = success. */
using Status = uint32_t;

constexpr Status kOk               = 0x0000;
constexpr Status kErrInvalidParam  = 0x1001;
constexpr Status kErrNotReady     = 0x1002;
constexpr Status kErrBufferTooSmall= 0x1003;
constexpr Status kErrCorrupt      = 0x1004;
constexpr Status kErrUnsupported   = 0x1005;
constexpr Status kErrHardwareError = 0x1006;  // GPU SDK / driver failure
constexpr Status kErrInternal      = 0x1FFF;

/** Human-readable status message, may be nullptr. */
inline const char* status_string(Status s) noexcept {
    switch (s) {
        case kOk:               return "ok";
        case kErrInvalidParam:  return "invalid parameter";
        case kErrNotReady:      return "not ready";
        case kErrBufferTooSmall:return "buffer too small";
        case kErrCorrupt:       return "corrupt data";
        case kErrUnsupported:   return "unsupported operation";
        case kErrHardwareError: return "hardware/driver error";
        default:                return "unknown error";
    }
}

// ---------------------------------------------------------------------------
// Timestamp (wall-clock microseconds)
// ---------------------------------------------------------------------------

/** Monotonic microsecond counter (same epoch as std::chrono). */
using TimestampUs = int64_t;

/** RTP NTP timestamp fraction scale (2^32). */
using Ntp64 = uint64_t;

// ---------------------------------------------------------------------------
// Network address (opaque, protocol-agnostic)
// ---------------------------------------------------------------------------

/** Maximum size of an encoded network address. */
constexpr size_t kMaxAddrLen = 64;

/** Opaque network address. Transport implementations own the format. */
struct Addr {
    uint8_t data[kMaxAddrLen] = {0};
    uint32_t len = 0;
};

inline bool operator==(const Addr& a, const Addr& b) noexcept {
    return a.len == b.len && std::memcmp(a.data, b.data, a.len) == 0;
}
inline bool operator!=(const Addr& a, const Addr& b) noexcept { return !(a == b); }

// ---------------------------------------------------------------------------
// Buffer view — unified with nimrtc::core::ByteSpan
// ---------------------------------------------------------------------------

/** Read-only view over an inbound packet buffer.
 *  Alias for nimrtc::core::ByteSpan for zero-overhead interoperability
 *  between the plugin interface layer and the module layer. */
using BufferView = core::ByteSpan;

/** Mutable outbound packet descriptor.
 *  Wraps nimrtc::core::MutableByteSpan with an additional destination address. */
struct OutPacket {
    core::MutableByteSpan buf;  // data + capacity
    size_t               len    = 0;  // valid bytes written
    Addr                dst_addr;    // destination address

    OutPacket() noexcept = default;
    explicit OutPacket(core::MutableByteSpan b) noexcept : buf(b) {}

    /** Pointer to writable region. Returns nullptr if needed > capacity. */
    [[nodiscard]] uint8_t* prepare(size_t needed) noexcept {
        if (needed > buf.size()) return nullptr;
        return buf.data();
    }

    /** Commit written bytes. */
    void commit(size_t n) noexcept {
        if (n <= buf.size()) len = n;
    }

    /** Current span of valid data (first len bytes). */
    [[nodiscard]] core::ByteSpan valid_data() const noexcept {
        return buf.first(len);
    }
};

// ---------------------------------------------------------------------------
// Transport-layer callbacks (Transport PAL Slice 8 — promoted from
// Slice 5/6 local typedefs to plugins/base.hpp)
// ---------------------------------------------------------------------------
//
// §6.5 of `docs/plan/transport-selection.md` calls for the callback
// types originally defined in Slice 5 (`OnSctpRecvCb` in
// `nimrtc/sctp/sctp_socket_iface.hpp`) and Slice 6 (`OnDatagramCb` +
// `Endpoint` in `nimrtc/raw_udp/raw_udp_datagram.hpp`) to be promoted
// into `plugins/base.hpp` once Slice 4/5/6 settled on the final
// shape. Slice 8 (v0.10.2) makes that promotion: the typedefs are now
// canonical here, and the Slice 5/6 headers re-export them as
// aliases for source-compat (callers that wrote `plugins::OnSctpRecvCb`
// before Slice 8 keep compiling).
//
// Why canonicalise here:
//   (a) `OnSctpRecvCb` and `OnDatagramCb` are the canonical callback
//       shapes used across the SCTP / raw_udp seams — they belong in
//       the same place as `BufferView` / `Status` / `Addr`.
//   (b) Future Slice 8.5+ code that needs a similar callback (e.g.
//       for a future QUIC datagram channel) gets the type for free
//       without having to know which module header it lives in.
//   (c) `Endpoint` was a local alias of `Addr` in raw_udp; promoting
//       it removes the local definition so there's a single source of
//       truth for "raw-UDP endpoint == ITransport endpoint".
// ---------------------------------------------------------------------------

/** Network endpoint for the raw-UDP bypass seam. Typed alias of
 *  `plugins::Addr` — raw UDP talks to the same opaque address
 *  encoding that `ITransport::send` / `recv` already use.
 *  Promoted from raw_udp_datagram.hpp in Slice 8. */
using Endpoint = Addr;

/** Per-datagram receive callback used by IRawUdpDatagram::on_recv.
 *  Fires from the ARQ recv thread on each in-window, in-order
 *  delivered datagram. Promoted from raw_udp_datagram.hpp in Slice 8. */
using OnDatagramCb = std::function<void(BufferView)>;

/** Inbound SCTP message callback used by ISctpSocket::set_on_recv.
 *  `stream` is the SCTP stream id (0..65534) the message arrived on;
 *  `data` is the message payload (zero-copy view; lifetime ends when
 *  the callback returns). Promoted from sctp_socket_iface.hpp in
 *  Slice 8. */
using OnSctpRecvCb = std::function<void(uint16_t stream,
                                        core::ByteSpan data)>;

// ---------------------------------------------------------------------------
// Media sample (aligned to RTP payload)
// ---------------------------------------------------------------------------

/** Uncompressed audio/video sample descriptor. */
struct MediaSample {
    enum class Kind : uint8_t { kAudio = 0, kVideo = 1 };
    Kind kind = Kind::kAudio;

    /** RTP SSRC this sample belongs to. */
    uint32_t ssrc = 0;

    /** RTP timestamp of the first byte. */
    uint32_t rtp_ts = 0;

    /** Media time in microseconds (from RTP->NTP conversion). */
    TimestampUs media_us = 0;

    /** Encoded payload — unified with core::ByteSpan. */
    core::ByteSpan payload;

    /** Video only: whether this is a key frame (I-frame). */
    bool is_keyframe = false;

    /** Video only: RTP sequence number. */
    uint16_t seq = 0;
};

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

/** Every plugin implements this interface. */
class IPlugin {
public:
    virtual ~IPlugin() = default;

    /** Human-readable plugin name, e.g. "NimRTC-Transport-WebRTC". */
    virtual const char* name() const noexcept = 0;

    /** Open-phase initialization. Called before any other method.
     *  @return kOk on success. */
    virtual Status open() noexcept = 0;

    /** Tear-down. Called when the engine stops or plugin is swapped.
     *  After close(), the plugin may be re-opened. */
    virtual void close() noexcept = 0;
};

// ---------------------------------------------------------------------------
// Plugin factory base (per §8.6 P1)
// ---------------------------------------------------------------------------

/** Base class for all plugin factories.
 *  Concrete factories (e.g. IDataChannelFactory) inherit from this so
 *  the PluginRegistry can hold heterogeneous factory types uniformly.
 *  @note P1 scaffold — stable interface, binary layout TBD P4. */
class IPluginFactory {
public:
    virtual ~IPluginFactory() = default;

    /** Unique identifier, e.g. "sctp", "quic". */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name, e.g. "SCTP (usrsctp)". */
    virtual std::string_view display_name() const noexcept = 0;
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_BASE_HPP
