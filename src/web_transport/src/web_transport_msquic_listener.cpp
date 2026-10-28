/**
 * @file src/web_transport/src/web_transport_msquic_listener.cpp
 * @brief WebTransportMsQuicListener implementation (Case E, v0.14.0-beta.2).
 *
 * Implements a WebTransport server using msquic's listener API:
 *   - MsQuicListenerOpen + MsQuicListenerStart → UDP port bound
 *   - QUIC_LISTENER_CALLBACK → accept incoming QUIC connections
 *   - QUIC_CONNECTION_CALLBACK (server-side) → HTTP/3 SETTINGS + HEADERS
 *   - First WebTransport HEADERS frame → WebTransportMsQuicSession created
 *     and delivered via the on_session callback
 *
 * ## HTTP/3 / WebTransport handshake (RFC 9298 + RFC 9114)
 *
 * Chrome WebTransport JS API sends an HTTP/3 CONNECT request on a
 * bidirectional QUIC stream, with:
 *   :method: CONNECT
 *   :scheme: https
 *   :authority: host:port
 *   :path: /path
 *   :protocol: webtransport
 *   sec-webtransport-version: ...
 *
 * The server must:
 *   1. Complete QUIC handshake (CONNECTED event).
 *   2. Send HTTP/3 SETTINGS that advertise Extended CONNECT, HTTP
 *      Datagrams, and a WebTransport session limit.  Chromium's
 *      DedicatedWebTransportHttp3Client::DoConnectComplete() calls
 *      session_->SupportsWebTransport() on the received SETTINGS and
 *      maps a false result to ERR_METHOD_NOT_SUPPORTED without ever
 *      sending the CONNECT request.
 *   3. Parse the HEADERS frame (QPACK-encoded) to extract `:protocol: webtransport`.
 *   4. Send a 200 response HEADERS frame.
 *   5. Hand the QUIC connection to a WebTransportMsQuicSession (server-side).
 *
 * Step 3/4 is the HTTP/3 layer — implemented here using a minimal
 * QPACK decoder (RFC 9204) sufficient to parse Chrome's WebTransport
 * request headers.
 *
 * ## Thread model
 *
 * All msquic callbacks fire on msquic's internal worker threads.
 * The HTTP/3 handler dispatches to the on_session callback; no
 * locking is needed at the IWebTransportSession surface level
 * per the contract.
 */

#include "web_transport_msquic_listener.hpp"

#include "web_transport_msquic_send.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Windows macros `min` and `max` conflict with std::min / std::max.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <nimrtc/core/log.hpp>
// Include the session header to get the full ApiTable definition
// and the WebTransportMsQuicSession class.
#include "web_transport_msquic_session.hpp"

// Windows headers must be included at file scope — pulling them in from
// inside a namespace leaks every Win32 declaration into that namespace.
#ifdef _WIN32
#include <windows.h>
#endif

namespace nimrtc::web_transport::detail {

// ===========================================================================
// TLS credential loading (portable across the msquic TLS backends)
// ===========================================================================
//
// demo-wtserver requires a TLS certificate to complete the QUIC (RFC 9001)
// handshake.  msquic binds a different TLS backend per platform and each
// backend only accepts credentials in its own shape:
//
//   - Windows     -> SChannel.  The certificate is referenced by its SHA-1
//                    thumbprint inside a Windows certificate store
//                    (QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE).
//                    scripts/generate_wtserver_cert.ps1 creates the
//                    certificate, installs it in Cert:\CurrentUser\My and
//                    writes the thumbprint to build/wtserver_hash.txt.
//
//   - Linux/macOS -> OpenSSL / quictls.  The certificate and its private key
//                    are read from PEM files
//                    (QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE).
//                    scripts/generate_wtserver_cert.py writes
//                    build/wtserver_cert.pem + build/wtserver_key.pem.
//
// The material is looked up in the first directory that holds it, in this
// order:
//   1. $NIMRTC_BUILD_ROOT         — set by the Chrome interop harness
//   2. NIMRTC_DEFAULT_BUILD_ROOT  — the CMake binary directory (compiled in)
//   3. .                          — the current working directory
//   4. ./..                       — ctest runs from <build>/tests
//
// Chrome is launched with --ignore-certificate-errors so it accepts
// the self-signed cert without a trusted CA — safe for a local test only.
// ===========================================================================

namespace {

/**
 * @brief Directories that may hold the WebTransport TLS material.
 */
std::vector<std::string> credential_search_dirs() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("NIMRTC_BUILD_ROOT"); env && env[0]) {
        dirs.emplace_back(env);
    }
#ifdef NIMRTC_DEFAULT_BUILD_ROOT
    dirs.emplace_back(NIMRTC_DEFAULT_BUILD_ROOT);
#endif
    dirs.emplace_back(".");
    dirs.emplace_back("./..");
    return dirs;
}

/**
 * @brief Locate `name` in the first candidate directory that holds it.
 *
 * @return true on success (out_path is set), false when the file is absent.
 */
bool find_credential_file(const char* name, std::string& out_path) {
    for (const std::string& dir : credential_search_dirs()) {
        std::string candidate = dir + "/" + name;
        std::ifstream probe(candidate, std::ios::binary);
        if (probe) {
            probe.close();
            out_path = candidate;
            return true;
        }
    }
    return false;
}

}  // anonymous namespace

#ifdef _WIN32

namespace {

/**
 * Load a certificate from the Windows certificate store and install it
 * in the msquic server configuration for TLS 1.3 handshake.
 *
 * Uses QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE which references the
 * certificate by its SHA-1 hash (the thumbprint) in a named store.
 *
 * @return true on success, false on failure.
 */
bool load_cert_hash_tls_credential(
    const ApiTable* api, HQUIC configuration) noexcept {

    // Read the SHA-1 hash from the file written by the cert generation script.
    // The file lives in the build directory (build/wtserver_hash.txt).
    std::string hash_path;
    if (!find_credential_file("wtserver_hash.txt", hash_path)) {
        nimrtc::core::log::Logger::instance().error(
            "Cannot find wtserver_hash.txt in the build directory "
            "(run scripts/generate_wtserver_cert.ps1 first)");
        return false;
    }
    std::ifstream hf(hash_path);
    if (!hf) {
        nimrtc::core::log::Logger::instance().error(
            "Cannot open certificate hash file: " + hash_path +
            " (run scripts/generate_wtserver_cert.ps1 first)");
        return false;
    }
    std::string hash_hex;
    hf >> hash_hex;
    hf.close();

    // Decode the hex string to bytes (20 bytes for SHA-1).
    if (hash_hex.length() != 40) {
        nimrtc::core::log::Logger::instance().error(
            "Invalid certificate hash length: " + hash_hex);
        return false;
    }
    uint8_t hash_bytes[20] = {};
    for (int i = 0; i < 40; i += 2) {
        unsigned int byte_val;
        if (std::sscanf(hash_hex.c_str() + i, "%02x", &byte_val) != 1) {
            nimrtc::core::log::Logger::instance().error(
                "Cannot parse certificate hash: " + hash_hex);
            return false;
        }
        hash_bytes[i / 2] = static_cast<uint8_t>(byte_val);
    }

    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    QUIC_CERTIFICATE_HASH_STORE hs{};
    std::memcpy(hs.ShaHash, hash_bytes, 20);
    std::memcpy(hs.StoreName, "My", 3);  // Cert:\CurrentUser\My
    cred.CertificateHashStore = &hs;

    const QUIC_STATUS rc = api->ConfigurationLoadCredential(configuration, &cred);
    if (QUIC_FAILED(rc)) {
        nimrtc::core::log::Logger::instance().error(
            "MsQuicConfigurationLoadCredential (HASH_STORE) failed: 0x" +
            std::to_string(rc) +
            " — ensure the cert is in Cert:\\CurrentUser\\My "
            "(run scripts/generate_wtserver_cert.ps1)");
        return false;
    }

    nimrtc::core::log::Logger::instance().info(
        "TLS certificate loaded from Windows store (SHA1 hash: " + hash_hex + ")");
    return true;
}

}  // anonymous namespace

#else  // !_WIN32

namespace {

/**
 * Load the certificate and its private key from PEM files and install them
 * in the msquic server configuration for the TLS 1.3 handshake.
 *
 * Uses QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE, which is the credential
 * shape the OpenSSL / quictls backend (Linux, macOS) understands: the
 * private key and the certificate chain are two separate PEM files.
 *
 * @return true on success, false on failure.
 */
bool load_cert_file_tls_credential(
    const ApiTable* api, HQUIC configuration) noexcept {

    std::string key_path;
    std::string cert_path;
    if (!find_credential_file("wtserver_key.pem", key_path) ||
        !find_credential_file("wtserver_cert.pem", cert_path)) {
        nimrtc::core::log::Logger::instance().error(
            "Cannot find wtserver_key.pem / wtserver_cert.pem in the build "
            "directory (run scripts/generate_wtserver_cert.py first)");
        return false;
    }

    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    QUIC_CERTIFICATE_FILE file{};
    file.PrivateKeyFile = key_path.c_str();
    file.CertificateFile = cert_path.c_str();
    cred.CertificateFile = &file;

    const QUIC_STATUS rc = api->ConfigurationLoadCredential(configuration, &cred);
    if (QUIC_FAILED(rc)) {
        nimrtc::core::log::Logger::instance().error(
            "MsQuicConfigurationLoadCredential (CERTIFICATE_FILE) failed: 0x" +
            std::to_string(rc) +
            " — ensure the key and certificate are a matching pair "
            "(run scripts/generate_wtserver_cert.py first)");
        return false;
    }

    nimrtc::core::log::Logger::instance().info(
        "TLS certificate loaded from PEM files: " + cert_path);
    return true;
}

}  // anonymous namespace

#endif  // _WIN32

namespace {

/**
 * @brief Load the TLS credential into the server configuration.
 *
 * Dispatches to the credential shape the platform's msquic TLS backend
 * accepts: SChannel on Windows (certificate store), OpenSSL / quictls on
 * Linux and macOS (PEM files).
 *
 * @return true on success, false on failure.
 */
bool load_tls_credential(const ApiTable* api, HQUIC configuration) noexcept {
#ifdef _WIN32
    return load_cert_hash_tls_credential(api, configuration);
#else
    return load_cert_file_tls_credential(api, configuration);
#endif
}

}  // anonymous namespace

// ===========================================================================
// HTTP/3 framing helpers
// ===========================================================================
//
// HTTP/3 (RFC 9114) frames the request/response streams with QUIC
// variable-length integers (RFC 9000 §16).  The helpers below are used
// for the frame type and the frame length of every HTTP/3 frame, and for
// the SETTINGS identifiers and values.
// ===========================================================================

namespace {

/**
 * @brief Decode a QUIC variable-length integer (RFC 9000 §16).
 *
 * @param data  In/out cursor; advanced past the integer on success.
 * @param value Receives the decoded value.
 * @return False when @p data does not hold a complete integer.
 */
bool quic_varint_decode(std::span<const std::uint8_t>& data,
                        std::uint64_t& value) noexcept {
    if (data.empty()) return false;
    const std::uint8_t lead = data[0];
    const std::size_t width = std::size_t{1} << (lead >> 6);
    if (data.size() < width) return false;
    std::uint64_t v = lead & 0x3Fu;
    for (std::size_t i = 1; i < width; ++i) {
        v = (v << 8) | data[i];
    }
    value = v;
    data = data.subspan(width);
    return true;
}

/**
 * @brief Append a QUIC variable-length integer (RFC 9000 §16).
 */
void quic_varint_encode(std::vector<std::uint8_t>& out,
                        std::uint64_t value) noexcept {
    if (value < (1ull << 6)) {
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value < (1ull << 14)) {
        out.push_back(static_cast<std::uint8_t>(0x40u | (value >> 8)));
        out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    } else if (value < (1ull << 30)) {
        out.push_back(static_cast<std::uint8_t>(0x80u | (value >> 24)));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xC0u | (value >> 56)));
        for (int shift = 48; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
        }
    }
}

/** @brief HTTP/3 frame types (RFC 9114 §7.2). */
constexpr std::uint8_t kFrameData = 0x00;
constexpr std::uint8_t kFrameHeaders = 0x01;
constexpr std::uint8_t kFrameSettings = 0x04;

/** @brief HTTP/3 setting identifiers. */
constexpr std::uint64_t kSettingQpackMaxTableCapacity = 0x01;      // RFC 9204 §5
constexpr std::uint64_t kSettingQpackBlockedStreams = 0x07;        // RFC 9204 §5
constexpr std::uint64_t kSettingEnableConnectProtocol = 0x08;      // RFC 9220 §3
constexpr std::uint64_t kSettingH3Datagram = 0x33;                 // RFC 9297 §2.1.1
// draft-ietf-webtrans-http3-02 SETTINGS_ENABLE_WEBTRANSPORT.
constexpr std::uint64_t kSettingEnableWebtransportDraft02 = 0x2b603743ull;
// draft-ietf-webtrans-http3-07 SETTINGS_WEBTRANSPORT_MAX_SESSIONS.
constexpr std::uint64_t kSettingWebtransportMaxSessionsDraft07 = 0x2b603742ull;
// IANA HTTP/3 Settings registry: SETTINGS_WEBTRANSPORT_MAX_SESSIONS.
constexpr std::uint64_t kSettingWebtransportMaxSessions = 0xc671706aull;

/** @brief Unidirectional stream types (RFC 9114 §6.2, RFC 9204 §4.3). */
constexpr std::uint8_t kUniControlStream = 0x00;
constexpr std::uint8_t kUniPushStream = 0x01;
constexpr std::uint8_t kUniQpackEncoder = 0x02;
constexpr std::uint8_t kUniQpackDecoder = 0x03;

}  // anonymous namespace

// ===========================================================================
// Http3Handler — HTTP/3 state machine (one instance per connection)
// ===========================================================================

Http3Handler::Http3Handler(Impl* listener_impl) noexcept
    : listener_impl_(listener_impl) {}

bool Http3Handler::on_connect(const ApiTable* api, HQUIC conn) noexcept {
    conn_ = conn;
    return send_settings(api, conn);
}

void Http3Handler::on_peer_stream_started(HQUIC stream,
                                          bool unidirectional) noexcept {
    std::lock_guard<std::mutex> l{stream_mutex_};
    auto& state = stream_states_[stream];
    // Bidirectional peer streams are request streams (RFC 9114 §6.2.1);
    // unidirectional ones carry their type in the first byte.
    state.role = unidirectional ? StreamRole::kUnknown : StreamRole::kRequest;
}

void Http3Handler::on_datagram_state_changed(bool send_enabled,
                                             std::uint16_t max_send_length) noexcept {
    datagram_state_known_ = true;
    datagram_send_enabled_ = send_enabled;
    datagram_max_send_length_ = max_send_length;
}

bool Http3Handler::send_settings(const ApiTable* api, HQUIC conn) noexcept {
    if (conn == nullptr) return false;

    // The HTTP/3 control stream MUST be a unidirectional stream
    // (RFC 9114 §6.2.1).  Opening a bidirectional stream here would
    // collide with the client's request stream.
    HQUIC ctrl_stream = nullptr;
    const QUIC_STATUS rc_open = api->StreamOpen(
        conn,
        QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        &quic_server_stream_callback,
        this,
        &ctrl_stream);
    if (QUIC_FAILED(rc_open) || ctrl_stream == nullptr) {
        nimrtc::core::log::Logger::instance().error(
            "Http3Handler: StreamOpen(control) failed: " +
            std::to_string(rc_open));
        return false;
    }

    const QUIC_STATUS rc_start = api->StreamStart(
        ctrl_stream, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(rc_start)) {
        api->StreamClose(ctrl_stream);
        return false;
    }

    // SETTINGS payload.  Chromium refuses the WebTransport handshake
    // (ERR_METHOD_NOT_SUPPORTED) unless the peer advertises all three
    // of: Extended CONNECT, HTTP Datagrams, and a non-zero WebTransport
    // session limit.  Both the draft-07 and IANA identifiers are sent
    // because Chrome versions in the wild still recognise either one;
    // RFC 9114 §7.2.4 requires unknown settings to be ignored.
    std::vector<std::uint8_t> settings;
    // The server never uses a dynamic table, so the encoder is told to
    // keep the QPACK dynamic table empty (RFC 9204 §5).
    quic_varint_encode(settings, kSettingQpackMaxTableCapacity);
    quic_varint_encode(settings, 0);
    quic_varint_encode(settings, kSettingQpackBlockedStreams);
    quic_varint_encode(settings, 0);
    quic_varint_encode(settings, kSettingEnableConnectProtocol);
    quic_varint_encode(settings, 1);
    quic_varint_encode(settings, kSettingH3Datagram);
    quic_varint_encode(settings, 1);
    quic_varint_encode(settings, kSettingEnableWebtransportDraft02);
    quic_varint_encode(settings, 1);
    quic_varint_encode(settings, kSettingWebtransportMaxSessionsDraft07);
    quic_varint_encode(settings, 1);
    quic_varint_encode(settings, kSettingWebtransportMaxSessions);
    quic_varint_encode(settings, 1);

    // Control stream content: the stream type byte followed by the
    // SETTINGS frame.
    std::vector<std::uint8_t> payload;
    payload.push_back(kUniControlStream);
    payload.push_back(kFrameSettings);
    quic_varint_encode(payload, settings.size());
    payload.insert(payload.end(), settings.begin(), settings.end());

    // msquic reads the payload asynchronously (it is copied into
    // msquic's own send buffer only when the data is flushed into a
    // packet), so both the QUIC_BUFFER descriptor and the bytes it
    // points at must stay alive until the matching
    // QUIC_STREAM_EVENT_SEND_COMPLETE.  OutboundPayload owns both and
    // is released by the stream callback.
    auto* outbound = new OutboundPayload(payload);

    // The control stream stays open for the lifetime of the connection,
    // hence no FIN flag.
    const QUIC_STATUS rc_send = api->StreamSend(
        ctrl_stream, outbound->buffers(), 1, QUIC_SEND_FLAG_NONE, outbound);
    if (QUIC_FAILED(rc_send)) {
        // The send was rejected, so no completion event will arrive and
        // the payload has to be released here.
        release_outbound_payload(outbound);
        api->StreamClose(ctrl_stream);
        return false;
    }

    nimrtc::core::log::Logger::instance().info(
        "Http3Handler: control stream opened, SETTINGS sent");
    return true;
}

bool Http3Handler::on_stream_data(
    HQUIC stream,
    std::span<const std::uint8_t> data,
    bool fin) noexcept {

    StreamRole role = StreamRole::kUnknown;
    std::vector<std::uint8_t> buf;

    {
        std::lock_guard<std::mutex> l{stream_mutex_};
        auto it = stream_states_.find(stream);
        if (it == stream_states_.end()) {
            // Data for a stream we do not know about — ignore it.
            return true;
        }
        auto& state = it->second;
        state.fin_seen = state.fin_seen || fin;
        state.buf.insert(state.buf.end(), data.begin(), data.end());
        role = state.role;
        buf = state.buf;
    }

    // Resolve the stream type of a unidirectional stream (RFC 9114 §6.2).
    if (role == StreamRole::kUnknown) {
        if (buf.empty()) return true;
        const std::uint8_t type = buf.front();
        buf.erase(buf.begin());
        switch (type) {
            case kUniControlStream:
                role = StreamRole::kControl;
                break;
            case kUniQpackEncoder:
                role = StreamRole::kEncoder;
                break;
            case kUniPushStream:
            case kUniQpackDecoder:
            default:
                // Neither is valid coming from a client.
                role = StreamRole::kPush;
                break;
        }
        std::lock_guard<std::mutex> l{stream_mutex_};
        auto it = stream_states_.find(stream);
        if (it != stream_states_.end()) it->second.role = role;
    }

    switch (role) {
        case StreamRole::kRequest: {
            process_request_frames(stream, buf);
            std::lock_guard<std::mutex> l{stream_mutex_};
            auto it = stream_states_.find(stream);
            if (it != stream_states_.end()) it->second.buf = std::move(buf);
            return true;
        }
        case StreamRole::kControl: {
            process_control_frames(buf);
            std::lock_guard<std::mutex> l{stream_mutex_};
            auto it = stream_states_.find(stream);
            if (it != stream_states_.end()) it->second.buf = std::move(buf);
            return true;
        }
        case StreamRole::kEncoder: {
            // The QPACK encoder stream is unframed; the decoder buffers
            // partial instructions itself (RFC 9204 §4.3).
            qpack_.feed_encoder_stream(buf);
            if (qpack_.failed()) {
                nimrtc::core::log::Logger::instance().error(
                    "Http3Handler: malformed QPACK encoder stream");
            }
            std::lock_guard<std::mutex> l{stream_mutex_};
            auto it = stream_states_.find(stream);
            if (it != stream_states_.end()) it->second.buf.clear();
            return true;
        }
        default:
            return true;
    }
}

bool Http3Handler::process_request_frames(
    HQUIC stream, std::vector<std::uint8_t>& buf) noexcept {

    std::span<const std::uint8_t> rem{buf.data(), buf.size()};

    while (!rem.empty()) {
        std::span<const std::uint8_t> cursor = rem;

        std::uint64_t frame_type = 0;
        if (!quic_varint_decode(cursor, frame_type)) break;
        std::uint64_t frame_len = 0;
        if (!quic_varint_decode(cursor, frame_len)) break;
        if (cursor.size() < frame_len) break;  // incomplete frame

        const std::span<const std::uint8_t> payload =
            cursor.first(static_cast<std::size_t>(frame_len));
        rem = cursor.subspan(static_cast<std::size_t>(frame_len));

        switch (frame_type) {
            case kFrameHeaders: {
                bool answered = false;
                {
                    std::lock_guard<std::mutex> l{stream_mutex_};
                    auto it = stream_states_.find(stream);
                    answered = (it != stream_states_.end()) &&
                               it->second.webtransport_sent;
                }
                if (!answered) {
                    parse_headers(stream, payload);
                }
                break;
            }
            case kFrameData:
                // Session payload of an established WebTransport session.
                // The demo server echoes datagrams, not stream data.
                break;
            default:
                break;
        }
    }

    buf.assign(rem.begin(), rem.end());
    return true;
}

bool Http3Handler::process_control_frames(
    std::vector<std::uint8_t>& buf) noexcept {

    std::span<const std::uint8_t> rem{buf.data(), buf.size()};

    while (!rem.empty()) {
        std::span<const std::uint8_t> cursor = rem;

        std::uint64_t frame_type = 0;
        if (!quic_varint_decode(cursor, frame_type)) break;
        std::uint64_t frame_len = 0;
        if (!quic_varint_decode(cursor, frame_len)) break;
        if (cursor.size() < frame_len) break;  // incomplete frame

        const std::span<const std::uint8_t> payload =
            cursor.first(static_cast<std::size_t>(frame_len));
        rem = cursor.subspan(static_cast<std::size_t>(frame_len));

        if (frame_type == kFrameSettings) {
            parse_settings(payload);
        }
        // Everything else on the control stream is informational for us.
    }

    buf.assign(rem.begin(), rem.end());
    return true;
}

bool Http3Handler::parse_settings(
    std::span<const std::uint8_t> payload) noexcept {

    std::span<const std::uint8_t> rem = payload;
    while (!rem.empty()) {
        std::uint64_t id = 0;
        if (!quic_varint_decode(rem, id)) break;
        std::uint64_t value = 0;
        if (!quic_varint_decode(rem, value)) break;

        switch (id) {
            case kSettingQpackMaxTableCapacity:
                // The peer's decoder capacity.  Our encoder never uses a
                // dynamic table, so the value does not constrain us.
                break;
            case kSettingQpackBlockedStreams:
            case kSettingEnableConnectProtocol:
            case kSettingH3Datagram:
            case kSettingEnableWebtransportDraft02:
            case kSettingWebtransportMaxSessionsDraft07:
            case kSettingWebtransportMaxSessions:
            default:
                break;
        }
    }

    settings_received_.store(true, std::memory_order_release);
    return true;
}

bool Http3Handler::parse_headers(
    HQUIC stream, std::span<const std::uint8_t> payload) noexcept {

    std::vector<QpackHeaderField> fields;
    if (!qpack_.decode(payload, fields)) {
        nimrtc::core::log::Logger::instance().error(
            "Http3Handler: QPACK decoding of the HEADERS frame failed");
        return false;
    }

    std::unordered_map<std::string, std::string> headers;
    headers.reserve(fields.size());
    for (const QpackHeaderField& field : fields) {
        headers.emplace(field.name, field.value);
    }

    const auto method = headers.find(":method");
    if (method == headers.end() || method->second != "CONNECT") {
        return false;  // not an Extended CONNECT request
    }
    const auto protocol = headers.find(":protocol");
    if (protocol == headers.end() || protocol->second != "webtransport") {
        return false;  // not a WebTransport request
    }

    // Mark the stream as answered before the response is sent so that a
    // second HEADERS frame cannot start a second session.
    {
        std::lock_guard<std::mutex> l{stream_mutex_};
        auto& state = stream_states_[stream];
        if (state.webtransport_sent) return false;
        state.webtransport_sent = true;
    }

    nimrtc::core::log::Logger::instance().info(
        "Http3Handler: WebTransport CONNECT request received");

    // Answer the request with ":status: 200" (RFC 9297 §3).
    if (!send_webtransport_response(&ApiTable::instance(), conn_, stream)) {
        nimrtc::core::log::Logger::instance().error(
            "Http3Handler: failed to send the 200 response");
        return false;
    }

    if (listener_impl_ != nullptr) {
        listener_impl_->on_webtransport_request(
            conn_, stream, headers,
            datagram_send_enabled_, datagram_max_send_length_,
            datagram_state_known_);
    }
    return true;
}

bool Http3Handler::send_webtransport_response(
    const ApiTable* api, HQUIC conn, HQUIC stream) noexcept {

    std::vector<QpackHeaderField> fields;
    fields.push_back(QpackHeaderField{":status", "200"});
    fields.push_back(QpackHeaderField{"sec-webtransport-version", "1"});

    std::vector<std::uint8_t> block;
    if (!encode_field_section(fields, block)) return false;

    // The CONNECT stream carries the session data, so the response must
    // not close it with a FIN.
    return send_frame(api, conn, stream, kFrameHeaders, block, false);
}

bool Http3Handler::send_frame(const ApiTable* api, HQUIC, HQUIC stream,
                               std::uint8_t frame_type,
                               std::span<const std::uint8_t> payload,
                               bool fin) noexcept {
    if (stream == nullptr) return false;

    std::vector<std::uint8_t> frame;
    frame.push_back(frame_type);
    quic_varint_encode(frame, payload.size());
    frame.insert(frame.end(), payload.begin(), payload.end());

    // The frame bytes are read by msquic asynchronously, so both the
    // QUIC_BUFFER descriptor and the bytes it points at have to stay
    // alive until QUIC_STREAM_EVENT_SEND_COMPLETE (see
    // OutboundPayload for the details).
    auto* outbound = new OutboundPayload(frame);

    const QUIC_STATUS rc = api->StreamSend(
        stream, outbound->buffers(), 1,
        fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE, outbound);
    if (QUIC_FAILED(rc)) {
        // No completion event will arrive for a rejected send.
        release_outbound_payload(outbound);
        return false;
    }
    return true;
}

void Http3Handler::on_stream_closed(HQUIC stream) noexcept {
    {
        std::lock_guard<std::mutex> l{stream_mutex_};
        stream_states_.erase(stream);
    }

    // The application owns every stream handle it receives from msquic
    // and has to close it exactly once.  Not closing the request stream
    // (the WebTransport CONNECT stream) keeps a reference to the QUIC
    // connection alive, so the connection is never destroyed -- and the
    // registration's rundown never completes, which makes
    // MsQuicRegistrationClose() block forever at shutdown.
    ApiTable::instance().StreamClose(stream);
}

// ===========================================================================
// QUIC callbacks — server-side
// ===========================================================================

// Per-connection HTTP/3 handler context stored in the connection handle.
struct ServerConnCtx {
    // The HTTP/3 handler is per connection: the QPACK dynamic table and
    // the stream map are connection-wide resources (RFC 9204 §3.2).
    std::unique_ptr<Http3Handler> handler;
    HQUIC conn;                    // accepted connection handle
    std::atomic<bool> http3_ready{false};
};

QUIC_STATUS QUIC_API quic_server_conn_callback(
    HQUIC Connection,
    void* Context,
    QUIC_CONNECTION_EVENT* Event) noexcept {

    auto* ctx = static_cast<ServerConnCtx*>(Context);
    (void)Connection;

    switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            nimrtc::core::log::Logger::instance().info(
                "QUIC connection established (handshake complete)");
            if (ctx && ctx->handler) {
                // Use the ApiTable singleton for the server-side connection.
                ctx->handler->on_connect(
                    &ApiTable::instance(), Connection);
                ctx->http3_ready.store(true, std::memory_order_release);
            }
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            nimrtc::core::log::Logger::instance().error(
                "QUIC connection shut down by transport: status=0x" +
                std::to_string(Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status) +
                " error_code=0x" +
                std::to_string(
                    Event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode));
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            nimrtc::core::log::Logger::instance().warn(
                "QUIC connection shut down by peer: error_code=0x" +
                std::to_string(
                    Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode));
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            if (ctx) delete ctx;
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
            // The peer advertised QUIC datagram support.  It is learned
            // during the handshake, i.e. before the WebTransport session
            // object exists, so the state is remembered here.
            if (ctx && ctx->handler) {
                ctx->handler->on_datagram_state_changed(
                    Event->DATAGRAM_STATE_CHANGED.SendEnabled != 0,
                    Event->DATAGRAM_STATE_CHANGED.MaxSendLength);
            }
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            HQUIC stream = Event->PEER_STREAM_STARTED.Stream;
            const bool unidirectional =
                (Event->PEER_STREAM_STARTED.Flags &
                 QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) != 0;
            if (ctx && ctx->handler) {
                ApiTable::instance().SetContext(stream, ctx->handler.get());
                ApiTable::instance().SetCallbackHandler(
                    stream,
                    reinterpret_cast<void*>(&quic_server_stream_callback),
                    ctx->handler.get());
                ctx->handler->on_peer_stream_started(stream, unidirectional);
            }
            return QUIC_STATUS_SUCCESS;
        }

        default:
            return QUIC_STATUS_SUCCESS;
    }
}

QUIC_STATUS QUIC_API quic_server_stream_callback(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) noexcept {

    // The payload of a completed send is released here.  This is done
    // before the handler is touched, because the completion event may
    // arrive after the connection (and with it the HTTP/3 handler) is
    // gone.
    if (Event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        release_outbound_payload(Event->SEND_COMPLETE.ClientContext);
        return QUIC_STATUS_SUCCESS;
    }

    auto* handler = static_cast<Http3Handler*>(Context);
    if (!handler) return QUIC_STATUS_INVALID_PARAMETER;

    switch (Event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            for (std::uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER* buf = &Event->RECEIVE.Buffers[i];
                handler->on_stream_data(
                    Stream,
                    std::span<const std::uint8_t>{
                        static_cast<const std::uint8_t*>(buf->Buffer),
                        static_cast<std::size_t>(buf->Length)},
                    (Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0);
            }
            return QUIC_STATUS_SUCCESS;
        }

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            handler->on_stream_closed(Stream);
            return QUIC_STATUS_SUCCESS;

        case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
            return QUIC_STATUS_SUCCESS;

        default:
            return QUIC_STATUS_SUCCESS;
    }
}

QUIC_STATUS QUIC_API quic_listener_callback(
    HQUIC /*Listener*/,
    void* Context,
    QUIC_LISTENER_EVENT* Event) noexcept {

    switch (Event->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
            HQUIC conn = Event->NEW_CONNECTION.Connection;

            // Context is Impl* (set in Impl constructor).
            auto* impl = static_cast<Impl*>(Context);
            if (!impl) return QUIC_STATUS_INVALID_PARAMETER;

            // Create the per-connection HTTP/3 handler and its context.
            auto* ctx = new ServerConnCtx{
                std::make_unique<Http3Handler>(impl), conn, false};

            nimrtc::core::log::Logger::instance().info(
                "QUIC connection accepted by listener");

            // Register the connection so it can be looked up for session creation.
            {
                std::lock_guard<std::mutex> l{impl->conn_mutex_};
                impl->connections_.push_back(conn);
            }

            // Set the connection context and callback.
            ApiTable::instance().SetContext(conn, ctx);
            ApiTable::instance().SetCallbackHandler(
                conn,
                reinterpret_cast<void*>(&quic_server_conn_callback),
                ctx);

            // Attach the server configuration to the accepted connection.
            //
            // msquic contract (docs/api/ListenerStart.md): "The server
            // application must call ConnectionSetConfiguration on the
            // connection, either before returning from the
            // QUIC_LISTENER_EVENT_NEW_CONNECTION callback, or on its own
            // after returning from the callback, otherwise the connection
            // handshake will stall and timeout."
            //
            // Without this call the listener still receives and ACKs the
            // client's Initial packet, but msquic never starts the TLS 1.3
            // handshake, so the client never sees a ServerHello and the
            // WebTransport session never comes up.
            const QUIC_STATUS rc_cfg =
                ApiTable::instance().ConnectionSetConfiguration(
                    conn, impl->configuration());
            if (QUIC_FAILED(rc_cfg)) {
                nimrtc::core::log::Logger::instance().error(
                    "MsQuicConnectionSetConfiguration failed on accepted "
                    "connection: 0x" + std::to_string(rc_cfg));
                ApiTable::instance().ConnectionClose(conn);
                delete ctx;
                return QUIC_STATUS_SUCCESS;
            }

            return QUIC_STATUS_SUCCESS;
        }

        case QUIC_LISTENER_EVENT_STOP_COMPLETE:
            return QUIC_STATUS_SUCCESS;

        default:
            return QUIC_STATUS_SUCCESS;
    }
}

// ===========================================================================
// Impl — msquic listener
// ===========================================================================

Impl::Impl(
    uint16_t port,
    const ApiTable* api,
    const WebTransportConfig& cfg,
    std::function<void(std::unique_ptr<IWebTransportSession>)> cb)
    : api_(api), cfg_(cfg), on_session_cb_(std::move(cb)) {

    if (!api_->ok()) {
        throw std::runtime_error(
            "WebTransport listener: msquic API not available");
    }

    // ---- Registration ----
    {
        QUIC_REGISTRATION_CONFIG reg_cfg{};
        reg_cfg.AppName = "nimrtc-webtransport-server";
        reg_cfg.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;
        const QUIC_STATUS rc = api_->RegistrationOpen(&reg_cfg, &registration_);
        if (QUIC_FAILED(rc)) {
            throw std::runtime_error(
                std::string("MsQuicRegistrationOpen failed: ") +
                std::to_string(rc));
        }
    }

    // ---- Configuration ----
    {
        // HTTP/3 ALPN as required by RFC 9114 / RFC 9298.
        // Chrome's WebTransport JS API sends QUIC ClientHello with ALPN="h3".
        // The configuration must advertise the same ALPN or msquic will reject
        // the handshake at ConfigOpen time (before any connection is accepted).
        static const QUIC_BUFFER kAlpn = {
            2,  // sizeof("h3") - 1
            (uint8_t*)"h3"
        };

        QUIC_SETTINGS settings{};
        std::memset(&settings, 0, sizeof(settings));

        if (cfg.max_idle_timeout_ms > 0) {
            settings.IdleTimeoutMs = cfg.max_idle_timeout_ms;
            settings.IsSet.IdleTimeoutMs = 1;
        }
        settings.IsSet.DatagramReceiveEnabled = 1;
        settings.DatagramReceiveEnabled = 1;
        settings.IsSet.PeerBidiStreamCount = 1;
        settings.IsSet.PeerUnidiStreamCount = 1;
        settings.PeerBidiStreamCount  = 10;
        settings.PeerUnidiStreamCount = 10;

        const QUIC_STATUS rc = api_->ConfigurationOpen(
            registration_, &kAlpn, 1,
            &settings, sizeof(settings),
            nullptr,
            &configuration_);
        if (QUIC_FAILED(rc)) {
            api_->RegistrationClose(registration_);
            throw std::runtime_error(
                std::string("MsQuicConfigurationOpen failed: ") +
                std::to_string(rc));
        }

        // ---- TLS credentials ----
        // QUIC (RFC 9001) requires TLS 1.3.  Load the certificate in the
        // shape the platform's msquic TLS backend accepts: the Windows
        // certificate store on Windows (SChannel), PEM files on Linux and
        // macOS (OpenSSL / quictls).
        // The cert must have been installed by scripts/generate_wtserver_cert.ps1
        // (Windows) or scripts/generate_wtserver_cert.py (Linux/macOS).
        // Chrome is launched with --ignore-certificate-errors so it accepts
        // the self-signed cert without a trusted CA (safe for test harness only).
        if (!load_tls_credential(api_, configuration_)) {
            api_->ConfigurationClose(configuration_);
            api_->RegistrationClose(registration_);
            throw std::runtime_error(
                "WebTransport listener: failed to load TLS credentials");
        }

    }    // ---- Listener ----
    {
        HQUIC listener_raw = nullptr;
        // Pass `this` (Impl*) as the listener callback context.
        // The listener callback casts it back and uses it to register connections.
        const QUIC_STATUS rc_open = api_->ListenerOpen(
            registration_,
            quic_listener_callback,
            this,  // Impl* — used as void* context
            &listener_raw);
        if (QUIC_FAILED(rc_open) || listener_raw == nullptr) {
            api_->ConfigurationClose(configuration_);
            api_->RegistrationClose(registration_);
            throw std::runtime_error(
                std::string("MsQuicListenerOpen failed: ") +
                std::to_string(rc_open));
        }
        listener_ = listener_raw;

        // Start the listener on the given port.
        // WebTransport uses HTTP/3 (RFC 9114) as its transport protocol.
        // The ALPN "h3" (RFC 9114) is the standard HTTP/3 ALPN.
        QUIC_ADDR addr{};
#ifdef _WIN32
        // The Windows datapath binds the wildcard address for an
        // unspecified family (dual-stack IPv6 socket).
        QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
#else
        // The POSIX datapath always creates an AF_INET6 socket and binds
        // the wildcard address, so the family has to be INET6 there — an
        // unspecified family is rejected by bind().  The socket is
        // dual-stack (IPV6_V6ONLY is cleared), so IPv4 clients are served
        // by the same listener.
        QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_INET6);
#endif
        QuicAddrSetPort(&addr, port);

        static const QUIC_BUFFER kAlpn = {
            2,  // sizeof("h3") - 1
            (uint8_t*)"h3"
        };
        const QUIC_STATUS rc_start = api_->ListenerStart(
            listener_,
            &kAlpn,
            1,
            &addr);
        if (QUIC_FAILED(rc_start)) {
            api_->ListenerClose(listener_);
            api_->ConfigurationClose(configuration_);
            api_->RegistrationClose(registration_);
            throw std::runtime_error(
                std::string("MsQuicListenerStart failed: ") +
                std::to_string(rc_start) + " on port " + std::to_string(port));
        }

        running_.store(true, std::memory_order_release);
        nimrtc::core::log::Logger::instance().info(
            std::string("WebTransport listener started on UDP port ") +
            std::to_string(port));
    }
}

Impl::~Impl() {
    stop();
}

void Impl::on_webtransport_request(
    HQUIC conn,
    [[maybe_unused]] HQUIC stream,
    [[maybe_unused]] const std::unordered_map<std::string, std::string>& headers,
    bool datagram_send_enabled,
    std::uint16_t datagram_max_send_length,
    bool datagram_state_known) noexcept {

    // When this is called, the HTTP/3 200 response has already been queued
    // by Http3Handler::send_webtransport_response().
    // Chrome's WebTransport API will now treat the QUIC connection as
    // a connected WebTransport session.
    //
    // The session wraps the QUIC connection the request arrived on.
    if (conn == nullptr) {
        nimrtc::core::log::Logger::instance().error(
            "WebTransport request: no connection handle available");
        return;
    }

    // Create a server-side WebTransportMsQuicSession.
    // The session is already connected (QUIC handshake was done by the time
    // the CONNECTED event fired on this connection).
    std::unique_ptr<WebTransportMsQuicSession> wt_session;
    try {
        // The WebTransportMsQuicSession constructor takes the connection
        // handle and transitions it to kConnected state.
        // For server-side, we use the server-side Impl constructor.
        wt_session = std::make_unique<WebTransportMsQuicSession>(
            conn, registration_, configuration_,
            api_, cfg_);
    } catch (const std::exception& e) {
        nimrtc::core::log::Logger::instance().error(
            std::string("WebTransport session creation failed: ") + e.what());
        return;
    }

    // The peer advertised QUIC datagram support during the handshake,
    // i.e. before this session object existed.  Seed the session with
    // that state so datagram sending works right away.
    wt_session->apply_datagram_state(datagram_send_enabled,
                                     datagram_max_send_length,
                                     datagram_state_known);

    std::unique_ptr<IWebTransportSession> session = std::move(wt_session);

    nimrtc::core::log::Logger::instance().info(
        "WebTransport session created and delivered");

    // Deliver the session to the user callback.
    if (on_session_cb_) {
        on_session_cb_(std::move(session));
    }
}

void Impl::stop() noexcept {
    if (stopped_.exchange(true)) return;
    running_.store(false, std::memory_order_release);

    if (listener_ != nullptr) {
        api_->ListenerClose(listener_);
        listener_ = nullptr;
    }

    // Close the connections that are still open.  The list is taken
    // under the lock but the handles are closed outside of it: msquic
    // delivers the shutdown events on its own worker threads and those
    // callbacks take the same lock.
    //
    // The listener owns the connection handles it accepted (the
    // server-side session only borrows them), so each handle is closed
    // exactly once, here.
    std::vector<HQUIC> conns;
    {
        std::lock_guard<std::mutex> l{conn_mutex_};
        conns.swap(connections_);
    }
    for (HQUIC conn : conns) {
        if (conn != nullptr) {
            api_->ConnectionClose(conn);
        }
    }

    if (configuration_ != nullptr) {
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
    }

    if (registration_ != nullptr) {
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
    }

    nimrtc::core::log::Logger::instance().info(
        "WebTransport listener stopped");
}

// ===========================================================================
// Listener — RAII handle
// ===========================================================================

Listener::Listener(
    uint16_t port,
    const WebTransportConfig& cfg,
    std::function<void(std::unique_ptr<IWebTransportSession>)> cb)
    : impl_(std::make_unique<Impl>(
          port, &ApiTable::instance(), cfg, std::move(cb))) {}

Listener::~Listener() = default;
Listener::Listener(Listener&&) noexcept = default;
Listener& Listener::operator=(Listener&&) noexcept = default;

// ===========================================================================
// create_msquic_webtransport_server — public factory function
// ===========================================================================

namespace {

/**
 * @brief msquic-backed IWebTransportServer implementation.
 *
 * Wraps a detail::Listener and exposes the IWebTransportServer interface.
 * Each call to start() creates a new listener on the given port.
 */
class MsQuicWebTransportServer final : public IWebTransportServer {
public:
    MsQuicWebTransportServer(const WebTransportConfig& cfg,
                              OnWebTransportSessionCb cb)
        : cfg_(cfg), on_session_cb_(std::move(cb)) {}

    bool start(uint16_t port) override {
        if (listener_) return false;  // already running
        try {
            listener_ = std::make_unique<detail::Listener>(
                port, cfg_,
                [this](std::unique_ptr<IWebTransportSession> session) {
                    if (on_session_cb_) {
                        on_session_cb_(std::move(session));
                    }
                });
            return listener_->running();
        } catch (const std::exception& e) {
            nimrtc::core::log::Logger::instance().error(
                std::string("MsQuicWebTransportServer::start failed: ") + e.what());
            return false;
        }
    }

    void stop() noexcept override {
        if (listener_) {
            listener_->stop();
            listener_.reset();
        }
    }

    void set_on_session(OnWebTransportSessionCb cb) noexcept override {
        on_session_cb_ = std::move(cb);
    }

    [[nodiscard]] bool running() const noexcept override {
        return listener_ && listener_->running();
    }

private:
    WebTransportConfig cfg_;
    OnWebTransportSessionCb on_session_cb_;
    std::unique_ptr<detail::Listener> listener_;
};

}  // anonymous namespace

}  // namespace nimrtc::web_transport::detail

// Public API.
namespace nimrtc::web_transport {

[[nodiscard]] std::unique_ptr<IWebTransportServer>
create_msquic_webtransport_server(
    const WebTransportConfig& cfg) noexcept(false) {
    return std::make_unique<detail::MsQuicWebTransportServer>(
        cfg, OnWebTransportSessionCb{});
}

}  // namespace nimrtc::web_transport
