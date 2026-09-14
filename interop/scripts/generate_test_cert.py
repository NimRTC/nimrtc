#!/usr/bin/env python3
"""Generate a fixed self-signed ECDSA P-256 test cert + key for Chrome interop.

Output: ../test_data/dtls_test_cert.pem (cert) and dtls_test_key.pem (key).

Both NimRTC and Chrome must use the SAME cert so that:
- SDP a=fingerprint matches on both sides (Chrome computes fingerprint from
  the RTCCertificate it sends; NimRTC computes it from its loaded cert's SPKI)
- BoringSSL's DTLS layer accepts the peer's cert without PKI chain failure:
  Chrome's RTCCertificate is treated as trusted during the DTLS handshake.

This is a TEST-ONLY cert; do NOT use it in production. The corresponding files
in interop/test_data/ are deliberately checked into the test tree so Chrome and
NimRTC always start from the same SPKI without runtime coordination.
"""

import datetime
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def main(out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    cert_path = out_dir / "dtls_test_cert.pem"
    key_path = out_dir / "dtls_test_key.pem"

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name(
        [
            x509.NameAttribute(NameOID.COMMON_NAME, "NimRTC DTLS interop test"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "NimRTC"),
        ]
    )
    now = datetime.datetime(2024, 1, 1, tzinfo=datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None), critical=True
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=False,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.ExtendedKeyUsage([x509.ExtendedKeyUsageOID.CLIENT_AUTH]),
            critical=True,
        )
        .sign(key, hashes.SHA256())
    )

    cert_pem = cert.public_bytes(serialization.Encoding.PEM)
    key_pem = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )

    cert_path.write_bytes(cert_pem)
    key_path.write_bytes(key_pem)

    # Print the SPKI SHA-256 fingerprint (the value both peers will put in
    # SDP a=fingerprint:sha-256 <colon-hex>) for sanity-check / log output.
    spki = cert.public_key().public_bytes(
        encoding=serialization.Encoding.DER,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    digest = hashes.Hash(hashes.SHA256())
    digest.update(spki)
    fp_bytes = digest.finalize()
    fp_hex = ":".join(f"{b:02X}" for b in fp_bytes)

    print(f"wrote {cert_path} ({len(cert_pem)} bytes)")
    print(f"wrote {key_path} ({len(key_pem)} bytes)")
    print(f"SPKI SHA-256 = {fp_hex}")
    return 0


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).resolve().parent.parent / "test_data"
    )
    sys.exit(main(out))
