"""
scripts/build_dtls_cert.py — Generate a Chrome-friendly self-signed ECC cert
for NimRTC DTLS interop.

Why this exists:
  The bundled wolfSSL `server-ecc.pem` is missing `subjectAltName`. Modern
  Chrome (BoringSSL, M131+) rejects such a cert during the DTLS handshake
  with a fatal alert 46 ("certificate_unknown") because RFC 5280 requires
  CA:FALSE end-entity certs to carry SAN.

  Rather than re-licence wolfSSL's X509 builder (which doesn't expose a
  SAN setter), we generate the cert at build time with `cryptography` and
  install it at `tests/wolfssl_dtls/certs/server-ecc.pem` if newer. The
  resulting cert has:
    - secp256r1 (P-256) key
    - 10-year validity
    - Subject  : CN=localhost (we don't tie it to a real hostname; SDP
                 `a=fingerprint` is the WebRTC trust anchor)
    - SAN      : DNS:localhost, DNS:127.0.0.1, IP:127.0.0.1
    - EKU      : serverAuth
    - KeyUsage : DigitalSignature, KeyAgreement
    - BasicConstraints CA:FALSE
    - SKI      : derived from SPKI (Chrome doesn't require AKI)

Usage (regenerate manually):
    python scripts/build_dtls_cert.py
"""

from __future__ import annotations

import datetime
import os
import sys
from pathlib import Path

# `cryptography` is imported where it is used, not here, so importing
# this module is free: `generate_dtls_certs_ci.py` imports it to ask
# whether the pair is present and current *before* it decides a pip
# install is needed, and an import that dies on a missing dependency
# cannot answer that question.
#
# The import is `cryptography`, but the wheel is `<42` on purpose: 42+
# ships a Rust-backed Windows wheel whose `cryptography_rust.dll` is
# not in the PyPI artefact, so `import cryptography` fails with "DLL
# load failed while importing _rust".  41.x is the last pure-C line.
try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
    HAVE_CRYPTOGRAPHY = True
except ImportError:  # pragma: no cover - depends on the host's site-packages
    x509 = None
    HAVE_CRYPTOGRAPHY = False

try:
    import ifaddr as _ifaddr  # type: ignore
except ImportError:
    _ifaddr = None

import socket
import ipaddress


ROOT = Path(__file__).resolve().parent.parent
# The generated pair lives HERE and only here.
#
# Why not also mirror into wolfSSL's own cert tree (`src/third_party/wolfssl/
# src/certs/`)?  That tree is a **git submodule**, and the two files behave
# differently inside it:
#
#   * `certs/ecc-key.pem`  is in wolfSSL's .gitignore (line 109) → writing it
#     leaves the submodule clean;
#   * `certs/server-ecc.pem` is **tracked** upstream → writing it makes the
#     submodule report `-dirty`, and the whole main repo then shows
#     `m src/third_party/wolfssl/src` forever.
#
# A build artefact has no business dirtying a vendored dependency's tracked
# files, so the mirror was removed (2026-11-22).  CMake now sources the pair
# from this directory instead — see cmake/NimRTCTest.cmake and
# cmake/NimRTCRuntime.cmake.
#
# This directory is itself gitignored (`.gitignore:227`), so the pair is a
# pure build artefact: regenerate it, never commit it.
CERT_PATH = ROOT / "tests" / "wolfssl_dtls" / "certs" / "server-ecc.pem"
KEY_PATH  = ROOT / "tests" / "wolfssl_dtls" / "certs" / "ecc-key.pem"


def collect_local_ips() -> list[str]:
    """Collect local IPv4 addresses that should appear in the cert SAN.

    DTLS-SRTP (Chrome's BoringSSL verifier) requires the connecting peer's
    IP to appear in the cert's Subject Alternative Name or it rejects the
    handshake with fatal alert 46 ("certificate_unknown").  SDP
    `a=fingerprint` is the WebRTC trust anchor, so SAN is purely used
    for the BoringSSL chain-validation pass on the actual connection IP
    — and Chrome's verifier checks that the IP of the selected ICE
    candidate pair matches one of the SAN entries before completing the
    DTLS handshake.

    Why we include every local interface:
      - On a single host with multiple NICs (this Windows test box has
        172.16.x.x, 172.25.x.x, 172.26.x.x, 10.x.x.x in addition to
        127.0.0.1) Chrome picks whichever ICE candidate pair survives
        connectivity checks; we don't know which one ahead of time.
      - Adding all "stable" (non-link-local, non-loopback) IPv4
        addresses means any of those interfaces works without
        regenerating the cert.

    Returns IPv4 strings, deduplicated, sorted (loopback last so SAN
    reads `localhost, 127.0.0.1, 172.x, 10.x, …`).
    """
    out: set[str] = set()
    if _ifaddr is not None:
        try:
            for iface in _ifaddr.get_adapters():
                for ip in iface.ips:
                    if ip.is_IPv4():
                        a = ip.ip[0]
                        # Skip loopback/link-local — handled separately
                        if a.startswith("169.254.") or a.startswith("127."):
                            continue
                        out.add(a)
        except Exception:
            pass
    # Fallback / belt-and-suspenders: also resolve via getaddrinfo.
    # This catches hosts where ifaddr is missing (test machines without
    # the extra package) or where the interfaces enumeration is empty
    # (containerised CI).  We deliberately skip link-local here too.
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None,
                                        family=socket.AF_INET):
            addr = info[4][0]
            if addr and not addr.startswith("169.254."):
                out.add(addr)
    except Exception:
        pass
    return sorted(out)


# A stale cert regenerates at most once per this many seconds.  See the
# comment in `build_or_skip()` for why the cap exists.
_STALE_REGEN_INTERVAL_SECONDS = 24 * 60 * 60


def _regenerated_within(seconds: float) -> bool:
    """True when the certificate file is younger than `seconds`."""
    try:
        return (datetime.datetime.now().timestamp()
                - CERT_PATH.stat().st_mtime) < seconds
    except OSError:
        return False


def verify_pair() -> tuple[int, str]:
    """(exit code, reason) -- is the existing pair usable as it is?

    0 / "current"      the pair exists, parses, carries SAN, and covers
                       every local IPv4 address.
    1 / "missing"      one or both files are absent.
    2 / "no-san"       the certificate exists but has no SAN, or cannot
                       be parsed.
    3 / "stale"        the SAN no longer covers every local IPv4 address
                       (a new NIC, VPN or Docker bridge came up).
    4 / "no-crypto"    `cryptography` is not importable, so the pair
                       cannot even be inspected.

    This is the whole decision `build_dtls_cert.py` makes, exposed as a
    function so `generate_dtls_certs_ci.py` can ask it with the
    interpreter that is about to build -- the *same* interpreter and
    the *same* site-packages -- instead of running pip on every CI job
    before finding out it was not needed.
    """
    if not HAVE_CRYPTOGRAPHY:
        return 4, "no-crypto"
    if not CERT_PATH.exists() or not KEY_PATH.exists():
        return 1, "missing"
    try:
        existing = x509.load_pem_x509_certificate(CERT_PATH.read_bytes())
    except Exception as e:
        return 2, "no-san (unparseable: %s)" % e
    try:
        san = existing.extensions.get_extension_for_class(
            x509.SubjectAlternativeName).value
    except x509.ExtensionNotFound:
        return 2, "no-san"
    # `str(x509.IPAddress(...))` is not the address.  An
    # `x509.GeneralName` renders its class and fields ("IPAddress
    # (value=10.0.0.1)"), while `str(n)` on *this* object has always
    # been the dotted address because `IPAddress` subclasses
    # `ipaddress.IPv4Address`.  Comparing the rendered form against a
    # collected address never matches, so every host read as "stale" --
    # the checked-in pair's SAN names every local IP and `verify_pair()`
    # still called it incomplete.  `n.value` is the address the SAN
    # actually carries.
    existing_san_ips = {
        str(n.value) for n in san if isinstance(n, x509.IPAddress)
    }
    local_ips = collect_local_ips()
    if local_ips and existing_san_ips:
        missing = sorted(ip for ip in local_ips if ip not in existing_san_ips)
        if missing:
            return 3, "stale (missing %d local IP(s) in SAN: %s)" % (
                len(missing), ", ".join(missing))
    return 0, "current"


def build_or_skip() -> int:
    """Build a new cert/key, but only overwrite the existing files if the
    existing cert is missing the subjectAltName extension (i.e. the old
    one).  This is safe to run on every build — if a developer has
    customised the cert for some reason and it's already Chrome-friendly,
    we leave it alone.

    Additionally, force regeneration whenever the SAN set is missing one
    of the current host's local IPv4 addresses — a previously-good cert
    becomes stale the moment a new NIC (or VPN, or Docker bridge) is
    brought up, and Chrome will fail the DTLS handshake with alert 46
    on the new interface if the IP isn't in the SAN.
    """
    if not HAVE_CRYPTOGRAPHY:
        # Non-zero on purpose.  A build step that cannot generate the
        # pair must not read as success: the copy rule downstream then
        # has no source file, and CMake's own error names the missing
        # file.  The exit code is the one a CI caller can branch on.
        print("build_dtls_cert.py: the `cryptography` package is not "
              "importable; run `python -m pip install 'cryptography<42'`.",
              file=sys.stderr)
        return 2

    code, reason = verify_pair()
    if code == 0:
        print(f"[skip] {CERT_PATH.name} already has subjectAltName "
              f"covering all local interface IPs")
        return 0
    if code == 2 and "unparseable" in reason:
        print(f"[warn] could not parse existing cert ({reason}); regenerating")
    elif code == 2:
        # The "no SAN, and no local IPs to compare" case used to skip
        # here.  It now regenerates instead: the pair is only checked
        # into this directory as a *generated* file, and the old skip
        # left the checked-out stale pair in place -- which is what the
        # `nimrtc_dtls_certs` build edge then copied, silently.  A
        # certificate with no SAN is never what CMake wants to stage.
        print("[regen] the existing cert has no subjectAltName")
    elif code == 3:
        print(f"[regen] cert is {reason}")
        # A stale cert is regenerated at most once a day per host.  The
        # "is it stale?" test is deliberately generous -- *any* missing
        # local interface IP regenerates -- and on a Windows box the
        # interface list changes as DHCP leases come and go.  Without
        # this cap, the single `nimrtc_dtls_certs` target (which every
        # test target depends on) would regenerate the pair before every
        # build, and every test executable's POST_BUILD copy would see a
        # changed source on every incremental build.  Deleting the file
        # remains the way to force a regeneration, and a *missing*
        # cert is always regenerated at once.
        if _regenerated_within(_STALE_REGEN_INTERVAL_SECONDS):
            print(f"[skip] {CERT_PATH.name} was regenerated less than a day "
                  f"ago (delete it to force a regeneration)")
            return 0
        print(f"[regen] the pair on disk is older than a day and its SAN is "
              f"incomplete for this host; regenerating")
    elif code == 1:
        print("[regen] the pair is missing")

    local_ips = collect_local_ips()

    key = ec.generate_private_key(ec.SECP256R1())
    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "localhost"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "NimRTC"),
        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, "DTLS"),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)

    # ---- SAN: localhost + every detected local IPv4 address -------------
    san_entries: list = [
        x509.DNSName("localhost"),
        x509.DNSName("127.0.0.1"),
        x509.IPAddress(ipaddress.IPv4Address("127.0.0.1")),
    ]
    for ip in local_ips:
        san_entries.append(x509.IPAddress(ipaddress.IPv4Address(ip)))

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=3650))  # ~10y
        .add_extension(
            x509.SubjectAlternativeName(san_entries),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True, key_encipherment=False,
                content_commitment=False, key_agreement=True,
                key_cert_sign=False, crl_sign=False,
                data_encipherment=False, encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
            critical=False,
        )
        # Chrome M124+ BoringSSL DTLS-SRTP verifier requires Subject Key
        # Identifier on end-entity certs — without SKI the verifier
        # callback returns ssl_verify_invalid and Chrome closes the
        # handshake with alert 46 (certificate_unknown).  Authority Key
        # Identifier is added too so future Chrome versions that require
        # SKI==AKI self-pairing (Chrome M131+ tracked) don't break again.
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(key.public_key()),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )

    CERT_PATH.parent.mkdir(parents=True, exist_ok=True)
    CERT_PATH.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    KEY_PATH.write_bytes(
        key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    print(f"[ok] wrote {CERT_PATH.relative_to(ROOT)}")
    print(f"[ok] wrote {KEY_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(build_or_skip())
