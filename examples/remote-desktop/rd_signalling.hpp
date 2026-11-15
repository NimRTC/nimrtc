/**
 * @file examples/remote-desktop/rd_signalling.hpp
 * @brief The signalling channel the remote-desktop examples speak.
 *
 * ## The dialect
 *
 * The device and the viewer exchange **newline-delimited JSON** on
 * stdin / stdout, using exactly the message shapes the reference
 * signalling service relays:
 *
 * ```
 *   {"type":"offer",     "sdp":"..."}
 *   {"type":"answer",    "sdp":"..."}
 *   {"type":"candidate", "candidate":"a=candidate:...", "sdpMLineIndex":0, "sdpMid":"0"}
 *   {"type":"bye"}
 *   {"type":"ice_restart"}
 * ```
 *
 * The dialect is deliberately the same as `demo-p2p`'s, so the existing
 * `examples/remote-desktop/client.py` bridges these binaries to the
 * reference WebSocket service **without a change** — the client is a
 * transparent pipe between the WebSocket and the binary's stdout /
 * stdin.
 *
 * ## Why the JSON is hand-rolled
 *
 * The examples link no JSON library: the *vendored* `nlohmann_json` is
 * a dependency of the library targets, not of the examples, and pulling
 * it into an example would make the example's build depend on a
 * third-party library for four message shapes.  The parser below is
 * therefore deliberately *narrow*: it handles the flat objects the
 * dialect uses and nothing else.
 *
 * @see examples/remote-desktop/client.py
 * @see docs/guides/remote-desktop.md
 */

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <poll.h>
#endif

namespace rd_sig {

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

/** Escape a string for a JSON string body (without the quotes). */
inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\r': out += "\\r";  break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

/** Unescape a JSON string value body. */
inline void json_unescape(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out.push_back(c);
            ++i;
            continue;
        }
        const char esc = s[i + 1];
        switch (esc) {
            case '\\': out.push_back('\\'); i += 2; break;
            case '"':  out.push_back('"');  i += 2; break;
            case '/':  out.push_back('/');  i += 2; break;
            case 'b':  out.push_back('\b'); i += 2; break;
            case 'f':  out.push_back('\f'); i += 2; break;
            case 'n':  out.push_back('\n'); i += 2; break;
            case 'r':  out.push_back('\r'); i += 2; break;
            case 't':  out.push_back('\t'); i += 2; break;
            case 'u': {
                if (i + 5 < s.size()) {
                    const auto hex = s.substr(i + 2, 4);
                    unsigned cp = 0;
                    bool ok = true;
                    for (char h : hex) {
                        cp <<= 4;
                        if      (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else { ok = false; break; }
                    }
                    if (ok) {
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        i += 6;
                        break;
                    }
                }
                out.push_back('\\');
                ++i;
                break;
            }
            default:
                out.push_back('\\');
                out.push_back(esc);
                i += 2;
                break;
        }
    }
    s = std::move(out);
}

/**
 * @brief Pull `"key":"value"` out of a flat JSON object.
 *
 * Only the flat shapes the dialect uses are handled: no nesting, and
 * the value itself is not escaped (the SDP and the candidate lines are
 * relayed verbatim by the service, and the SDP's own escapes are
 * handled by `json_unescape`).
 */
inline bool json_get_string(std::string_view json, std::string_view key,
                             std::string* out) {
    std::string needle;
    needle.reserve(key.size() + 3);
    needle.append("\"").append(key).append("\":");
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    // The value may contain *escaped* quotes (`\"`), so the scan has to
    // skip them: a naive `find('"')` would stop at the first escaped
    // quote and truncate the SDP.
    std::size_t end = std::string_view::npos;
    for (std::size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '\\') {
            ++i;   // the escaped character
            continue;
        }
        if (json[i] == '"') {
            end = i;
            break;
        }
    }
    if (end == std::string_view::npos) return false;
    out->assign(json.substr(pos, end - pos));
    json_unescape(*out);
    return true;
}

// ---------------------------------------------------------------------------
// Emitting
// ---------------------------------------------------------------------------

/** Emit one JSON line on stdout (flushed). */
inline void emit_json(std::string_view type, std::string_view body) {
    std::fprintf(stdout, "{\"type\":\"%.*s\",%.*s}\n",
                 static_cast<int>(type.size()), type.data(),
                 static_cast<int>(body.size()), body.data());
    std::fflush(stdout);
}

/** Emit an `offer` or an `answer` message. */
inline void emit_sdp(std::string_view type, std::string_view sdp) {
    // The body is the *named* field: `{"type":"offer","sdp":"..."}`.
    // (The key is part of the dialect — the peer looks the field up by
    // name, so an anonymous string body would be an unreadable message.)
    emit_json(type, "\"sdp\":\"" + json_escape(sdp) + "\"");
}

/** Emit a `candidate` message. */
inline void emit_candidate(std::string_view candidate, int mline_index = 0,
                            std::string_view mid = "0") {
    std::string body = "\"candidate\":\"";
    body += json_escape(candidate);
    body += "\",\"sdpMLineIndex\":";
    body += std::to_string(mline_index);
    body += ",\"sdpMid\":\"";
    body += json_escape(mid);
    body += "\"";
    emit_json("candidate", body);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

/**
 * @brief Read one line from stdin, waiting at most `timeout_ms`.
 *
 * @return true when a line was read; false on a timeout or on EOF.
 *
 * ## Why the reader owns its own buffer
 *
 * The reader must not mix a *buffered* stdio read (`fgetc`) with an
 * *unbuffered* peek (`PeekNamedPipe`).  The two disagree about where
 * the data is: `fgetc` fills stdio's own 4 KiB buffer, and
 * `PeekNamedPipe` only reports what is still in the **kernel** pipe
 * buffer.  So a line that `fgetc` already pulled into stdio's buffer
 * is invisible to the next peek — and a reader that waits for the peek
 * before reading waits forever for a line it already has.  The symptom
 * is a session that connects, never sees the peer's SDP, and times
 * out with no error.
 *
 * So on Windows the reader uses `ReadFile` on the pipe handle
 * directly and keeps **its own** line buffer.  There is exactly one
 * reader of the handle and one buffer, so there is nothing to
 * disagree.  On the other platforms `poll()` + `fgetc()` is used, and
 * the same one-reader-one-buffer rule holds.
 *
 * The console path (Windows) reads synchronously, because a console's
 * stdin has no non-blocking read and the examples are driven by a pipe
 * in practice.
 */
namespace detail {

/// The reader's own buffer: the bytes read from the handle that have
/// not been consumed by a line yet.
inline std::string& pending_bytes() {
    static std::string pending;
    return pending;
}

/// Pull one '\n'-terminated line out of `pending_bytes()`.
///
/// @return true when a line was found (and `line` was filled).
inline bool take_line(std::string* line) {
    std::string& pending = pending_bytes();
    const auto nl = pending.find('\n');
    if (nl == std::string::npos) return false;
    *line = pending.substr(0, nl);
    pending.erase(0, nl + 1);
    while (!line->empty() &&
           (line->back() == '\r' || line->back() == '\n')) {
        line->pop_back();
    }
    return true;
}

} // namespace detail

inline bool read_line(std::string* line, int timeout_ms) {
    line->clear();

    // A line already in the buffer is returned immediately.
    if (detail::take_line(line)) return true;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

#ifdef _WIN32
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    const bool is_pipe = GetFileType(h) != FILE_TYPE_CHAR;
    if (is_pipe) {
        char chunk[4096];
        while (true) {
            DWORD avail = 0;
            if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) &&
                avail > 0) {
                DWORD read = 0;
                if (!ReadFile(h, chunk, sizeof(chunk), &read, nullptr)) {
                    return false;
                }
                if (read == 0) return false;
                detail::pending_bytes().append(chunk, read);
                if (detail::take_line(line)) return true;
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            Sleep(10);
        }
    }
    while (true) {
        const int c = fgetc(stdin);
        if (c == EOF) return false;
        if (c == '\n') {
            while (!line->empty() &&
                   (line->back() == '\r' || line->back() == '\n')) {
                line->pop_back();
            }
            return true;
        }
        line->push_back(static_cast<char>(c));
    }
#else
    struct pollfd pfd = { fileno(stdin), POLLIN, 0 };
    const int r = ::poll(&pfd, 1, timeout_ms);
    if (r <= 0) return false;
    int c = fgetc(stdin);
    if (c == EOF) return false;
    while (c != EOF && c != '\n') {
        line->push_back(static_cast<char>(c));
        c = fgetc(stdin);
    }
    while (!line->empty() &&
           (line->back() == '\r' || line->back() == '\n')) {
        line->pop_back();
    }
    return true;
#endif
}

} // namespace rd_sig
