#!/usr/bin/env python3
"""
scripts/generate_wtserver_cert.py
=================================
Generate a self-signed localhost PFX certificate for the demo-wtserver
TLS handshake (QUIC / RFC 9001).

Uses Python's `cryptography` library (no OpenSSL needed on Windows).

Run once during the build (or manually):
    python scripts/generate_wtserver_cert.py

Outputs (all under <project_root>/build/, auto-created):
    wtserver_cert.pfx   — PFX blob for msquic SChannel
    wtserver_cert.pem   — PEM certificate (msquic OpenSSL/quictls backend)
    wtserver_key.pem    — PEM private key (msquic OpenSSL/quictls backend)
    wtserver_hash.txt   — lowercase SHA-1 hex (matches .ps1 output)
    wtserver_thumbprint.txt — uppercase SHA-1 hex (matches .ps1 output)

Both .txt files share the format used by the PowerShell sibling so the
two scripts are interchangeable — pick whichever is available on the host
(PowerShell is preferred on Windows because the cert is also installed in
the `Cert:\CurrentUser\My` store for msquic SChannel QUIC_CREDENTIAL_TYPE_
CERTIFICATE_HASH_STORE).

On Linux and macOS the msquic OpenSSL / quictls backend loads the
certificate through QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE, which needs a
PEM private key next to the PEM certificate — that is what
`wtserver_key.pem` is for.  On Windows the SChannel backend instead
needs the certificate inside the Windows certificate store, so the
PowerShell sibling is the script to use there.
"""

import argparse
import ipaddress
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR   = os.path.dirname(SCRIPT_DIR)  # NimRTC project root
BUILD_DIR  = os.path.join(ROOT_DIR, "build")
OUT_PFX    = os.path.join(BUILD_DIR, "wtserver_cert.pfx")
OUT_PEM    = os.path.join(BUILD_DIR, "wtserver_cert.pem")
OUT_KEY    = os.path.join(BUILD_DIR, "wtserver_key.pem")
PFX_PASSWORD = "nimrtc-wtserver"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate localhost PFX cert for demo-wtserver")
    parser.add_argument("--days", type=int, default=365)
    parser.add_argument("--key-size", type=int, default=2048)
    parser.add_argument("--out-pfx", default=OUT_PFX)
    parser.add_argument("--out-pem", default=OUT_PEM)
    parser.add_argument("--out-key", default=OUT_KEY)
    parser.add_argument("--out-hash", default=None,
                        help="path for the lowercase SHA-1 hex file "
                             "(default: next to --out-pfx)")
    parser.add_argument("--out-thumbprint", default=None,
                        help="path for the uppercase SHA-1 hex file "
                             "(default: next to --out-pfx)")
    args = parser.parse_args()

    try:
        from cryptography import x509
        from cryptography.x509.oid import NameOID, ExtensionOID
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        # pkcs12 is a sub-module of serialization and is NOT imported by
        # `from ... import serialization`.  Without this explicit import
        # `serialization.pkcs12` raises AttributeError on cryptography
        # 3.x (the version pip installs on Linux/macOS), and the script
        # dies before it writes the SHA-1 hash files.
        from cryptography.hazmat.primitives.serialization import pkcs12
        from datetime import datetime, timedelta
    except ImportError:
        print("ERROR: cryptography package not found.", file=sys.stderr)
        print("Install it with: pip install cryptography", file=sys.stderr)
        return 1

    os.makedirs(BUILD_DIR, exist_ok=True)

    # --- 1. Generate RSA key ---
    print(f"Generating RSA {args.key_size}-bit key...")
    private_key = rsa.generate_private_key(
        public_exponent=65537,
        key_size=args.key_size,
    )
    public_key = private_key.public_key()

    # --- 2. Build X.509 certificate ---
    print("Building X.509 certificate...")
    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "localhost"),
    ])

    now = datetime.utcnow()
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(public_key)
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + timedelta(days=args.days))
        .add_extension(
            x509.SubjectAlternativeName([
                x509.DNSName("localhost"),
                x509.IPAddress(ipaddress.IPv4Address("127.0.0.1")),
            ]),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None),
            critical=True,
        )
        .sign(private_key, hashes.SHA256())
    )
    print(f"  Certificate valid for {args.days} days: {now.date()} → "
          f"{(now + timedelta(days=args.days)).date()}")

    # --- 3. Export PEM (certificate) ---
    pem_bytes = cert.public_bytes(serialization.Encoding.PEM)
    with open(args.out_pem, "wb") as f:
        f.write(pem_bytes)
    print(f"  PEM written: {args.out_pem}  ({len(pem_bytes):,} bytes)")

    # --- 3b. Export PEM private key (unencrypted) ---
    # The msquic OpenSSL / quictls backend (Linux, macOS) loads the server
    # certificate through QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE, which needs
    # the private key in its own PEM file next to the certificate PEM.
    key_bytes = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )
    with open(args.out_key, "wb") as f:
        f.write(key_bytes)
    print(f"  Key PEM written: {args.out_key}  ({len(key_bytes):,} bytes)")

    # --- 4. Export PFX ---
    # Windows SChannel / msquic expects a PFX blob.
    # We include the private key so SChannel can use it for TLS.
    #
    # The keyword is `key_encryption_algorithm` from cryptography 37.0
    # onwards; older releases (3.x — what pip installs on many Linux and
    # macOS images) spell it `encryption_algorithm`.  Probe the signature
    # instead of the version number so a backport of either spelling works.
    #
    # The PFX is a Windows-only artifact: the Linux / macOS msquic backend
    # is the OpenSSL/quictls one, which loads the PEM pair and never
    # touches the PFX.  A failure here must therefore not abort the script
    # — the PEM pair and the SHA-1 hash files are what those platforms
    # need, and they are written below.
    try:
        import inspect
        pkcs12_params = inspect.signature(
            pkcs12.serialize_key_and_certificates).parameters
        enc_kwarg = ("key_encryption_algorithm"
                     if "key_encryption_algorithm" in pkcs12_params
                     else "encryption_algorithm")
        pfx_bytes = pkcs12.serialize_key_and_certificates(
            name=b"nimrtc-wtserver",
            key=private_key,
            cert=cert,
            cas=None,
            **{enc_kwarg: serialization.BestAvailableEncryption(
                PFX_PASSWORD.encode())},
        )
        with open(args.out_pfx, "wb") as f:
            f.write(pfx_bytes)
        print(f"  PFX written: {args.out_pfx}  ({len(pfx_bytes):,} bytes)")
    except Exception as exc:  # noqa: BLE001 - non-fatal, see comment above
        print(f"  PFX not written ({exc}); "
              "PFX is only required by the Windows SChannel backend — "
              "the PEM pair below is what Linux / macOS use.")

    # --- 5. Export SHA-1 hash + thumbprint (matches .ps1 sibling) ---
    # msquic SChannel uses QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE which
    # references the cert by its SHA-1 thumbprint in a Windows cert store.
    # lowercase hex → wtserver_hash.txt (C++ reads this via NIMRTC_BUILD_ROOT)
    # uppercase hex → wtserver_thumbprint.txt (matches `New-SelfSignedCertificate`)
    # The hash / thumbprint files describe the certificate that was just
    # written, so they follow the certificate's own output directory
    # instead of the hard-coded BUILD_DIR.  Without this, running the
    # script from a WSL / Linux host against a shared checkout overwrote
    # the Windows SChannel hash file with a cert that is not installed in
    # Cert:\CurrentUser\My — the Windows seam tests then failed to load
    # their TLS credentials even though the material was "present".
    out_dir = os.path.dirname(os.path.abspath(args.out_pfx))
    os.makedirs(out_dir, exist_ok=True)
    hash_path = args.out_hash or os.path.join(out_dir, "wtserver_hash.txt")
    thumb_path = args.out_thumbprint or os.path.join(
        out_dir, "wtserver_thumbprint.txt")
    sha1_hex = cert.fingerprint(hashes.SHA1()).hex()
    with open(hash_path, "w") as f:
        f.write(sha1_hex)
    with open(thumb_path, "w") as f:
        f.write(sha1_hex.upper())
    print(f"  SHA1 hash:      {hash_path}  ({sha1_hex})")
    print(f"  Thumbprint:     {thumb_path}  ({sha1_hex.upper()})")

    print("Done.")
    return 0


if __name__ == "__main__":
    # ipaddress was imported at module top; stdlib import only for clarity here.
    sys.exit(main())
