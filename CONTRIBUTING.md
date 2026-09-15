# Contributing to NimRTC

Thank you for your interest in contributing to NimRTC.

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
