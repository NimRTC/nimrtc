# Contributing to NimRTC

Thank you for your interest in contributing to NimRTC.

## Questions & discussion

For questions not covered by these guidelines, start a thread in
[**GitHub Discussions → Q&A**](https://github.com/NimRTC/NimRTC/discussions/categories/Q&A).
For design discussion before opening a PR, use
[**GitHub Discussions → Ideas**](https://github.com/NimRTC/NimRTC/discussions/categories/Ideas).
See [`.github/DISCUSSIONS.md`](.github/DISCUSSIONS.md) for category descriptions.

## Developer Certificate of Origin (DCO)

All contributions to NimRTC must include a signed-off-by line certifying that
you have the right to contribute under the terms of the Apache-2.0 license.

Use `git commit -s` to add the signed-off-by line automatically:

```bash
git commit -s -m "feat(rtp): add RFC 5285 extension parser"
```

Commits without `-s` will be rejected by the CI.

## Development setup

### Prerequisites

- C++20 compiler: MSVC 19.43+, GCC 10+, or Clang 12+
- CMake 3.20+
- Python 3.8+ (for codegen and tooling)
- On Windows: Visual Studio 2022 or Visual Studio Build Tools 2022

### Quick start

```bash
# Configure (Debug, using clang on Linux)
cmake --preset debug

# Build
cmake --build --preset debug

# Run tests (substitute `tests.msvc` on Windows / MSVC)
ctest --preset tests          # Linux/macOS
ctest --preset tests.msvc     # Windows / MSVC
```

### Code formatting

All C++ code must be formatted with `clang-format` before committing.
A hook is planned for pre-commit enforcement (P1).

```bash
# Format a single file
clang-format -i src/modules/rtp/src/parser.cpp

# Format all source files
find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
```

### Code style summary

- **C++ standard**: C++20 (no C++23 features)
- **Column limit**: 100 characters
- **Naming**: `PascalCase` for types, `camelCase` for functions/variables,
  `kPascalCase` for constants
- **No tabs**: spaces only, 4-space indent
- **No trailing whitespace**
- **No mutable global state** in public headers

## Pull request process

1. **Fork** the repository and create a branch from `main`.
   Use a descriptive branch name: `feat/<module>/<short-description>`.
2. **Implement** the change. Keep each PR focused — one feature or bug fix per PR.
3. **Add tests** for any new public API or non-trivial logic.
4. **Run locally** before pushing:
   ```bash
   cmake --build --preset asan
   ctest --preset tests          # Linux/macOS
   ctest --preset tests.msvc     # Windows / MSVC
   ```
5. **Push** and open a Pull Request against `main`.
6. A maintainer will review. Address feedback by amending or adding commits.
7. Once approved, a maintainer will squash-merge your PR.

## Branch protection — `main` is sacred

`main` only contains code that has been **strictly tested** and has cleared all
required CI checks. This is a hard rule, not a guideline. The build, the unit
tests, and the linter must all be green on the merge commit before `main`
advances. No exceptions, no "we'll fix it in a follow-up".

### Why this matters

NimRTC is consumed as a library by downstream projects. A broken `main` propagates
to every consumer on the next bump. Catching issues at the PR stage is cheap;
catching them after a release is expensive. The strict rule keeps `git bisect`
trustworthy and lets users track `main` without fear.

### Required CI gates (all must pass before merge)

| Gate | What it checks | Who owns it |
|---|---|---|
| **Linux build + tests** | GCC + Clang, ASan + UBSan, full `ctest` | CI |
| **Windows build + tests** | MSVC 19.43+, full `ctest` | CI |
| **clang-format** | C++ style enforcement | Pre-commit hook (planned P1) |
| **License headers + DCO** | `Signed-off-by` line + SPDX | CI |
| **Codeowners review** | One reviewer per touched module | CODEOWNERS file |

A PR that turns any of these red stays red until fixed. **Do not bypass**
branch protection by pushing to `main` directly — `main` is write-protected
for everyone except release managers.

### The slice workflow (canonical example)

Large refactors that touch many modules are split into **slices**. Each slice:

1. Lands as one PR with a single conventional-commit message
   (e.g. `refactor(dtls): add DTLS PAL seam (Slice 4)`).
2. Builds clean and passes 100% of `ctest` **on its own** — no "merge and see".
3. Does not break tests for slices that already landed (regression bar).
4. Documents its scope in `docs/plan/<slice-name>.md` so reviewers can
   audit the rule set before reading the diff.

The `refactor/transport-seam-slices-4-8` branch that landed DTLS (Slice 4),
SCTP (Slice 5), raw-UDP (Slice 6), and the transport Selector (Slice 7) is
the canonical worked example. Each slice had its own test target, its own
build verification, and its own commit — `main` never saw an unbuilt tree.

### When you find a test gap on `main`

You **fix forward** in a new PR, you don't paper over it. Steps:

1. Open an issue describing the gap (or a low-priority backlog ticket).
2. Write a failing test on a branch.
3. Land the test (red) plus the fix (green) as one PR.
4. Merge into `main` once CI is green.

This keeps `main` honest at every commit and avoids the "fix in flight"
anti-pattern where a PR fixes a regression it didn't introduce.

### Review documents: what stays in the tree and what does not

Reviews are how findings become work, and the tree has to hold that work
-- but a review document is not the machinery, and a tree that keeps
every review accumulates process history nobody reads.  The history
before 2026-11-25 had no rule at all, and the result was two PRs on the
same subject that did opposite things: one **deleted** a review
(`reviews/2026-11-24-nimrtc-feedback.md`, 291 lines, in #174) because
"it is a document about the process, not part of the machinery"; three
days later another **added** a 640-line plan derived from a review of
the same kind (#189, `docs/plan/v1.5-plan.md`).  Neither was wrong on
its own terms.  Without a rule, both could be.

The rule, from 2026-11-27:

| Document | Where it lives | Why |
|---|---|---|
| **A plan** -- work items with a writeset, a probe and a decision each | `docs/plan/<version>-plan.md`, committed | It is the contract the code is written against; the code and the plan are reviewed together. |
| **A design record** -- a decision that constrains future code | `docs/adr/ADR-NNN-*.md`, committed | An ADR is cited by code and by the plan; it has to be in the tree. |
| **A finding that fixes a defect** | Not a document: the fix, its test, and a `CHANGELOG.md` entry | The finding's value is the change it caused, and the changelog is where the *why* is kept. |
| **A review of a plan or a PR** -- prose about a moment in time | outside the repository | It describes a tree that has moved on; nothing in the tree may cite it. |
| **A survey that a plan quotes** | summarised in the plan, with the date it was made | A plan that depends on a document outside the tree must stand without it -- quote the claims, do not link the document. |

The test is one question: **does the tree's code or its plans need to
cite this file?**  If yes, it belongs in `docs/plan/` or `docs/adr/`.
If no, it does not belong in the tree -- and the PR that removes it
says so, in the commit message, so the *next* review author is not
left guessing which kind theirs is.

The existing documents under `docs/plan/` (`v0.11-preview.md`,
`v0.15.0-self-review.md`, `dtls_chrome_interop_review.md`,
`acceptance-mutation-test-review.md`) predate the rule.  They are kept
as they are: they are cited by the plans and ADRs around them, which is
the test above, and deleting them now would remove the citations.  The
rule is forward-looking; it is not a retro-active sweep.

## Commit message format

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short summary>

[optional body]

[optional footer]
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `perf`, `ci`

Examples:
```
feat(rtp): add RFC 5285 extension parser
fix(jb): prevent underflow in adaptive delay calculation
docs(sdp): document Direction enum values
test(core): add Result<T> move-semantics coverage
ci: add clang-tidy to GitHub Actions
```

## Module ownership

See `CODEOWNERS` for per-module maintainers. Changes to a module's public API
require review from its owner.

## Reporting security issues

See `SECURITY.md`. **Do not open a public GitHub issue for security vulnerabilities.**

## License

By contributing to NimRTC, you agree that your contributions will be
licensed under the Apache-2.0 License.
