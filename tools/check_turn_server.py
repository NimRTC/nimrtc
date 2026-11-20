#!/usr/bin/env python3
"""Check the reference TURN server's deployment against coturn's real options.

Scope: ``examples/turn-server/`` only.  The directory is a *deployment*,
not a library -- nothing in it is compiled, and nothing in CI runs it
yet (v1.4 plan item **N5**; the CI leg that uses it is **N5-2**).  So
the deployment has no compiler to catch a mistake in it and no test to
catch a mistake in it.  This check is that test.

The failures it guards are not hypothetical.  Each rule below names a
defect that was actually in this directory, and each one fails *silently*
at runtime -- the container comes up, the port is published, and the
thing the deployment exists to prove is not true:

1.  ``--tls-listening-port`` appears twice.  coturn parses the config
    file first and the command line second, and the command line
    *replaces* the value rather than adding a listener.  A second
    occurrence therefore **moves the TLS listener off the port the
    client was told to use** and leaves that port serving plain TCP --
    which is exactly the defect N1 exists to rule out.

2.  A config line names an option coturn does not have.  coturn's
    config reader logs "Bad configuration format" and **continues**, so
    the file looks like it is enforcing something it is not enforcing.
    (``no-dtlsv1`` / ``no-dtlsv1_1`` were the real examples: coturn has
    one DTLS switch, not a version ladder.)

3.  A config file carries a machine-specific absolute path.  The WSL
    variant pointed at one developer's ``D:\\MyOpen\\NimRTC``, so on any
    other clone the certificate path does not exist -- and coturn's
    response to a missing certificate is to **start anyway with the TLS
    listener disabled** (see rule 4).

4.  The certificate's private key is not readable by the user the
    server runs as.  OpenSSL writes keys 0600; the coturn image runs as
    ``nobody``.  The key is then unreadable, and coturn again starts
    with TLS disabled -- silently.

5.  The log directory is not writable by the server's user, so the log
    line that is N1's *evidence* never lands.

Usage:
    python tools/check_turn_server.py [--verbose]
"""

from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, "examples", "turn-server")

CONF = os.path.join(DIR, "turnserver.conf")
WSL_CONF = os.path.join(DIR, "turnserver.wsl.conf")
COMPOSE = os.path.join(DIR, "docker-compose.yml")
GEN_CERTS = os.path.join(DIR, "gen-certs.sh")
RUN_NATIVE = os.path.join(DIR, "run-native.sh")
README = os.path.join(DIR, "README.md")


def read(path: str) -> list[str]:
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read().split("\n")


def strip_comments(lines: list[str]) -> list[str]:
    """Drop whole-line comments and blank lines.

    Only whole-line comments are dropped: a trailing ``#`` on a value
    line is not a coturn comment (coturn's own config reader does not
    strip them), and the option-name rules are about the *name*.
    """
    out = []
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        out.append(stripped)
    return out


# The options coturn 4.6.3 actually has.  Taken from the long_options
# table in src/apps/relay/mainrelay.c, plus the single-character
# spellings the config reader accepts.
KNOWN_OPTIONS = {
    "CA-file",
    "Verbose",
    "acme-redirect",
    "allocation-default-address-family",
    "allow-loopback-peers",
    "allowed-peer-ip",
    "alt-listening-port",
    "alt-tls-listening-port",
    "alternate-server",
    "aux-server",
    "bps-capacity",
    "cert",
    "channel-lifetime",
    "check-origin-consistency",
    "cipher-list",
    "cli-ip",
    "cli-max-output-sessions",
    "cli-password",
    "cli-port",
    "daemon",
    "db",
    "denied-peer-ip",
    "dh-file",
    "dh1066",
    "dh566",
    "ec-curve-name",
    "external-ip",
    "fingerprint",
    "keep-address-family",
    "listening-device",
    "listening-ip",
    "listening-port",
    "log-binding",
    "log-file",
    "lt-cred-mech",
    "max-allocate-lifetime",
    "max-allocate-timeout",
    "max-bps",
    "max-port",
    "min-port",
    "mobility",
    "mongo-userdb",
    "mysql-userdb",
    "ne",
    "new-log-timestamp",
    "new-log-timestamp-format",
    "no-auth",
    "no-auth-pings",
    "no-cli",
    "no-dtls",
    "no-dynamic-ip-list",
    "no-dynamic-realms",
    "no-multicast-peers",
    "no-rfc5780",
    "no-software-attribute",
    "no-sslv2",
    "no-sslv3",
    "no-stdout-log",
    "no-stun",
    "no-stun-backward-compatibility",
    "no-tcp",
    "no-tcp-relay",
    "no-tls",
    "no-tlsv1",
    "no-tlsv1_1",
    "no-tlsv1_2",
    "no-udp",
    "no-udp-relay",
    "oauth",
    "permission-lifetime",
    "pidfile",
    "pkey",
    "pkey-pwd",
    "proc-group",
    "proc-user",
    "prod",
    "prometheus",
    "prometheus-address",
    "prometheus-port",
    "prometheus-username-labels",
    "psql-userdb",
    "raw-public-keys",
    "realm",
    "redis-statsdb",
    "redis-userdb",
    "relay-device",
    "relay-ip",
    "relay-threads",
    "respond-http-unsupported",
    "response-origin-only-with-rfc5780",
    "rest-api-separator",
    "secret-key-file",
    "secret-ts-exp-time",
    "secure-stun",
    "server-name",
    "server-relay",
    "simple-log",
    "sql-userdb",
    "stale-nonce",
    "static-auth-secret",
    "stun-only",
    "syslog",
    "syslog-facility",
    "tcp-proxy-port",
    "tls-alternate-server",
    "tls-listening-port",
    "total-quota",
    "udp-self-balance",
    "use-auth-secret",
    "user",
    "user-quota",
    "userdb",
    "verbose",
    "version",
    "web-admin",
    "web-admin-ip",
    "web-admin-listen-on-workers",
    "web-admin-port",
}

# The single-character options the config reader accepts, in the form
# "=<letter>" (the reader matches the option's *name*).
SINGLE_CHAR_OPTIONS = {
    "A",
    "B",
    "C",
    "E",
    "J",
    "K",
    "L",
    "M",
    "N",
    "O",
    "Q",
    "S",
    "V",
    "X",
    "a",
    "b",
    "d",
    "e",
    "f",
    "i",
    "l",
    "m",
    "o",
    "p",
    "q",
    "r",
    "s",
    "u",
    "v",
    "z",
}


def option_name(line: str) -> str | None:
    """The option a config line names, or None if it is not an option."""
    name = line.split("=", 1)[0].strip()
    if not name or " " in name:
        return None
    return name


class Report:
    def __init__(self, verbose: bool) -> None:
        self.failures: list[str] = []
        self.verbose = verbose

    def fail(self, where: str, message: str) -> None:
        self.failures.append(f"{where}: {message}")

    def note(self, message: str) -> None:
        if self.verbose:
            print(f"  {message}")


def check_config_options(report: Report, path: str, label: str) -> None:
    """Every option a config file names must exist in coturn."""
    for lineno, line in enumerate(read(path), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        name = option_name(stripped)
        if name is None:
            continue
        if name in KNOWN_OPTIONS or name in SINGLE_CHAR_OPTIONS:
            continue
        report.fail(
            f"{label}:{lineno}",
            f"coturn has no option '{name}' -- the line is discarded with a "
            f"'Bad configuration format' warning, so the file claims something "
            f"it does not enforce",
        )


def check_tls_port_is_single(report: Report) -> None:
    """The TLS listener's port must be named in exactly one place."""
    conf = [l for l in strip_comments(read(CONF)) if l.startswith("tls-listening-port")]
    if len(conf) != 1:
        report.fail(
            "turnserver.conf",
            f"expected exactly one 'tls-listening-port' line, found {len(conf)}",
        )

    compose = read(COMPOSE)
    occurrences = [
        (i, l.strip())
        for i, l in enumerate(compose, start=1)
        if "--tls-listening-port" in l and not l.strip().startswith("#")
    ]
    if occurrences:
        for lineno, line in occurrences:
            report.fail(
                f"docker-compose.yml:{lineno}",
                f"'{line}' -- coturn parses the config file first and the "
                f"command line second, so a command-line "
                f"'--tls-listening-port' REPLACES the config file's value "
                f"instead of adding a listener: the TLS listener moves off "
                f"5349 and that port falls back to plain TCP, which is the "
                f"defect N1 exists to rule out",
            )


def check_no_machine_paths(report: Report) -> None:
    """A committed config must not carry one machine's absolute paths."""
    # A Windows drive letter, or a /mnt/<drive>/ WSL path.  The WSL
    # variant is allowed to carry a *placeholder*, which is what
    # run-native.sh substitutes.
    windows = re.compile(r"[A-Za-z]:[\\/]")
    wsl = re.compile(r"/mnt/[a-zA-Z]/")
    for path, label in ((CONF, "turnserver.conf"), (WSL_CONF, "turnserver.wsl.conf")):
        for lineno, line in enumerate(read(path), start=1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if windows.search(stripped):
                report.fail(
                    f"{label}:{lineno}",
                    f"a machine-specific absolute path ('{stripped}') -- the "
                    f"file is wrong on every other machine and in every other "
                    f"clone, and a missing certificate path makes coturn start "
                    f"with the TLS listener silently disabled",
                )
            if wsl.search(stripped):
                report.fail(
                    f"{label}:{lineno}",
                    f"a machine-specific absolute path ('{stripped}') -- see "
                    f"the note about @TURN_ROOT@ in the file's header",
                )


def check_key_permissions(report: Report) -> None:
    """The private key must be readable by the user the server runs as."""
    lines = read(GEN_CERTS)
    text = "\n".join(lines)

    # The chmod that matters is the one on the *key*.  Matching on the
    # word "chmod 0644" anywhere in the file would also match the
    # certificate's chmod, and would then pass even when the key's own
    # chmod was gone -- which is the defect, and the check has to fail
    # on it.
    key_chmod = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if "chmod" in stripped and "$key" in stripped:
            key_chmod = True
            break
    if not key_chmod:
        report.fail(
            "gen-certs.sh",
            "the private key's mode is not widened -- OpenSSL writes keys "
            "0600 and the coturn image runs as 'nobody', so an unreadable "
            "key makes coturn start with the TLS listener disabled (silently)",
        )

    # The log directory has to be writable by that same user.
    logs_writable = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if "chmod" in stripped and "secrets/logs" in stripped:
            logs_writable = True
            break
    if not logs_writable:
        report.fail(
            "gen-certs.sh",
            "the log directory's permissions are not set -- the log line "
            "that is N1's evidence never lands if the server's user cannot "
            "write there",
        )


def check_compose_external_ip(report: Report) -> None:
    """Both services must advertise an external IP the peer can reach."""
    compose = read(COMPOSE)
    in_coturn = False
    for lineno, line in enumerate(compose, start=1):
        stripped = line.strip()
        if stripped.startswith("coturn:") or stripped.startswith("coturn-plain:"):
            in_coturn = True
            continue
        if in_coturn and "--external-ip" in stripped and not stripped.startswith("#"):
            report.note(f"docker-compose.yml:{lineno} carries an external IP")
            break
    else:
        report.fail(
            "docker-compose.yml",
            "no service passes '--external-ip' -- overriding 'command' "
            "replaces the image's default CMD wholesale, so without it the "
            "server advertises its container address (172.x.x.x), which no "
            "peer can route to: every relayed session would be established "
            "but dead",
        )


def check_evidence_is_documented(report: Report) -> None:
    """The README must say how to tell TLS from plain in the log."""
    text = "\n".join(read(README))
    for needle, why in (
        ("cipher=", "the log line that names the TLS cipher and version"),
        ("method=TLSv", "the TLS version on that line"),
    ):
        if needle not in text:
            report.fail(
                "README.md",
                f"it does not document '{needle}' -- {why} is how a reader "
                f"tells a TLS session from a plain one, and its absence is "
                f"how the failure goes unnoticed",
            )


def check_wsl_placeholder_is_substituted(report: Report) -> None:
    """The WSL variant's placeholder must be filled by the script."""
    if not os.path.isfile(RUN_NATIVE):
        report.fail(
            "run-native.sh",
            "it is missing -- the WSL config carries a placeholder and "
            "coturn does not expand environment variables, so something "
            "has to do the substitution",
        )
        return
    text = "\n".join(read(RUN_NATIVE))
    if "@TURN_ROOT@" not in text:
        report.fail(
            "run-native.sh",
            "it does not substitute the @TURN_ROOT@ placeholder that "
            "turnserver.wsl.conf carries",
        )
    if not os.path.isfile(WSL_CONF):
        report.fail("turnserver.wsl.conf", "it is missing")
        return
    text = "\n".join(read(WSL_CONF))
    if "@TURN_ROOT@" not in text:
        report.fail(
            "turnserver.wsl.conf",
            "it carries no @TURN_ROOT@ placeholder -- the paths it needs "
            "are machine-specific and must not be written down",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    for path in (CONF, WSL_CONF, COMPOSE, GEN_CERTS, README):
        if not os.path.isfile(path):
            print(f"check_turn_server: missing {path}", file=sys.stderr)
            return 2

    report = Report(args.verbose)
    check_config_options(report, CONF, "turnserver.conf")
    check_config_options(report, WSL_CONF, "turnserver.wsl.conf")
    check_tls_port_is_single(report)
    check_no_machine_paths(report)
    check_key_permissions(report)
    check_compose_external_ip(report)
    check_evidence_is_documented(report)
    check_wsl_placeholder_is_substituted(report)

    if report.failures:
        print("check_turn_server: FAILED", file=sys.stderr)
        for failure in report.failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print("check_turn_server: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
