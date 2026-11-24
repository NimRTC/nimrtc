#!/usr/bin/env python3
"""Static check of the install/export contract for the NimRTC package config.

Why this exists
---------------
The installed consumer contract is a hand-written CMake package config
(`cmake/NimRTCConfig.cmake.in`) processed by `configure_package_config_file()`
in `cmake/NimRTCInstall.cmake`.  The template's `@…@` placeholders are the only
seam between the install rules and the config a host actually consumes: a
placeholder that reaches the installed file as an empty string makes
`find_package()` succeed while the target is broken, and nothing notices until
a host builds against the bundle — which macOS only started proving with N9.
This check is a cheap static proxy for that runtime proof, so a template edit
that drops a placeholder fails before merge, on every platform leg.

It verifies, without configuring or building:

  1. Every `@…@` placeholder in the template has a source at configure time:
     `@PACKAGE_INIT@` and `@PACKAGE_<PATH_VAR>@` are supplied by
     `configure_package_config_file()` itself; the remaining names must be CMake
     variables guaranteed in scope at configure time (`PROJECT_VERSION`, …).  A
     name with no source would configure to an empty string.
  2. Every PATH_VAR passed to `configure_package_config_file()` is actually
     used in the template (a redundant PATH_VAR is a note, not an error).
  3. The template hard-codes no platform-specific path — `lib64`, `/usr/local`,
     an absolute prefix.  A config that assumes GNU's `lib64` or an absolute
     install prefix breaks `find_package()` when `CMAKE_INSTALL_PREFIX` is
     relative (the staging-dir shape every CI leg uses).
  4. The template uses no GNU-only construct that would break the Apple Clang /
     MSVC legs.

    python tools/check_install_export.py
    python tools/check_install_export_self_test.py   # self-test (separate file)

Exit status: 0 when the export contract is sound, 1 when a defect is found,
2 on a usage error (missing file, unparseable call).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = REPO_ROOT / "cmake" / "NimRTCConfig.cmake.in"
INSTALL = REPO_ROOT / "cmake" / "NimRTCInstall.cmake"

_PLACEHOLDER_RE = re.compile(r"@([A-Za-z_][A-Za-z0-9_]*)@")
_CALL_RE = re.compile(r"configure_package_config_file\s*\(")

# Keywords recognised inside configure_package_config_file(...).
_CALL_KEYWORDS = frozenset((
    "PATH_VARS",
    "INSTALL_DESTINATION",
    "INSTALL_PREFIX",
    "NO_SET_AND_CHECK_MACRO",
    "NO_CHECK_REQUIRED_COMPONENTS_MACRO",
))

# Placeholder names supplied by configure_package_config_file() itself.
_AUTO_NAMES = frozenset(("PACKAGE_INIT",))

# CMake variables guaranteed in scope at configure time inside the included
# install module: project() metadata plus CMake runtime globals.  A template
# may reference them verbatim; configure_file() substitutes the values.
_CONFIG_VARS = frozenset((
    "PROJECT_VERSION",
    "PROJECT_VERSION_MAJOR",
    "PROJECT_VERSION_MINOR",
    "PROJECT_VERSION_PATCH",
    "PROJECT_NAME",
    "CMAKE_PROJECT_NAME",
    "CMAKE_PROJECT_VERSION",
    "CMAKE_SOURCE_DIR",
    "CMAKE_CURRENT_SOURCE_DIR",
    "CMAKE_CURRENT_BINARY_DIR",
    "CMAKE_HOST_SYSTEM_NAME",
))

# GNUInstallDirs variables that MUST go through PATH_VARS -> @PACKAGE_<v>@.
# Referencing them directly bakes the configure-time value (absolute, or the
# bare libdir name) into the config and breaks a relative-prefix install.
_INSTALL_DIR_VARS = frozenset((
    "CMAKE_INSTALL_PREFIX",
    "CMAKE_INSTALL_BINDIR",
    "CMAKE_INSTALL_SBINDIR",
    "CMAKE_INSTALL_LIBEXECDIR",
    "CMAKE_INSTALL_SYSCONFDIR",
    "CMAKE_INSTALL_SHAREDSTATEDIR",
    "CMAKE_INSTALL_LOCALSTATEDIR",
    "CMAKE_INSTALL_LIBDIR",
    "CMAKE_INSTALL_INCLUDEDIR",
    "CMAKE_INSTALL_OLDINCLUDEDIR",
    "CMAKE_INSTALL_DATAROOTDIR",
    "CMAKE_INSTALL_DATADIR",
    "CMAKE_INSTALL_INFODIR",
    "CMAKE_INSTALL_LOCALEDIR",
    "CMAKE_INSTALL_MANDIR",
    "CMAKE_INSTALL_DOCDIR",
))

# Absolute /usr- or /opt-shaped path prefixes a config must not hard-code.
_ABS_POSIX_RE = re.compile(r"(?<![A-Za-z0-9_.-])/(?:usr|opt|var|etc|Users|home|lib|lib64|local)/")
# Windows drive-letter absolute prefixes.
_ABS_WINDOWS_RE = re.compile(r"(?<![A-Za-z])[A-Za-z]:[\\/]")
_LIB64_RE = re.compile(r"lib64")
# GNU-dialect / GCC-only constructs a portable template must not use.
_GNU_ONLY_RE = re.compile(r"gnu\+\+|-m(?:32|64)|-static-lib|__gnu_linux__|__GNUC__")


def _strip_comment(line: str) -> str:
    """Return `line` without its trailing `#` comment (quotes respected)."""
    in_quote = False
    for i, c in enumerate(line):
        if c == '"':
            in_quote = not in_quote
        elif c == "#" and not in_quote:
            return line[:i]
    return line


def _tokenize(text: str) -> list[str]:
    """Split CMake argument text into tokens: bare words and quoted strings."""
    tokens: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c.isspace() or c in "(),":
            i += 1
            continue
        if c == '"':
            j = text.find('"', i + 1)
            if j < 0:
                tokens.append(text[i + 1:])
                break
            tokens.append(text[i + 1:j])
            i = j + 1
        elif c == "#":
            break
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '(),"':
                j += 1
            tokens.append(text[i:j])
            i = j
    return tokens


def _unquote(token: str) -> str:
    return token[1:-1] if len(token) >= 2 and token[0] == token[-1] == '"' else token


def extract_configure_call(install_text: str) -> dict | None:
    """Parse the `configure_package_config_file(...)` call in the install rules.

    Returns ``{'template', 'output', 'install_destination', 'path_vars'}`` or
    ``None`` when the call is missing or unparseable.
    """
    m = _CALL_RE.search(install_text)
    if m is None:
        return None
    start = m.end() - 1  # position of '('
    depth = 0
    i = start
    while i < len(install_text):
        c = install_text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                break
        elif c == '"':
            j = install_text.find('"', i + 1)
            if j < 0:
                break
            i = j
        i += 1
    if depth != 0:
        return None
    tokens = _tokenize(install_text[start + 1:i])
    args = [t for t in tokens if t not in ("(", ")", ",")]
    if len(args) < 2:
        return None
    info: dict = {
        "template": _unquote(args[0]),
        "output": _unquote(args[1]),
        "install_destination": None,
        "path_vars": [],
    }
    i = 2
    while i < len(args):
        token = args[i]
        if token in _CALL_KEYWORDS:
            keyword = token
            i += 1
            if keyword == "PATH_VARS":
                while i < len(args) and args[i] not in _CALL_KEYWORDS:
                    info["path_vars"].append(_unquote(args[i]))
                    i += 1
            elif keyword in ("INSTALL_DESTINATION", "INSTALL_PREFIX"):
                if i < len(args):
                    info["install_destination"] = _unquote(args[i])
                    i += 1
            continue
        i += 1
    return info


def extract_placeholders(template_text: str) -> list[str]:
    """Names of the `@…@` placeholders in the template, in first-use order.

    Comment-only lines are ignored: a `@…@` inside a comment is not part of
    the generated config and must not be treated as a placeholder.
    """
    names: list[str] = []
    seen: set[str] = set()
    for raw in template_text.splitlines():
        code = _strip_comment(raw)
        for m in _PLACEHOLDER_RE.finditer(code):
            name = m.group(1)
            if name not in seen:
                seen.add(name)
                names.append(name)
    return names


def check_placeholders(names: list[str], path_vars: list[str]) -> list[str]:
    """Every placeholder must have a configure-time source (else: empty string)."""
    problems: list[str] = []
    auto_package = {"PACKAGE_" + v for v in path_vars}
    for name in names:
        if name in _AUTO_NAMES or name in auto_package:
            continue
        if name in _INSTALL_DIR_VARS:
            problems.append(
                f"template references @{name}@ directly; pass it via PATH_VARS "
                f"and use @PACKAGE_{name}@ so the path stays relative to the "
                f"install prefix")
            continue
        if name in _CONFIG_VARS:
            continue
        problems.append(
            f"placeholder @{name}@ has no source: it is not @PACKAGE_INIT@, not "
            f"@PACKAGE_<PATH_VAR>@ (PATH_VARS={path_vars or 'none'}), and not a "
            f"known configure-time variable -- the installed config will carry "
            f"an empty string")
    return problems


def check_path_vars_usage(template_text: str, path_vars: list[str]) -> list[str]:
    """A PATH_VAR the template never uses is redundant (note, not an error)."""
    notes: list[str] = []
    for var in path_vars:
        if f"@PACKAGE_{var}@" not in template_text:
            notes.append(
                f"PATH_VARS passes {var} but the template never references "
                f"@PACKAGE_{var}@ (redundant PATH_VARS entry)")
    return notes


def check_hardcoded_paths(template_text: str) -> list[str]:
    """No platform-specific path may be baked into the template."""
    problems: list[str] = []
    for lineno, raw in enumerate(template_text.splitlines(), 1):
        code = _strip_comment(raw).strip()
        if not code:
            continue
        if _ABS_POSIX_RE.search(code):
            problems.append(f"line {lineno}: absolute POSIX path in template: {code}")
        if _ABS_WINDOWS_RE.search(code):
            problems.append(f"line {lineno}: absolute Windows path in template: {code}")
        if _LIB64_RE.search(code):
            problems.append(f"line {lineno}: hard-coded lib64 in template: {code}")
    return problems


def check_gnu_only(template_text: str) -> list[str]:
    """No GNU-only construct may break the Apple Clang / MSVC legs."""
    problems: list[str] = []
    for lineno, raw in enumerate(template_text.splitlines(), 1):
        code = _strip_comment(raw).strip()
        if not code:
            continue
        if _GNU_ONLY_RE.search(code):
            problems.append(f"line {lineno}: GNU-only construct in template: {code}")
    return problems


def check_all(template_text: str, install_text: str) -> tuple[list[str], list[str]]:
    """Run every check; return ``(problems, notes)``."""
    problems: list[str] = []
    notes: list[str] = []
    call = extract_configure_call(install_text)
    if call is None:
        problems.append(
            "cmake/NimRTCInstall.cmake has no parseable "
            "configure_package_config_file() call")
        return problems, notes
    template_arg = call.get("template")
    if template_arg and "NimRTCConfig.cmake.in" not in template_arg:
        notes.append(
            f"configure_package_config_file() configures '{template_arg}', not "
            f"the checked NimRTCConfig.cmake.in")
    path_vars = call.get("path_vars", [])
    if not path_vars:
        problems.append(
            "configure_package_config_file() passes no PATH_VARS -- the "
            "template's @PACKAGE_<PATH_VAR>@ references would be empty")
    names = extract_placeholders(template_text)
    problems += check_placeholders(names, path_vars)
    notes += check_path_vars_usage(template_text, path_vars)
    problems += check_hardcoded_paths(template_text)
    problems += check_gnu_only(template_text)
    return problems, notes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--template", default=str(TEMPLATE),
                        help="package-config template to check "
                             "(default: the repo's NimRTCConfig.cmake.in)")
    parser.add_argument("--install", default=str(INSTALL),
                        help="install rules containing configure_package_config_file "
                             "(default: the repo's NimRTCInstall.cmake)")
    args = parser.parse_args(argv)

    template_path = Path(args.template)
    install_path = Path(args.install)
    if not template_path.is_file():
        print(f"error: {template_path} does not exist", file=sys.stderr)
        return 2
    if not install_path.is_file():
        print(f"error: {install_path} does not exist", file=sys.stderr)
        return 2

    template_text = template_path.read_text(encoding="utf-8")
    install_text = install_path.read_text(encoding="utf-8")
    problems, notes = check_all(template_text, install_text)

    for note in notes:
        print("NOTE  " + note)
    for problem in problems:
        print("FAIL  " + problem)
    if problems:
        return 1

    names = extract_placeholders(template_text)
    print(f"PASS -- {template_path.name}: {len(names)} placeholder(s) all have "
          f"sources; no hard-coded paths; no GNU-only constructs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
