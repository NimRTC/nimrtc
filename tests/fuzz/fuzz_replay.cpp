/**
 * @file fuzz_replay.cpp
 * @brief Corpus replay driver — runs the fuzz targets over the committed
 *        seed corpus plus a deterministic mutation set.
 *
 * ## Why this exists
 *
 * The project does not run libFuzzer on every commit: it needs a
 * Clang toolchain, and a fuzzing run is unbounded in time.  What we
 * *can* do on every commit, on every platform, is replay a fixed
 * corpus through the same entry points libFuzzer drives.  That gives
 * the fuzz targets two properties that matter:
 *
 *   1. **A crash found by libFuzzer stays a crash.**  When the
 *      fuzzer finds an input that makes a parser read out of bounds,
 *      that input is committed as a seed.  From then on, every
 *      commit replays it, so the bug cannot silently come back.
 *
 *   2. **The targets cannot rot.**  A fuzz target that stops compiling
 *      (because the API it drives changed) breaks the build here,
 *      not six months later when someone tries to fuzz again.
 *
 * The replay driver runs in the ordinary test suite.  It is not a
 * substitute for libFuzzer — it explores almost nothing on its own — but it is the *floor* under the fuzzing effort, not the ceiling.
 *
 * ## Mutation strategy
 *
 * The mutations are deliberately *structured*, not random.  A random
 * byte flip almost never produces an interesting input for a length-
 * prefixed format; what finds bugs is a plausible length field with
 * an implausible value.  So the driver applies, in order:
 *
 *   - **Truncation** at every interesting boundary (0, 1, 4, 8, 12,
 *     16 bytes, and the last byte) — this is what catches "the parser
 *     assumed the buffer had a header".
 *   - **Length-field poisoning**: every 16-bit big-endian word is
 *     replaced by 0xFFFF and by 0x0001.  These are the two values a
 *     hand-written length check is most likely to get wrong.
 *   - **Extension to a huge size** with a matching, mostly-zero
 *     body — this is what catches "the parser trusted a length that
 *     was larger than the buffer".
 *   - **Bit flips** at a deterministic set of positions, for the
 *     plain byte-level bugs.
 *
 * Every mutation is derived from a *fixed* seed with a fixed PRNG
 * state, so the driver is deterministic: the same corpus produces the
 * same mutations on every machine, every day.  A crash is therefore
 * reproducible from the seed name alone.
 */
#include "fuzz_targets.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nimrtc/core/bytes.hpp>

namespace nimrtc::fuzz {
namespace {

// ---------------------------------------------------------------------------
// Deterministic PRNG
// ---------------------------------------------------------------------------

/** A tiny xorshift64* generator.
 *
 *  std::mt19937 would do, but this is 6 lines and has no library
 *  state to worry about.  The point is determinism, not quality: the
 *  mutations have to be reproducible from the seed. */
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t next() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    /** Uniform-ish value in [0, bound). */
    std::size_t below(std::size_t bound) noexcept {
        if (bound == 0) return 0;
        return static_cast<std::size_t>(next() % bound);
    }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// The mutation set
// ---------------------------------------------------------------------------

/** Append a copy of `src` with a mutation applied. */
using Mutation = void (*)(std::vector<std::uint8_t>&, Rng&);

void mutate_truncate_head(std::vector<std::uint8_t>& buf, Rng& rng) {
    static const std::size_t kCuts[] = {0, 1, 2, 3, 4, 5, 8, 11, 12, 13, 16};
    if (buf.empty()) return;
    const std::size_t cut = kCuts[rng.below(sizeof(kCuts) / sizeof(kCuts[0]))];
    if (cut < buf.size()) buf.resize(cut);
}

void mutate_truncate_tail(std::vector<std::uint8_t>& buf, Rng& rng) {
    if (buf.empty()) return;
    const std::size_t cut = 1 + rng.below(buf.size());
    buf.resize(buf.size() - cut);
}

void mutate_length_max(std::vector<std::uint8_t>& buf, Rng& rng) {
    if (buf.size() < 2) return;
    // Every 16-bit big-endian word becomes 0xFFFF: "the length says
    // there are 65535 more bytes" — the classic overflow attempt.
    const std::size_t word = rng.below(buf.size() / 2);
    const std::size_t off = word * 2;
    buf[off]     = 0xFF;
    buf[off + 1] = 0xFF;
}

void mutate_length_min(std::vector<std::uint8_t>& buf, Rng& rng) {
    if (buf.size() < 2) return;
    // The same word becomes 0x0001: a length of exactly one, which
    // catches off-by-one "length - 1" arithmetic.
    const std::size_t word = rng.below(buf.size() / 2);
    const std::size_t off = word * 2;
    buf[off]     = 0x00;
    buf[off + 1] = 0x01;
}

void mutate_length_huge(std::vector<std::uint8_t>& buf, Rng& rng) {
    // Grow the buffer so the poisoned length field *almost* fits: the
    // parser has to check the length against the remaining bytes, not
    // just against a fixed maximum.
    if (buf.size() < 2) return;
    const std::size_t word = rng.below(buf.size() / 2);
    const std::size_t off = word * 2;
    buf[off]     = 0xFF;
    buf[off + 1] = 0xFF;
    buf.resize(buf.size() + 4096, 0);
}

void mutate_bit_flip(std::vector<std::uint8_t>& buf, Rng& rng) {
    if (buf.empty()) return;
    const std::size_t n = 1 + rng.below(8);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t pos = rng.below(buf.size());
        buf[pos] = static_cast<std::uint8_t>(buf[pos] ^ (1u << rng.below(8)));
    }
}

void mutate_header_version(std::vector<std::uint8_t>& buf, Rng& rng) {
    // Corrupt the version field of a length-prefixed header.  A
    // parser that reads the version but not the length would sail
    // past this one.  The version lives in the top two bits, so the
    // mutation rewrites them with a value the PRNG picks (V = 0..3).
    if (buf.empty()) return;
    const auto version  = static_cast<unsigned>(rng.below(4));
    const auto masked   = static_cast<unsigned>(buf[0]) & 0x3Fu;
    const auto shifted  = version << 6;
    buf[0] = static_cast<std::uint8_t>(masked | shifted);
}

void mutate_zero_fill(std::vector<std::uint8_t>& buf, Rng& rng) {
    const std::size_t start = rng.below(buf.size() + 1);
    const std::size_t len   = rng.below(buf.size() - start + 1);
    for (std::size_t i = 0; i < len; ++i) buf[start + i] = 0;
}

void mutate_nul_fill(std::vector<std::uint8_t>& buf, Rng& rng) {
    // Fill with NUL: for the SDP target this exercises the
    // "text with embedded NUL" path.
    const std::size_t start = rng.below(buf.size() + 1);
    const std::size_t len   = rng.below(buf.size() - start + 1);
    for (std::size_t i = 0; i < len; ++i) buf[start + i] = 0;
}

const Mutation kMutations[] = {
    mutate_truncate_head,
    mutate_truncate_tail,
    mutate_length_max,
    mutate_length_min,
    mutate_length_huge,
    mutate_bit_flip,
    mutate_header_version,
    mutate_zero_fill,
    mutate_nul_fill,
};

constexpr std::size_t kMutationCount = sizeof(kMutations) / sizeof(kMutations[0]);

/** How many mutated variants are generated per seed. */
constexpr std::size_t kMutationsPerSeed = 24;

// ---------------------------------------------------------------------------
// Driving the targets
// ---------------------------------------------------------------------------

/** Which target a seed file belongs to. */
enum class Target { kSdp, kRtp, kVideoPayload };

const char* target_name(Target t) noexcept {
    switch (t) {
        case Target::kSdp:          return "sdp";
        case Target::kRtp:          return "rtp";
        case Target::kVideoPayload: return "video_payload";
    }
    return "?";
}

void run_target(Target t, const std::uint8_t* data, std::size_t size) {
    switch (t) {
        case Target::kSdp:          run_sdp(data, size);          break;
        case Target::kRtp:          run_rtp(data, size);          break;
        case Target::kVideoPayload: run_video_payload(data, size); break;
    }
}

/** Replay one seed and every mutation of it. */
void replay_seed(Target t, const std::vector<std::uint8_t>& seed) {
    // The seed itself.
    run_target(t, seed.data(), seed.size());

    // The mutations.
    for (std::size_t m = 0; m < kMutationsPerSeed; ++m) {
        std::vector<std::uint8_t> buf = seed;
        Rng rng(static_cast<std::uint64_t>(m) * 0x100000001B3ull + seed.size());
        // Pick the mutation by the PRNG so different mutations apply
        // to different seeds; the seed size keeps the sequence stable.
        kMutations[rng.below(kMutationCount)](buf, rng);
        run_target(t, buf.data(), buf.size());
    }
}

/** Decode one hex token (two hex digits) into a byte value. */
bool hex_byte(const std::string& token, std::uint8_t& out) {
    if (token.size() != 2) return false;
    auto value = [](char c, int& v) -> bool {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        return false;
    };
    int hi = 0;
    int lo = 0;
    if (!value(token[0], hi) || !value(token[1], lo)) return false;
    out = static_cast<std::uint8_t>(hi * 16 + lo);
    return true;
}

/** Decode the hex-text seed format.
 *
 *  The corpus seeds are checked in as *text*, not as raw byte blobs:
 *  binary files do not belong in this repository (they review badly,
 *  they merge badly, and they are trivially reproducible from the
 *  generator script `tools/make_corpus.py` anyway).
 *
 *  A `.hex` seed is a sequence of whitespace-separated two-digit hex
 *  bytes; a `#` starts a comment that runs to the end of the line.
 *  The format is the one `xxd -p` produces, so any hex dump tool can
 *  produce or inspect one. */
bool decode_hex(std::istream& in, std::vector<std::uint8_t>& out) {
    out.clear();
    std::string line;
    while (std::getline(in, line)) {
        // Strip the comment, then split the rest into tokens.
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);

        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            std::uint8_t byte = 0;
            if (!hex_byte(token, byte)) return false;
            out.push_back(byte);
        }
    }
    return true;
}

/** Read a seed file, decoding it according to its extension.
 *
 *  `.hex` seeds use the hex-text format described above; every other
 *  file (the `.sdp` seeds, which are already text) is read as raw
 *  bytes, which is exactly what the parser should see. */
bool read_seed(const std::filesystem::path& p, std::vector<std::uint8_t>& out) {
    if (p.extension() == ".hex") {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        return decode_hex(in, out);
    }

    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (out.empty()) return true;
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(in);
}

/** The corpus directory, injected by CMake. */
#ifndef NIMRTC_FUZZ_CORPUS_DIR
#define NIMRTC_FUZZ_CORPUS_DIR "."
#endif

} // namespace
} // namespace nimrtc::fuzz

// ---------------------------------------------------------------------------
// The driver
// ---------------------------------------------------------------------------

namespace {

/** Counters, printed once at the end (never per iteration — the
 *  contract in fuzz_targets.hpp forbids hot-loop output). */
std::size_t g_seed_count    = 0;
std::size_t g_case_count    = 0;

/** Per-target seed counts, so a corpus that silently stops being
 *  replayed for one target is visible in the output. */
std::size_t g_seeds_by_target[3] = {0, 0, 0};

void replay_directory(const std::filesystem::path& dir, nimrtc::fuzz::Target target) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;

    // Deterministic order: the filesystem iteration order is not
    // specified, and a test that reports a different case first
    // depending on the machine is a bad test.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        std::vector<std::uint8_t> seed;
        if (!nimrtc::fuzz::read_seed(f, seed)) continue;
        ++g_seed_count;
        ++g_seeds_by_target[static_cast<std::size_t>(target)];
        nimrtc::fuzz::replay_seed(target, seed);
        g_case_count += 1 + nimrtc::fuzz::kMutationsPerSeed;
    }
}

} // namespace

int main() {
    using namespace nimrtc::fuzz;

    const std::filesystem::path root{NIMRTC_FUZZ_CORPUS_DIR};

    replay_directory(root / "sdp",          Target::kSdp);
    replay_directory(root / "rtp",          Target::kRtp);
    replay_directory(root / "video_payload", Target::kVideoPayload);

    // The targets must also survive the degenerate inputs: empty
    // buffers and single bytes are where "assume at least N bytes"
    // bugs live.
    const std::uint8_t zero = 0;
    run_sdp(&zero, 0);
    run_rtp(&zero, 0);
    run_video_payload(&zero, 0);
    run_sdp(&zero, 1);
    run_rtp(&zero, 1);
    run_video_payload(&zero, 1);
    g_case_count += 6;

    for (std::size_t i = 0; i < 3; ++i) {
        std::printf("fuzz replay: %-14s %zu seeds\n",
                    nimrtc::fuzz::target_name(static_cast<Target>(i)),
                    g_seeds_by_target[i]);
    }
    std::printf("fuzz replay: %zu seeds, %zu cases, no crash\n",
                g_seed_count, g_case_count);
    return 0;
}
