#pragma once
// sdk_signalling.hpp -- the sample's whole signalling client: newline-
// delimited JSON on stdin/stdout, the same dialect the reference signalling
// service's client.py bridges.  The service (signalling.py) is reused as-is;
// this header is the only C++ the host writes for signalling.

#include <cstdio>
#include <string>
#include <string_view>

namespace sdk {

// Emit one message as a JSON object line:
//   {"type":"offer|answer|candidate","sdp|candidate":"<escaped>"}
inline void emit(std::FILE* out, const char* type, const char* field, std::string_view v) {
    std::string e;
    for (char c : v) {
        if (c == '"' || c == '\\' || c == '\n' || c == '\r') e += '\\';
        if (c == '\n') e += 'n';
        else if (c == '\r') e += 'r';
        else if (c != '"' && c != '\\') e += c;
    }
    std::fprintf(out, "{\"type\":\"%s\",\"%s\":\"%s\"}\n", type, field, e.c_str());
    std::fflush(out);
}

// Blocking read of one JSON line; trailing \r / \n stripped.
inline bool read_line(std::FILE* in, std::string& line) {
    line.clear();
    for (int c = std::fgetc(in); c != EOF && c != '\n'; c = std::fgetc(in))
        if (c != '\r') line.push_back(static_cast<char>(c));
    return !line.empty() || !std::feof(in);
}

// Pull the unescaped string value of `key` from one JSON object line.
inline bool get_string(std::string_view line, const char* key, std::string& out) {
    const std::string n = std::string("\"") + key + "\":\"";
    const std::size_t p = line.find(n);
    if (p == std::string_view::npos) return false;
    out.clear();
    for (std::size_t i = p + n.size(); i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') return true;
        if (c == '\\' && i + 1 < line.size()) {
            const char e = line[++i];
            out += e == 'n' ? '\n' : e == 'r' ? '\r' : e;
        } else out += c;
    }
    return false;
}

}  // namespace sdk
