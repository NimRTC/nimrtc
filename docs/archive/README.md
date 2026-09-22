# `docs/archive/`

Historical documents retained for archaeology. These should not be
linked from the main docs and may be deleted at a future major
release (1.0+).

| File | Era | Replaced by | Note |
|---|---|---|---|
| `WOLFSSL_INTEGRATION.md` | pre-0.9.0 | `docs/zh/architecture.md` §11; `docs/plan/transport-selection.md` | Historical design draft. The wolfSSL-only migration landed in `src/modules/dtls/`; this document's build steps and directory layout are obsolete. |

Rules for `archive/`:

- Documents added here MUST retain their original contents verbatim
  (or with only minor formatting fixes).
- They MUST carry a visible "SUPERSEDED" header that points to the
  successor document.
- They MUST NOT be linked from active docs (`docs/api/`,
  `docs/guides/`, `docs/profiles.md`, README, etc.).
- They MAY be deleted at the 1.0 release if the build chain still
  functions without them.
