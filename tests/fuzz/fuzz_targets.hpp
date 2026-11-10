/**
 * @file fuzz_targets.hpp
 * @brief The untrusted-input entry points the fuzzers drive.
 *
 * ## Why these three
 *
 * A WebRTC endpoint parses bytes it did not produce: the peer's SDP
 * description, the peer's RTP / RTCP packets, and the video payload
 * inside those packets.  Every one of those parsers is a place where a
 * malformed input becomes a bug in *our* code — an out-of-bounds read,
 * an unchecked length, an integer that wraps — and every one of them is
 * reachable by a remote peer.  Those three are the entries NimRTC owns
 * and therefore the entries NimRTC has to fuzz.
 *
 * The DTLS and STUN parsers are deliberately **not** here: they belong
 * to the vendored wolfSSL and libjuice, which carry their own upstream
 * fuzzing.  Re-fuzzing a vendored library from here would test the
 * vendored code, not ours.
 *
 * ## How the entry points are used
 *
 * Each `run_*()` function takes one arbitrary byte range and drives one
 * parser over it.  The same function is used by two callers:
 *
 *   - the **libFuzzer** targets (`NIMRTC_FUZZING` defined), which call
 *     it from `LLVMFuzzerTestOneInput`;
 *   - the **corpus replay** driver (`fuzz_replay.cpp`), which runs on
 *     every platform in the normal test suite, feeds every committed
 *     seed plus a deterministic set of mutations through it, and fails
 *     the build if any of them crashes the process.
 *
 * The replay driver is what makes the fuzzers useful in a project that
 * does not run libFuzzer on every commit: the seeds are regression
 * inputs, so a crash found by libFuzzer stays a crash once the seed is
 * committed.
 *
 * The contract every `run_*()` has to honour:
 *
 *   - it must not crash, hang or leak on ANY input;
 *   - it must not depend on global state that another run changed
 *     (each call constructs its own parser);
 *   - it must not write to stdout in a hot loop (the replay driver
 *     prints only a summary).
 */
#ifndef NIMRTC_TESTS_FUZZ_TARGETS_HPP
#define NIMRTC_TESTS_FUZZ_TARGETS_HPP

#include <cstddef>
#include <cstdint>

namespace nimrtc::fuzz {

/** Drive the SDP parser over an arbitrary byte range.
 *
 *  The input is treated as UTF-8 text (SDP is a text format).  A byte
 *  range that is not valid UTF-8 is still fed to the parser: the parser
 *  is responsible for rejecting it without reading out of bounds.
 *
 *  Not `noexcept`: the entry points allocate (a parse result, a
 *  serialised copy), and a `std::bad_alloc` must propagate rather
 *  than terminate the process.  The contract is "must not crash on a
 *  malformed *input*", not "must not fail at all". */
void run_sdp(const std::uint8_t* data, std::size_t size);

/** Drive the RTP / RTCP parsers over an arbitrary byte range.
 *
 *  The same bytes are offered to:
 *
 *   - `rtp::Parser::parse()` (RFC 3550 packet, RFC 5285 extensions);
 *   - `rtp::demux_rtcp()` (RFC 3550 §6.1 compound walk);
 *   - the per-message RTCP parsers (SR / RR / NACK / transport-cc),
 *     each of which re-parses the compound from its own offset.
 *
 *  The RTCP compound walk and the RTP packet parse have different
 *  headers, so one byte range legitimately exercises both "reject as
 *  RTP" and "reject as RTCP" paths.
 *
 *  Not `noexcept` — see `run_sdp()`. */
void run_rtp(const std::uint8_t* data, std::size_t size);

/** Drive the video payload parsers over an arbitrary byte range.
 *
 *  The bytes are offered to the H.264 (RFC 6184) payload parser, to the
 *  VP8 and VP9 payload parsers, and — after a synthetic RTP header is
 *  prepended — to the H.264 FU-A depacketizer, which is the piece that
 *  reassembles a fragmented NAL unit out of untrusted fragments.
 *
 *  Not `noexcept` — see `run_sdp()`. */
void run_video_payload(const std::uint8_t* data, std::size_t size);

} // namespace nimrtc::fuzz

#endif // NIMRTC_TESTS_FUZZ_TARGETS_HPP
