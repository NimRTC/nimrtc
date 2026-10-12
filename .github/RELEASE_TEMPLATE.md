<!--
  GitHub Release Notes template for NimRTC.
  Attach this to the release body when you `gh release create` or
  use the web UI "Generate release notes" button. Fill in or replace
  every {{...}} placeholder. Sections marked (delete if N/A) can be
  removed on a per-release basis.
-->

# {{TAG}} — {{TITLE}}

> **Cut {{DATE}}.** {{ONE_LINE_SUMMARY}}
> {{API_STATUS_LINE}} <!-- e.g. "API experimental — not for production through v1.0.0." -->

{{OPENING_PARAGRAPH: 1–2 sentences that say "what is this release and
why does it exist." Lead with the user-visible surface, not the
internal refactor count.}}

---

## TL;DR

- {{BULLET_1: the single most visible change}}
- {{BULLET_2: the second most visible change}}
- {{BULLET_3: ...}}
- {{BULLET_4: ...}}
- {{BULLET_5: ...}}

---

## What's new

### {{FEATURE_1_NAME}}

{{FEATURE_1_PARAGRAPH_1: what is it, in one sentence.}}

{{FEATURE_1_PARAGRAPH_2: why does it exist, in one sentence.}}

{{FEATURE_1_PARAGRAPH_3: how do you use it (short code snippet, link to
docs).}}

{{FEATURE_1_COVERAGE: test name and pass count, if applicable.}}

### {{FEATURE_2_NAME}}

{{FEATURE_2_BODY: same pattern.}}

### {{FEATURE_3_NAME}}

{{FEATURE_3_BODY: same pattern.}}

---

## What's not in this release (deferred)

<!-- Optional section. Always honest about cuts; never bury deferrals. -->

- **{{DEFERRED_ITEM_1}}** — {{WHY_DEFERRED}}, see
  `{{LINK_TO_PLAN_SECTION}}`.
- **{{DEFERRED_ITEM_2}}** — {{WHY_DEFERRED}}.
- **{{DEFERRED_ITEM_3}}** — {{WHY_DEFERRED}}.

---

## Migrating from {{PREVIOUS_TAG}}

**Source-compat:** {{SOURCE_COMPAT_NOTE: e.g. "every v0.10.x caller
continues to compile and link."}}

- {{BREAKING_CHANGE_1: e.g. "API change: the `dtls_name` field is new
  in `EngineConfig`. Defaults to `""`."}}
- {{BREAKING_CHANGE_2: ...}}

**Wire-compat:** {{WIRE_COMPAT_NOTE: e.g. "the Chrome media interop
contract is identical to the previous release."}}

**Layout invariants preserved:** {{LAYOUT_INVARIANTS_NOTE}}

---

## Known issues / limitations

- {{KNOWN_ISSUE_1}}
- {{KNOWN_ISSUE_2}}
- {{KNOWN_ISSUE_3}}

---

## Test surface

| Test | Result | Notes |
|---|---|---|
| {{TEST_1}} | {{RESULT_1}} | {{NOTES_1}} |
| {{TEST_2}} | {{RESULT_2}} | {{NOTES_2}} |
| {{TEST_3}} | {{RESULT_3}} | {{NOTES_3}} |

---

## Acknowledgements

- {{THANKS_1: e.g. "The wolfSSL maintainers for the embedded-friendly
  DTLS 1.3 backend."}}
- {{THANKS_2: e.g. "Everyone who filed an issue or opened a discussion
  on {{PREVIOUS_TAG}} — your feedback drove {{DRIVER_1}}, {{DRIVER_2}},
  and the {{DEFERRED_AREA}} deferral."}}

---

## Get involved

- 📖 [Full documentation](https://github.com/NimRTC/nimrtc/tree/main/docs)
- 💬 [GitHub Discussions](https://github.com/NimRTC/nimrtc/discussions)
- 🐛 [Issue tracker](https://github.com/NimRTC/nimrtc/issues)
- 📰 [Why NimRTC? (blog)](https://github.com/NimRTC/nimrtc/blob/main/docs/blog/2026-10-10-why-nimrtc.md)

---

**License:** Apache-2.0
**Version:** {{TAG}} ({{CUT_STATUS: e.g. "Beta 前哨"}})
**Next planned release:** {{NEXT_TAG}}
