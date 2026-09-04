# docs/adr/

Architectural Decision Records (ADRs).

Each ADR captures a significant technical decision: what was decided, why,
what alternatives were considered, and what the consequences are.

## Format

ADRs follow the lightweight format described in:
https://github.com/joelparkerhenderson/architecture-decision-record

Filename convention: `ADR-<number>-<short-title>.md`

## When to write an ADR

Write an ADR when a decision:
- Affects more than one module
- Introduces a new external dependency
- Changes an existing public API
- Has significant implications for performance or security
- Was reached after meaningful debate among contributors

## Current ADRs

| Number | Title | Phase |
|--------|-------|-------|
| [ADR-001](ADR-001-plugin-system.md) | Pluggable Module Architecture | P0 |

## Process

1. Propose a decision in a GitHub Issue
2. Open a PR that adds `docs/adr/ADR-NNN-title.md`
3. Require at least one approving review from a module owner
4. Merge to record the decision (ADRs are append-only in P0/P1)
