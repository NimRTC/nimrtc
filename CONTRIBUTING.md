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
