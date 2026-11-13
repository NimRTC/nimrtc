/**
 * @file tests/test_plugin_id_contract.cpp
 * @brief ADR-023 — the plugin id contract, enforced by a test rather than by prose.
 *
 * ## Why this test exists
 *
 * `docs/adr/ADR-023-plugin-id-naming-contract.md` fixes how a plugin id is
 * spelled, validated and changed.  The ADR's Decision 7 states the contract
 * is *enforced by a test*, not described in prose — prose drifts, and a
 * contract that only exists in a Markdown file is not a contract.
 *
 * The ADR splits the enforcement into two layers:
 *
 * | Layer | Catches | Diagnostic |
 * |---|---|---|
 * | `NIMRTC_PLUGIN_ID` — compile time | the *spelling* of a literal | a compile error at the callsite |
 * | `PluginRegistry::register_one` — runtime | ids that bypass the macro | a log line at registration |
 *
 * This file covers the **runtime** layer and the **value-level** invariants:
 *
 *   1. Every id the in-tree default registrars publish satisfies the
 *      grammar.  This is the regression test that keeps the in-tree ids
 *      honest as modules are added — a new module whose id is `"Opus"`
 *      fails here, loudly, at the point where the mistake was made.
 *   2. A malformed id is rejected by the registry.  The hyphen is **not**
 *      in the malformed list: the grammar permits it, and the shipped
 *      `"webrtc-classic"` / `"raw-udp-arq"` ids must keep registering.
 *   3. A rejected id does not remove or shadow a previously-registered id
 *      in the same slot.  "One bad plugin does not poison the rest."
 *   4. The compile-time layer rejects a malformed literal.  The test file
 *      compiles, which is itself the proof — a `static_assert` inside a
 *      macro that fires only for a bad spelling cannot be checked at run
 *      time, so the proof is that the *good* spelling compiles and that
 *      the bad one does not (verified out of band; see the note at the
 *      bottom of this file).
 *
 * ## The diagnostic
 *
 * A rejected id must produce a diagnostic that names the offending id and
 * the family it was offered to.  The test installs a capturing log sink
 * and asserts on the message, because "the registry silently dropped my
 * plugin" is exactly the failure mode the ADR exists to prevent.
 *
 * ## Why the probes use the DataChannel slot
 *
 * The registry is a process-wide singleton, so a probe id that leaked out
 * of this test would be visible to every later test in the same binary.
 * The DataChannel slot is the only one with a documented test-only removal
 * helper (`unregister_datachannel`), so the probes use it and clean up
 * after themselves.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nimrtc/core/log.hpp"
#include "nimrtc/core/plugin_id.hpp"
#include "nimrtc/core/registry.hpp"

namespace {

using nimrtc::core::PluginRegistry;
namespace detail = nimrtc::core::detail;

// ---------------------------------------------------------------------------
// A log sink that captures what the registry reported.
//
// The registry's rejection diagnostic goes to the core logger (ADR-023
// Decision 4: registration happens at static-init time, where throwing
// would terminate the process before `main` with a message that names no
// plugin).  To assert on that message the test needs to *see* it, so it
// installs its own sink for the duration of the test.
// ---------------------------------------------------------------------------
class CapturingSink final : public nimrtc::core::log::Sink {
public:
    void write(nimrtc::core::log::Level level,
               std::string_view msg) noexcept override {
        lines_.emplace_back(level, std::string(msg));
    }

    /// The captured messages at the Error level.
    [[nodiscard]] std::vector<std::string> errors() const {
        std::vector<std::string> out;
        for (const auto& [level, msg] : lines_) {
            if (level == nimrtc::core::log::Level::Error) out.push_back(msg);
        }
        return out;
    }

    /// True if some captured Error mentions @p needle.
    [[nodiscard]] bool mentions(std::string_view needle) const {
        for (const std::string& msg : errors()) {
            if (msg.find(needle) != std::string::npos) return true;
        }
        return false;
    }

private:
    std::vector<std::pair<nimrtc::core::log::Level, std::string>> lines_;
};

/// RAII installer for the capturing sink.
class ScopedSink {
public:
    ScopedSink() {
        previous_ = nimrtc::core::log::Logger::instance().sink();
        nimrtc::core::log::Logger::instance().set_sink(
            std::make_shared<CapturingSink>());
    }
    ~ScopedSink() {
        nimrtc::core::log::Logger::instance().set_sink(previous_);
    }

    [[nodiscard]] CapturingSink* get() const {
        return static_cast<CapturingSink*>(
            nimrtc::core::log::Logger::instance().sink().get());
    }

private:
    nimrtc::core::log::SinkPtr previous_;
};

/// Every registry slot, with the accessor that lists its ids.
///
/// The ADR requires the in-tree ids to be honest.  "Every id" means every
/// slot — a new family that nobody thought to add to this table would
/// silently escape the check, so the table is spelled out in full.
struct SlotProbe {
    const char* name;
    std::vector<std::string> ids;
};

std::vector<SlotProbe> CollectSlots() {
    PluginRegistry& reg = PluginRegistry::instance();
    return {
        {"transport",         reg.list_transports()},
        {"ice-transport",    reg.list_ice_transports()},
        {"rtp",              reg.list_rtp()},
        {"sdp",              reg.list_sdp()},
        {"jb",               reg.list_jb()},
        {"audio3a",          reg.list_audio3a()},
        {"codec",            reg.list_codecs()},
        {"audio-decoder",    reg.list_audio_decoders()},
        {"audio-receiver",   reg.list_audio_receivers()},
        {"session",          reg.list_sessions()},
        {"dtmf",             reg.list_dtmfs()},
        {"video-codec",      reg.list_video_codecs()},
        {"video-source",     reg.list_video_sources()},
        {"video-capture",    reg.list_video_captures()},
        {"video-sink",       reg.list_video_sinks()},
        {"video-receiver",   reg.list_video_receivers()},
        {"video-sender",     reg.list_video_senders()},
        {"bwe",              reg.list_bwes()},
        {"scheduler",        reg.list_schedulers()},
        {"net-feedback",     reg.list_net_feedbacks()},
        {"xr-report",        reg.list_xr_reports()},
        {"datachannel",      reg.list_datachannels()},
        {"dtls-session",     reg.list_dtls_sessions()},
        {"sctp-socket",      reg.list_sctp_sockets()},
        {"raw-udp",          reg.list_raw_udp_datagrams()},
        {"transport-stack",  reg.list_transport_stacks()},
        {"web-transport",    reg.list_web_transports()},
        {"srtp-backend",     reg.list_srtp_backends()},
    };
}

/// True if @p id is present in @p ids.
///
/// The probes register a null factory pointer, so `get_*()` cannot be used
/// to prove presence — a null factory and a missing entry are
/// indistinguishable through the lookup API.  The listing API can tell
/// them apart, so the assertions use it.
bool Contains(const std::vector<std::string>& ids, std::string_view id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// Register @p id in the DataChannel slot and remove it again on scope exit.
///
/// The DataChannel slot is used because it is the only one with a
/// documented test-only removal helper (`unregister_datachannel`): the
/// registry is a process-wide singleton, so a probe id that leaked out of
/// this file would be visible to every later test in the same binary.
class DataChannelProbe {
public:
    explicit DataChannelProbe(std::string_view id) : id_(id) {
        PluginRegistry::instance().register_datachannel(id_, nullptr);
    }
    ~DataChannelProbe() {
        PluginRegistry::instance().unregister_datachannel(id_);
    }
    DataChannelProbe(const DataChannelProbe&) = delete;
    DataChannelProbe& operator=(const DataChannelProbe&) = delete;

private:
    std::string id_;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. The grammar itself.
// ---------------------------------------------------------------------------

TEST(PluginIdContract, GrammarAcceptsTheShippedIds) {
    // The hyphen is permitted by Decision 1 because the published
    // transport-stack ids use it.  If this ever fails, either the
    // grammar or the shipped ids changed — and the ids are public config
    // values, so the grammar is what must give.
    EXPECT_TRUE(detail::is_valid_plugin_id("opus"));
    EXPECT_TRUE(detail::is_valid_plugin_id("webrtc-classic"));
    EXPECT_TRUE(detail::is_valid_plugin_id("raw-udp-arq"));
    EXPECT_TRUE(detail::is_valid_plugin_id("nimrtc_audio_rx"));
    EXPECT_TRUE(detail::is_valid_plugin_id("a"));
    EXPECT_TRUE(detail::is_valid_plugin_id("a0"));
    EXPECT_TRUE(detail::is_valid_plugin_id("a_b-c9"));
}

TEST(PluginIdContract, GrammarRejectsTheMalformedShapes) {
    // Decision 7's item 2 lists these.  The hyphen is deliberately
    // absent — it is legal.
    EXPECT_FALSE(detail::is_valid_plugin_id(""));
    EXPECT_FALSE(detail::is_valid_plugin_id("0opus"));
    EXPECT_FALSE(detail::is_valid_plugin_id("Opus"));
    EXPECT_FALSE(detail::is_valid_plugin_id("opus codec"));
    EXPECT_FALSE(detail::is_valid_plugin_id("opus.codec"));
    EXPECT_FALSE(detail::is_valid_plugin_id("opus/codec"));
    EXPECT_FALSE(detail::is_valid_plugin_id("opus\topus"));
    // Non-ASCII: the grammar is ASCII-only, and a non-ASCII byte in an
    // id is a config-file encoding problem waiting to happen.
    EXPECT_FALSE(detail::is_valid_plugin_id("opu\xc3\xa9"));
    // A leading underscore or hyphen is not a lowercase letter.
    EXPECT_FALSE(detail::is_valid_plugin_id("_opus"));
    EXPECT_FALSE(detail::is_valid_plugin_id("-opus"));
}

TEST(PluginIdContract, ClassifierNamesTheReason) {
    // The diagnostic says *what* is wrong, not just "it is wrong".
    EXPECT_EQ(detail::classify_plugin_id("opus"),
              detail::PluginIdError::kNone);
    EXPECT_EQ(detail::classify_plugin_id(""),
              detail::PluginIdError::kEmpty);
    EXPECT_EQ(detail::classify_plugin_id("0opus"),
              detail::PluginIdError::kBadFirstCharacter);
    EXPECT_EQ(detail::classify_plugin_id("Opus"),
              detail::PluginIdError::kBadFirstCharacter);
    EXPECT_EQ(detail::classify_plugin_id("opus codec"),
              detail::PluginIdError::kBadCharacter);
    EXPECT_EQ(detail::classify_plugin_id("opu\xc3\xa9"),
              detail::PluginIdError::kBadCharacter);
}

// ---------------------------------------------------------------------------
// 2. Every in-tree id satisfies the grammar.
// ---------------------------------------------------------------------------

TEST(PluginIdContract, EveryInTreeIdSatisfiesTheGrammar) {
    nimrtc::core::register_all_default_plugins();

    for (const SlotProbe& slot : CollectSlots()) {
        for (const std::string& id : slot.ids) {
            EXPECT_TRUE(detail::is_valid_plugin_id(id))
                << "The '" << slot.name << "' slot holds the id \"" << id
                << "\", which does not match ^[a-z][a-z0-9_-]*$. "
                   "See docs/adr/ADR-023-plugin-id-naming-contract.md.";
        }
    }
}

TEST(PluginIdContract, TheShippedTransportStackIdsStillRegister) {
    // The hyphenated ids are public config values — a host writes
    // `transport_stack_name = "webrtc-classic"` — so they must survive
    // the validation that every other id goes through.
    nimrtc::core::register_all_default_plugins();
    const PluginRegistry& reg = PluginRegistry::instance();

    const auto stacks = reg.list_transport_stacks();
    EXPECT_TRUE(Contains(stacks, "webrtc-classic"))
        << "The \"webrtc-classic\" transport-stack id no longer registers. "
           "It is a public config value; the grammar must permit it.";
    EXPECT_TRUE(Contains(stacks, "raw-udp-arq"))
        << "The \"raw-udp-arq\" transport-stack id no longer registers. "
           "It is a public config value; the grammar must permit it.";
}

// ---------------------------------------------------------------------------
// 3. The registry rejects a malformed id.
// ---------------------------------------------------------------------------

TEST(PluginIdContract, RegistryRejectsAMalformedId) {
    ScopedSink sink;
    PluginRegistry& reg = PluginRegistry::instance();

    reg.register_datachannel("Bad Id", nullptr);

    // The id must not be registered.
    EXPECT_FALSE(Contains(reg.list_datachannels(), "Bad Id"))
        << "The registry accepted a malformed id (\"Bad Id\").";
}

TEST(PluginIdContract, RejectionDiagnosticNamesTheIdAndTheFamily) {
    ScopedSink sink;
    PluginRegistry& reg = PluginRegistry::instance();

    reg.register_datachannel("Bad Id", nullptr);

    EXPECT_TRUE(sink.get()->mentions("Bad Id"))
        << "The rejection diagnostic does not name the offending id.";
    EXPECT_TRUE(sink.get()->mentions("datachannel"))
        << "The rejection diagnostic does not name the family that "
           "rejected the id.";
}

TEST(PluginIdContract, RegistryRejectsEveryMalformedShape) {
    ScopedSink sink;
    PluginRegistry& reg = PluginRegistry::instance();

    const char* const kMalformed[] = {
        "",
        "0leading-digit",
        "Uppercase",
        "has space",
        "has.dot",
        "has/slash",
    };

    for (const char* id : kMalformed) {
        reg.register_datachannel(id, nullptr);
        EXPECT_FALSE(Contains(reg.list_datachannels(), id))
            << "The registry accepted the malformed id \"" << id << "\".";
    }
}

// ---------------------------------------------------------------------------
// 4. A rejected id does not poison the slot.
// ---------------------------------------------------------------------------

TEST(PluginIdContract, ARejectedIdDoesNotShadowAPreviouslyRegisteredId) {
    PluginRegistry& reg = PluginRegistry::instance();

    // Register a good id first.
    {
        DataChannelProbe probe("contract_probe_good");
        ASSERT_TRUE(Contains(reg.list_datachannels(), "contract_probe_good"));

        // Now offer a malformed one.
        {
            ScopedSink sink;
            reg.register_datachannel("Bad Id", nullptr);
        }

        // The good id must still be there — one bad plugin does not
        // poison the rest of the registry.
        EXPECT_TRUE(Contains(reg.list_datachannels(), "contract_probe_good"))
            << "A rejected registration removed or shadowed an id that was "
               "already registered in the same slot.";

        // And the malformed one must not have been registered at all.
        EXPECT_FALSE(Contains(reg.list_datachannels(), "Bad Id"));
    }
}

// ---------------------------------------------------------------------------
// 5. A good id still registers normally.
// ---------------------------------------------------------------------------

TEST(PluginIdContract, AWellFormedIdStillRegisters) {
    PluginRegistry& reg = PluginRegistry::instance();

    DataChannelProbe probe("contract_probe_valid");
    EXPECT_TRUE(Contains(reg.list_datachannels(), "contract_probe_valid"))
        << "A well-formed id was rejected — the validation is too strict.";
}

// ---------------------------------------------------------------------------
// 6. The compile-time layer.
//
// The `NIMRTC_PLUGIN_ID` macro asserts the grammar at compile time.  A
// `static_assert` inside a macro cannot be exercised from a run-time test
// — the proof that it fires is that a *bad* spelling does not compile.
// The good spelling compiles right here, which is the half of the
// contract this file can express.
//
// The other half (a bad spelling fails to compile) is verified out of
// band; the check is:
//
//     $ cat > /tmp/bad.cpp <<'EOF'
//     #include <nimrtc/core/plugin_id.hpp>
//     constexpr auto bad = NIMRTC_PLUGIN_ID("Opus");
//     EOF
//     $ g++ -std=c++20 -fsyntax-only -I src/core/include /tmp/bad.cpp
//     error: static assertion failed: NIMRTC_PLUGIN_ID: the plugin id
//     must match ^[a-z][a-z0-9_-]*$ ...
//
// (verified on GCC 11 and Clang; the assertion is in the macro body, so
// it fires wherever the macro is expanded.)
// ---------------------------------------------------------------------------

TEST(PluginIdContract, TheCompileTimeGateAcceptsAWellFormedLiteral) {
    constexpr auto id = NIMRTC_PLUGIN_ID("contract_probe_literal");
    EXPECT_EQ(std::string_view("contract_probe_literal"),
              std::string_view{id.value});
}
