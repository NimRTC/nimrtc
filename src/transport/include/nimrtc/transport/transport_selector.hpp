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
 * The Selector implementation is shipped with **TODO bodies** that
 * construct empty `TransportSession` values and log "stack factories
 * pending Slice 7.5" — because the default Stack factories depend on
 * Slice 4/5/6 default impls being merged first. The PRIORITY LOGIC is
 * fully enumerated in code comments so reviewers can verify the rule
 * set is complete without running anything.
 *
 * Integration tests use **mock factories** that bypass the
 * PluginRegistry lookups, so the selector's rule engine is testable
 * today.
// ----------------------------------------------------------------------------
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc {

// ---------------------------------------------------------------------------
// TransportRequirements — capability flags the application expresses
// ---------------------------------------------------------------------------
struct TransportRequirements {
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
    std::optional<std::string> required_backend;

    /** Soft QUIC preference (hint, not requirement). Open question §8 #2
     *  ratified "hint" — see transport-selection.md. */
    bool prefer_quic = false;
};

// ---------------------------------------------------------------------------
// ITransportSelector — interface
// ---------------------------------------------------------------------------
class ITransportSelector {
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
// Implements the four-rule priority list from §5.3. The body currently
// returns TransportSession values with empty `unique_ptr<ITransportStack>`
// because default StackFactory implementations are Slice 7.5 work.
// Test code injects MockStackFactory through the resolver below so the
// rule engine itself is testable today.
// ---------------------------------------------------------------------------
class CapabilitySelector final : public ITransportSelector {
public:
    CapabilitySelector() = default;

    /** Selector with explicit resolver hooks. Production code uses the
     *  default constructor (registry-driven lookup) but tests inject
     *  mocks to assert the rule engine without standing up the registry.
     *
     *  Resolver signatures:
     *    - `stack_factory(id)` returns a borrowed factory pointer (or
     *      `nullptr` if unknown). The selector does NOT own the
     *      pointer; the registry / mock does. The selector's contract
     *      is "I choose the id; someone else builds the stack".
     *    - `create_stack(factory, cfg)` invokes the factory's create()
     *      method and returns a unique_ptr to the resulting stack.
     *
     *  Both resolvers default to nullptr, which makes the selector
     *  operate in "rule-only" mode (returns empty TransportSession +
     *  sets the id strings correctly for downstream debugging).
     *
     *  std::function is used (not raw function pointers) so that tests
     *  can pass capturing lambdas without forcing the selector to depend
     *  on type-erased std::function in production code.
     */
    using StackFactoryResolver =
        std::function<const ITransportStackFactory*(std::string_view id)>;
    using StackCreateResolver =
        std::function<std::unique_ptr<ITransportStack>(
            const ITransportStackFactory*, const StackConfig&)>;

    CapabilitySelector(StackFactoryResolver factory_lookup,
                       StackCreateResolver factory_create) noexcept;

    TransportSession select(
        const TransportRequirements& requirements) const override;

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
};

} // namespace nimrtc
