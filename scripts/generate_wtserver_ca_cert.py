#!/usr/bin/env python3
"""
scripts/generate_wtserver_ca_cert.py
====================================
Generate a *local CA* plus a CA-signed ``localhost`` leaf certificate for
the demo-wtserver TLS (QUIC / RFC 9001) handshake.

Why a CA and not a single self-signed leaf
-------------------------------------------
Chrome (and Chromium in general) does **not** relax the certificate check
for WebTransport / QUIC sessions.  Both ``--ignore-certificate-errors``
and ``--ignore-certificate-errors-spki-list`` are honoured for classic
TLS but *not* for the QUIC handshake that WebTransport runs on, so a
plain self-signed leaf makes Chrome fail with::

    net::ERR_QUIC_PROTOCOL_ERROR.QUIC_TLS_CERTIFICATE_UNKNOWN
    (TLS handshake failure (ENCRYPTION_HANDSHAKE) 46: certificate unknown)

The only way to make Chrome trust a local WebTransport server is to let
the certificate chain up to a root that the browser already trusts.
This script therefore emits:

* a self-signed **CA** certificate (basicConstraints ``CA:TRUE``), and
* a **leaf** certificate for ``CN=localhost`` signed by that CA, carrying
  the SANs ``DNS:localhost``, ``IP:127.0.0.1``, ``IP:::1`` and the
  ``serverAuth`` EKU.

Install the CA into the Windows trust store (see
``scripts/install_wtserver_ca.ps1``): with the CA in the root store Chrome
accepts this certificate over classic TLS, and over QUIC when an
OpenSSL-based QUIC server presents it.

CAUTION (measured 2026-10-26, Chrome 155): the root-store install is
**necessary but not sufficient** for the NimRTC demo-wtserver.  The
msquic/SChannel listener presents the very same leaf (byte-identical, no
chain) and Chrome still aborts the QUIC handshake with
``QUIC_TLS_CERTIFICATE_UNKNOWN (46: certificate unknown)`` /
``CERTIFICATE_VERIFY_FAILED``, while an OpenSSL-based server presenting
the identical certificate is accepted.  The residual Chrome failure is
therefore in the SChannel TLS presentation, not in this certificate and
not in the trust store.

Outputs (all under ``<project_root>/build/``, auto-created):

    wtserver_ca_cert.pem   CA certificate (PEM) -- import into the root store
    wtserver_ca_key.pem    CA private key (PEM) -- keep out of the repo
    wtserver_ca.pfx        CA certificate + key (PFX)
    wtserver_cert.pem      leaf certificate (PEM) -- msquic OpenSSL backend
    wtserver_key.pem       leaf private key (PEM)  -- msquic OpenSSL backend
    wtserver_cert.pfx      leaf certificate + key (PFX) -- msquic SChannel
    wtserver_hash.txt      lowercase SHA-1 hex of the leaf (msquic HASH_STORE)
    wtserver_thumbprint.txt uppercase SHA-1 hex of the leaf

The two ``.txt`` files share the format used by
``scripts/generate_wtserver_cert.ps1`` so the two generators stay
interchangeable.

Usage
-----
    python scripts/generate_wtserver_ca_cert.py
    python scripts/generate_wtserver_ca_cert.py --days 397 --ca-days 3650
"""

from __future__ import annotations

import argparse
import ipaddress
import os
import sys
from datetime import datetime, timedelta, timezone

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)  # NimRTC project root
BUILD_DIR = os.path.join(ROOT_DIR, "build")

PFX_PASSWORD = "nimrtc-wtserver"
CA_SUBJECT_CN = "NimRTC Dev Root CA"
LEAF_SUBJECT_CN = "localhost"


def _write(path: str, data: bytes) -> None:
    with open(path, "wb") as fh:
        fh.write(data)
    print(f"  written: {path}  ({len(data):,} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a local CA + CA-signed localhost certificate "
                    "for demo-wtserver")
    parser.add_argument("--days", type=int, default=397,
                        help="leaf validity in days (default: 397)")
    parser.add_argument("--ca-days", type=int, default=3650,
                        help="CA validity in days (default: 3650)")
    parser.add_argument("--key-size", type=int, default=2048)
    parser.add_argument("--build-dir", default=BUILD_DIR)
    parser.add_argument("--password", default=PFX_PASSWORD)
    args = parser.parse_args()

    try:
        from cryptography import x509
        from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.hazmat.primitives.serialization import pkcs12
    except ImportError:
        print("ERROR: the 'cryptography' package is required.",
              file=sys.stderr)
        print("Install it with: pip install cryptography", file=sys.stderr)
        return 1

    out_dir = os.path.abspath(args.build_dir)
    os.makedirs(out_dir, exist_ok=True)

    now = datetime.now(timezone.utc)
    ca_not_after = now + timedelta(days=args.ca_days)
    leaf_not_after = now + timedelta(days=args.days)

    # 鈹€鈹€ 1. CA key + self-signed CA certificate 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    print(f"Generating CA key ({args.key_size} bit)...")
    ca_key = rsa.generate_private_key(public_exponent=65537,
                                      key_size=args.key_size)
    ca_name = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, CA_SUBJECT_CN),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "NimRTC"),
    ])
    ca_cert = (
        x509.CertificateBuilder()
        .subject_name(ca_name)
        .issuer_name(ca_name)
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(ca_not_after)
        .add_extension(x509.BasicConstraints(ca=True, path_length=0),
                       critical=True)
        .add_extension(x509.KeyUsage(
            digital_signature=True, content_commitment=False,
            key_encipherment=False, data_encipherment=False,
            key_agreement=False, key_cert_sign=True, crl_sign=True,
            encipher_only=False, decipher_only=False),
                       critical=True)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(
            ca_key.public_key()), critical=False)
        .sign(ca_key, hashes.SHA256())
    )
    print(f"  CA valid until {ca_not_after.date()}")

    # 鈹€鈹€ 2. Leaf key + CA-signed leaf certificate 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    print(f"Generating leaf key ({args.key_size} bit)...")
    leaf_key = rsa.generate_private_key(public_exponent=65537,
                                        key_size=args.key_size)
    leaf_name = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, LEAF_SUBJECT_CN),
    ])
    leaf_cert = (
        x509.CertificateBuilder()
        .subject_name(leaf_name)
        .issuer_name(ca_name)
        .public_key(leaf_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(leaf_not_after)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None),
                       critical=True)
        .add_extension(x509.KeyUsage(
            digital_signature=True, content_commitment=False,
            key_encipherment=True, data_encipherment=False,
            key_agreement=False, key_cert_sign=False, crl_sign=False,
            encipher_only=False, decipher_only=False),
                       critical=True)
        .add_extension(x509.ExtendedKeyUsage(
            [ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .add_extension(x509.SubjectAlternativeName([
            x509.DNSName("localhost"),
            x509.IPAddress(ipaddress.IPv4Address("127.0.0.1")),
            x509.IPAddress(ipaddress.IPv6Address("::1")),
        ]), critical=False)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(
            leaf_key.public_key()), critical=False)
        .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(
            ca_key.public_key()), critical=False)
        .sign(ca_key, hashes.SHA256())
    )
    print(f"  leaf valid until {leaf_not_after.date()}")

    # 鈹€鈹€ 3. Write PEM files 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    def pem_key(key) -> bytes:
        return key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )

    _write(os.path.join(out_dir, "wtserver_ca_cert.pem"),
           ca_cert.public_bytes(serialization.Encoding.PEM))
    _write(os.path.join(out_dir, "wtserver_ca_key.pem"), pem_key(ca_key))
    _write(os.path.join(out_dir, "wtserver_cert.pem"),
           leaf_cert.public_bytes(serialization.Encoding.PEM))
    _write(os.path.join(out_dir, "wtserver_key.pem"), pem_key(leaf_key))

    # 鈹€鈹€ 4. Write PFX files (Windows SChannel / msquic) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    import inspect
    params = inspect.signature(pkcs12.serialize_key_and_certificates).parameters
    enc_kwarg = ("key_encryption_algorithm"
                 if "key_encryption_algorithm" in params
                 else "encryption_algorithm")
    enc = serialization.BestAvailableEncryption(args.password.encode())
    _write(os.path.join(out_dir, "wtserver_cert.pfx"),
           pkcs12.serialize_key_and_certificates(
               name=b"nimrtc-wtserver", key=leaf_key, cert=leaf_cert,
               cas=[ca_cert], **{enc_kwarg: enc}))
    _write(os.path.join(out_dir, "wtserver_ca.pfx"),
           pkcs12.serialize_key_and_certificates(
               name=b"nimrtc-dev-root-ca", key=ca_key, cert=ca_cert,
               cas=None, **{enc_kwarg: enc}))

    # 鈹€鈹€ 5. SHA-1 hash / thumbprint of the leaf 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    sha1_hex = leaf_cert.fingerprint(hashes.SHA1()).hex()
    with open(os.path.join(out_dir, "wtserver_hash.txt"), "w") as fh:
        fh.write(sha1_hex)
    with open(os.path.join(out_dir, "wtserver_thumbprint.txt"), "w") as fh:
        fh.write(sha1_hex.upper())
    ca_sha1 = ca_cert.fingerprint(hashes.SHA1()).hex()
    print(f"  leaf SHA-1:      {sha1_hex}")
    print(f"  CA   SHA-1:      {ca_sha1}")

    print()
    print("Next step (Windows): import the CA into the trusted root store")
    print("so Chrome accepts the chain, and the leaf into the personal")
    print("store so msquic can load it by SHA-1 hash:")
    print("    powershell -File .\\scripts\\install_wtserver_ca.ps1")
    return 0


if __name__ == "__main__":
    sys.exit(main())

