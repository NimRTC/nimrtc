/**
 * @file fuzz_sdp.cpp
 * @brief Fuzz target for the SDP parser (and the SDP serialiser).
 *
 * The SDP description is the first thing a peer sends us and it is pure
 * text, so it is the input a remote party controls most completely.
 * The parser turns it into a `SessionDescription`; the serialiser turns
 * that back into text.  Both are driven here, because the interesting
 * failures are not only "the parser mis-reads a field" but also "the
 * parser accepted something the serialiser then writes back differently",
 * which is how a description drifts out of sync with itself.
 *
 * The target also drives the *plugin* adapter (`plugins::ISDP`), which
 * is what the engine actually calls — the adapter owns a buffer whose
 * lifetime the parsed views borrow, so a mistake there is exactly the
 * kind of bug this target is meant to find.
 */
#include "fuzz_targets.hpp"

#include <string>
#include <string_view>

#include <nimrtc/plugins/sdp.hpp>
#include <nimrtc/sdp/sdp_plugin.hpp>
#include <nimrtc/sdp/session_description.hpp>

namespace nimrtc::fuzz {

void run_sdp(const std::uint8_t* data, std::size_t size) {    // The parser takes a string_view, and the bytes may contain NULs —
    // a string_view over them is fine (it is a (pointer, length) pair,
    // not a C string), and the parser must cope with a NUL inside a
    // line.
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    // A fresh parser per call: the parser is stateless, but constructing
    // it inside the entry point keeps the target independent of any
    // state a previous run left behind.
    nimrtc::sdp::Parser parser;
    auto parsed = parser.parse(text);
    if (!parsed.ok()) return;

    // Round-trip: serialise what we parsed and parse it again.  A
    // serialiser that emits text the parser cannot read back is a real
    // defect (the engine writes the answer with it).
    nimrtc::sdp::Munger munger;
    auto text_out = munger.to_sdp(parsed.value());
    if (!text_out.ok()) return;

    nimrtc::sdp::Parser reparser;
    (void)reparser.parse(text_out.value());

    // The plugin adapter: the engine's own path.  Its views borrow from
    // the input buffer, so the buffer has to outlive them — the
    // `owned` string below does exactly that.
    nimrtc::sdp::PluginAdapter adapter;
    const std::string owned(text);
    (void)adapter.parse(owned, [](plugins::Status, std::string_view) {});
}

} // namespace nimrtc::fuzz
