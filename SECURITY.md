# Security Policy

## Reporting a vulnerability

NimRTC takes security seriously. If you discover a security vulnerability,
please report it responsibly.

**Please do NOT open a public GitHub issue.**

Send a detailed report to the maintainers via one of:

1. **GitHub Private Vulnerability Reporting** (preferred):
   Use the "Report a vulnerability" tab on the repository's Security tab.

2. **Email**: Send a description of the vulnerability and its impact to the
   maintainers listed in `CODEOWNERS`.

Include as much detail as possible:
- Type of vulnerability (buffer overflow, use-after-free, etc.)
- Steps to reproduce
- Potential impact
- Any suggested fixes (optional)

## Response timeline

| Timeline | Action |
|----------|--------|
| Within 24 hours | Acknowledge receipt and assign a severity label |
| Within 7 days | Provide initial assessment and expected fix timeline |
| Within 30 days | Publish a fix (if feasible) and a security advisory |

## Supported versions

Security fixes are applied to the current stable release and the most recent
patch release of the previous minor version.

| Version | Status |
|---------|--------|
| 0.1.x   | ⚠️ Not yet released — no security support |
| 0.2.x   | Not yet released |

## Security considerations in NimRTC

- **No network listeners by default**: NimRTC exposes no sockets until the
  caller explicitly creates them.
- **No dynamic code loading**: NimRTC does not use `dlopen` or equivalent.
- **DTLS**: DTLS handshake is delegated to mbedTLS, which follows its own
  security disclosure policy.
- **SRTP**: libsrtp handles all SRTP operations; key material is never
  logged or exposed in debug output.

For the full threat model, see §11 of `docs/zh/NimRTC-V2-技术文档.md`.
