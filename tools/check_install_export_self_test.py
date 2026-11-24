#!/usr/bin/env python3
"""Self-test for tools/check_install_export.py.

The gate only reports the defects check_install_export.py recognises, so this
pins that behaviour: a template that drops a placeholder MUST be reported, a
normal template MUST stay clean, and the real tree MUST pass.  Same shape as
the existing `--self-test` precedents in ci.yml (`check_readme_release.py`,
`check_release_tag_filter.py`) — except this one is a standalone file so the
positive/negative samples can live next to the checker they pin.

    python tools/check_install_export_self_test.py

Exit status: 0 when every classification is right, 1 otherwise.
"""

from __future__ import annotations

import sys

from check_install_export import (
    TEMPLATE,
    INSTALL,
    check_all,
    check_hardcoded_paths,
    extract_configure_call,
    extract_placeholders,
)


def main() -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str) -> None:
        print(("PASS  " if cond else "FAIL  ") + what)
        if not cond:
            failures.append(what)

    template_text = TEMPLATE.read_text(encoding="utf-8")
    install_text = INSTALL.read_text(encoding="utf-8")

    # 1. The real tree must pass (criterion 12 / 17 / 25).
    problems, _notes = check_all(template_text, install_text)
    for p in problems:
        print("        " + p)
    expect(not problems,
           "the real NimRTCConfig.cmake.in passes every check")

    call = extract_configure_call(install_text)
    expect(call is not None,
           "cmake/NimRTCInstall.cmake has a parseable configure_package_config_file()")
    if call is not None:
        path_vars = call.get("path_vars", [])
        expect("CMAKE_INSTALL_INCLUDEDIR" in path_vars
               and "CMAKE_INSTALL_LIBDIR" in path_vars,
               "PATH_VARS carries CMAKE_INSTALL_INCLUDEDIR / CMAKE_INSTALL_LIBDIR")

    # 2. The known placeholders are all present in the real template.
    names = extract_placeholders(template_text)
    for expected in ("PACKAGE_INIT", "PROJECT_VERSION",
                     "PACKAGE_CMAKE_INSTALL_INCLUDEDIR",
                     "PACKAGE_CMAKE_INSTALL_LIBDIR"):
        expect(expected in names,
               f"real template carries placeholder @{expected}@")

    # 3. A `@…@` inside a comment is not a placeholder.
    commented = ("# example, not live: @MISSING_VARIABLE@\n"
                 "set(NIMRTC_VERSION \"@PROJECT_VERSION@\")\n")
    expect(extract_placeholders(commented) == ["PROJECT_VERSION"],
           "a @…@ inside a comment is not a placeholder")

    # 4. Negative: a template that drops a placeholder MUST be reported
    #    (criterion 13) — the installed config would carry an empty string.
    bad_template = (
        "@PACKAGE_INIT@\n"
        "set_and_check(NimRTC_INCLUDE_DIR \"@PACKAGE_CMAKE_INSTALL_INCLUDEDIR@\")\n"
        "set(NIMRTC_VERSION \"@PROJECT_VERSION@\")\n"
        "set(NIMRTC_HOST_PREFIX \"@MISSING_VARIABLE@\")\n")
    problems, _notes = check_all(bad_template, install_text)
    expect(any("MISSING_VARIABLE" in p for p in problems),
           "a template that drops a placeholder is reported")

    # 5. Negative: a GNUInstallDirs variable referenced directly (not via
    #    PATH_VARS -> @PACKAGE_<v>@) is reported — it bakes an absolute path.
    direct_libdir = (
        "@PACKAGE_INIT@\n"
        "set(NimRTC_LIB_DIR \"@CMAKE_INSTALL_LIBDIR@\")\n")
    problems, _notes = check_all(direct_libdir, install_text)
    expect(any("CMAKE_INSTALL_LIBDIR" in p for p in problems),
           "a direct @CMAKE_INSTALL_LIBDIR@ reference is reported")

    # 6. Negative: a hard-coded lib64 / absolute prefix is reported (criterion 17).
    hardcoded = 'find_library(_x NAMES foo PATHS "/usr/local/lib64" NO_DEFAULT_PATH)\n'
    problems = check_hardcoded_paths(hardcoded)
    expect(any("lib64" in p for p in problems),
           "a hard-coded lib64 path is reported")
    expect(any("absolute" in p for p in problems),
           "an absolute /usr/local path is reported")

    # 7. Positive: a normal synthetic template (both PATH_VARs used) is clean.
    good_template = (
        "@PACKAGE_INIT@\n"
        "set_and_check(NimRTC_INCLUDE_DIR \"@PACKAGE_CMAKE_INSTALL_INCLUDEDIR@\")\n"
        "set_and_check(NimRTC_LIB_DIR \"@PACKAGE_CMAKE_INSTALL_LIBDIR@\")\n"
        "set(NIMRTC_VERSION \"@PROJECT_VERSION@\")\n")
    problems, notes = check_all(good_template, install_text)
    expect(not problems and not notes,
           "a normal template (both PATH_VARs used) is not reported")

    # 8. Positive: an unused PATH_VAR is a note, not a problem.
    underused = (
        "@PACKAGE_INIT@\n"
        "set_and_check(NimRTC_INCLUDE_DIR \"@PACKAGE_CMAKE_INSTALL_INCLUDEDIR@\")\n"
        "set(NIMRTC_VERSION \"@PROJECT_VERSION@\")\n")
    problems, notes = check_all(underused, install_text)
    expect(not problems,
           "an unused PATH_VAR does not fail the check")
    expect(any("redundant PATH_VARS" in n for n in notes),
           "an unused PATH_VAR is surfaced as a note")

    if failures:
        print(f"\n{len(failures)} self-test failure(s).")
        return 1
    print("\nPASS -- the self-test confirms every classification.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
