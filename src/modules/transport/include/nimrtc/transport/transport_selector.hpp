/**
 * @file nimrtc/transport/transport_selector.hpp
 * @brief ITransportSelector + TransportRequirements + CapabilitySelector.
 *
 * Slice 7 of the transport-selection plan
 * (docs/plan/transport-selection.md §5.2 + §5.3 + §6.4). The Selector
 * maps a `TransportRequirements` (capability flags from the JSON
 * Profile) to a `TransportSession` (media_stack + control_stack pair).
 *
 * ----------------------------------------------------------------------------
 * §5.3 PRIORITY RULES (numbered — must appear in select() body in order)
 * ----------------------------------------------------------------------------
 *   1. needs_browser_interop == true
 *      → media_stack  = webrtc-classic
 *      → control_stack = raw-udp-arq if needs_high_freq_control, else null
 *
 *   2. needs_browser_interop == false && needs_high_freq_control == true
 *      → media_stack    = raw-udp-arq
 *      → control_stack  = raw-udp-arq (independent stack)
 *
 *   3. needs_browser_interop == false && prefer_quic == true
 *      → media_stack = webrtc-quic (Slice 7.x 二期, msquic/ngtcp2)
 *      → control_stack = per needs_high_freq_control
 *
 *   4. Default
 *      → media_stack = webrtc-classic
 *      → control_stack = per needs_high_freq_control
 *
 * ----------------------------------------------------------------------------
 * Slice-7 DELIVERABLE
 * ----------------------------------------------------------------------------
 * The Selector implementation resolves each stack factory by id and
 * constructs the stack through the injected lookup / create hooks.  The
 * PRIORITY LOGIC is fully enumerated in code comments so reviewers can
 * verify the rule set is complete without running anything.
 *
 * Only the `webrtc-quic` branch (rule 3) is still a stub — it needs the
 * msquic / ngtcp2 backends.
 *
 * Integration tests use **mock factories** that bypass the
 * PluginRegistry lookups, so the selector's rule engine is testable
 * today.
// ----------------------------------------------------------------------------
 */
#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc {

// ---------------------------------------------------------------------------
// TransportRequirements — capability flags the application expresses
// ---------------------------------------------------------------------------
struct NIMRTC_API_STABLE TransportRequirements {
    /** True if the connection must terminate in a browser (Chrome/Firefox/
     *  Safari). Forces webrtc-classic media plane. */
    bool needs_browser_interop = true;

    /** True if a separate, low-latency control channel is required (50–200 Hz,
     *  p99 ≤ 80 ms budget). Triggers raw-udp-arq side-stack when allowed. */
    bool needs_high_freq_control = false;

    /** Hard ceiling on end-to-end p99 latency in ms. Empty = no cap. */
    std::optional<int> max_p99_ms;

    /** Force a specific backend id (e.g. "wolfssl"). Empty = any registered
     *  implementation is acceptable. */
    std::optional<std::string> required_backend = std::nullopt;

    /** Soft QUIC preference (hint, not requirement). Open question §8 #2
     *  ratified "hint" — see transport-selection.md. */
    bool prefer_quic = false;
};

// ---------------------------------------------------------------------------
// ITransportSelector — interface
// ---------------------------------------------------------------------------
class NIMRTC_API_STABLE ITransportSelector {
public:
    virtual ~ITransportSelector() = default;

    /** Map requirements → TransportSession. Idempotent / pure with respect
     *  to `requirements` — does NOT touch the network or allocate any
     *  plugin; just decides which factory ids to use. The actual stack
     *  instantiation is the caller's job (or future PluginRegistry
     *  resolution). */
    virtual TransportSession select(
        const TransportRequirements& requirements) const = 0;
};

// ---------------------------------------------------------------------------
// CapabilitySelector — default ITransportSelector (Slice 7)
// ---------------------------------------------------------------------------
//
// Implements the four-rule priority list from §5.3.
//
// The rule engine resolves a factory id for the media plane and (when the
// requirements ask for it) for the control plane.  Whether a *stack* is
// actually constructed depends on the resolver hooks:
//
//   - "rule-only" mode (default ctor): the ids are resolved and recorded
//     in `last_trace()`, but no stack is built.  The Slice-7 unit tests
//     use this mode.
//   - "resolving" mode (the engine): the resolver hooks are bound to
//     `core::PluginRegistry`, so the returned `TransportSession` carries
//     live `ITransportStack` instances.
// ---------------------------------------------------------------------------
class NIMRTC_API_STABLE CapabilitySelector final : public ITransportSelector {
public:
    /** Factory lookup hook — maps a factory id to a borrowed
     *  `ITransportStackFactory*` (or nullptr when the id is unknown). */
    using StackFactoryResolver =
        std::function<const ITransportStackFactory*(std::string_view id)>;
    /** Stack construction hook — invokes the factory's `create(cfg)`. */
    using StackCreateResolver =
        std::function<std::unique_ptr<ITransportStack>(
            const ITransportStackFactory*, const StackConfig&)>;

    /** Default-constructed selector — "rule-only" mode.
     *
     *  No resolver hooks are installed, so `select()` resolves the §5.3
     *  factory ids but constructs no stacks (the returned session carries
     *  the id strings and null stack pointers).  This is the mode the
     *  Slice-7 unit tests use.
     */
    CapabilitySelector() = default;

    /** Selector with explicit resolver hooks.
     *
     *  Resolver signatures:
     *    - `stack_factory(id)` returns a borrowed factory pointer (or
     *      `nullptr` if unknown). The selector does NOT own the
     *      pointer; the registry / mock does. The selector's contract
     *      is "I choose the id; someone else builds the stack".
     *    - `create_stack(factory, cfg)` invokes the factory's create()
     *      method and returns a unique_ptr to the resulting stack.
     *
     *  A null resolver makes the selector operate in "rule-only" mode
     *  (returns an empty TransportSession + sets the id strings
     *  correctly for downstream debugging).
     *
     *  std::function is used (not raw function pointers) so that tests
     *  can pass capturing lambdas without forcing the selector to depend
     *  on type-erased std::function in production code.
     *
     *  @param cfg  The per-stack backend selection handed to the create
     *               resolver.  Before v1.2.0 the selector always used a
     *               hard-coded default `StackConfig`, which meant the
     *               engine's `EngineConfig` (DTLS backend, SCTP socket,
     *               raw-UDP factory) never reached the stack factory.
     *               The default constructor keeps the old behaviour
     *               (default `StackConfig`); the engine passes its own.
     */
    CapabilitySelector(StackFactoryResolver factory_lookup,
                       StackCreateResolver factory_create,
                       const StackConfig& cfg = StackConfig{}) noexcept;

    /** Set the per-stack backend selection.  Equivalent to passing it
     *  to the three-argument constructor. */
    void set_stack_config(const StackConfig& cfg) noexcept { stack_cfg_ = cfg; }

    /** The per-stack backend selection in force. */
    [[nodiscard]] const StackConfig& stack_config() const noexcept { return stack_cfg_; }

    /** Explicit media-stack override.
     *
     *  When non-empty, the selector ignores the §5.3 rule's media id and
     *  uses this one instead.  This is the engine's escape hatch for
     *  `EngineConfig::transport_stack_name`: the user says "I want THIS
     *  stack", the Selector says "the strategy would have chosen THAT
     *  one".  The user's explicit choice wins.
     *
     *  The control stack is NOT affected — the strategy still decides
     *  whether a separate control plane exists.
     */
    void set_media_factory_override(std::string id) {
        media_override_ = std::move(id);
    }

    TransportSession select(
        const TransportRequirements& requirements) const override;

    /** Build a session from **explicit** factory ids.
     *
     *  This is the "the caller already knows which stack it wants" entry
     *  point, and it is what `nimrtc::transport::make_transport_instance()`
     *  delegates to.  It performs no rule resolution — the ids are taken
     *  verbatim — but it uses the same resolver hooks and the same
     *  `StackConfig` handling as `select()`, so there is exactly one
     *  implementation of "turn a factory id into a stack".
     *
     *  @param media_factory_id   The media stack factory id.  Must be
     *                             non-empty; an empty id yields an empty
     *                             session (the caller reports the error).
     *  @param control_factory_id The control stack factory id.  Empty = no
     *                             control stack.
     */
    TransportSession instantiate(std::string_view media_factory_id,
                                 std::string_view control_factory_id) const;

    // ---- Test-only accessors ---------------------------------------------
    //
    // Tests verify that the selector picked the right factory id for
    // each rule. These accessors return the most recent choice the
    // select() invocation made.
    struct SelectionTrace {
        std::string media_factory_id;
        std::string control_factory_id;
    };
    SelectionTrace last_trace() const noexcept { return last_trace_; }

private:
    StackFactoryResolver factory_lookup_ = nullptr;
    StackCreateResolver  factory_create_  = nullptr;
    mutable SelectionTrace last_trace_{};

    /** The per-stack backend selection handed to the create resolver.
     *  Mirrors what the engine's `EngineConfig` prescribes (ICE / DTLS /
     *  RTP / SCTP / raw-UDP ids). */
    StackConfig stack_cfg_{};

    /** Explicit media-stack id override (see
     *  `set_media_factory_override`).  Empty = the §5.3 rule decides. */
    std::string media_override_;
};

} // namespace nimrtc
