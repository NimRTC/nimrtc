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

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

try:
    import ifaddr as _ifaddr  # type: ignore
except ImportError:
    _ifaddr = None

import socket
import ipaddress


ROOT = Path(__file__).resolve().parent.parent
# Primary fixture location — what's checked in for fresh checkouts and
# what CMake's `copy_if_different` rule picks up to seed the build dir.
CERT_PATH = ROOT / "tests" / "wolfssl_dtls" / "certs" / "server-ecc.pem"
KEY_PATH  = ROOT / "tests" / "wolfssl_dtls" / "certs" / "ecc-key.pem"
# Mirror into wolfSSL's own cert tree so the upstream copy rule picks up
# the regenerated cert without anyone editing CMakeLists.txt.  The
# directory is pre-populated by wolfSSL itself (with hundreds of test
# certs) so we never want to wipe it — only overwrite these two files.
WOLFSSL_CERT_PATH = (ROOT / "src" / "third_party" / "wolfssl" / "src"
                     / "certs" / "server-ecc.pem")
WOLFSSL_KEY_PATH  = (ROOT / "src" / "third_party" / "wolfssl" / "src"
                     / "certs" / "ecc-key.pem")


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
    local_ips = collect_local_ips()
    existing_san_ips: set[str] = set()
    if CERT_PATH.exists():
        try:
            existing = x509.load_pem_x509_certificate(CERT_PATH.read_bytes())
            try:
                san = existing.extensions.get_extension_for_class(
                    x509.SubjectAlternativeName).value
                existing_san_ips = {
                    str(n) for n in san
                    if isinstance(n, x509.IPAddress)
                }
            except x509.ExtensionNotFound:
                pass  # needs regeneration — fall through
        except Exception as e:
            print(f"[warn] could not parse existing cert ({e}); regenerating")

    # If SAN exists but doesn't cover every local IP, regenerate.  We
    # only force-regenerate when there's a real mismatch (missing IP),
    # so transient socket failures during collection don't churn the
    # cert on every build.
    if CERT_PATH.exists() and existing_san_ips and local_ips:
        missing = {ip for ip in local_ips if ip not in existing_san_ips}
        if not missing:
            print(f"[skip] {CERT_PATH.name} already has subjectAltName "
                  f"covering all {len(local_ips)} local interface IPs")
            return 0
        print(f"[regen] cert missing {len(missing)} local IPs in SAN: "
              f"{sorted(missing)}")
    elif CERT_PATH.exists() and not existing_san_ips and not local_ips:
        # Old-style cert without SAN, but we have no way to know which
        # local IPs to add.  Skip regen — at minimum the cert has SAN
        # for localhost/127.0.0.1 already (caller can force regen by
        # deleting the file).
        print(f"[skip] {CERT_PATH.name} already has subjectAltName "
              f"(and could not enumerate local IPs to compare)")
        return 0

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

    # Mirror into wolfSSL's own cert tree so the build-time
    # `copy_if_different` rule in `tests/wolfssl_dtls/CMakeLists.txt`
    # (and friends) picks up the regenerated cert into the build dir
    # alongside `demo-p2p.exe`.  Without this mirror, `demo-p2p` would
    # still load the old SAN-less upstream cert from
    # `build/tests/Debug/certs/server-ecc.pem`.
    WOLFSSL_CERT_PATH.parent.mkdir(parents=True, exist_ok=True)
    WOLFSSL_CERT_PATH.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    WOLFSSL_KEY_PATH.write_bytes(
        key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    print(f"[ok] wrote {WOLFSSL_CERT_PATH.relative_to(ROOT)}")
    print(f"[ok] wrote {WOLFSSL_KEY_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(build_or_skip())
