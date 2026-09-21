# GitHub Discussions

NimRTC uses **GitHub Discussions** as its structured community channel. It supplements
GitHub Issues (for bugs and tracked work items) with a space for conversation,
design discussion, and Q&A.

## Table of Contents

- [Categories](#categories)
- [Posting guidelines](#posting-guidelines)
- [Feature proposals vs. formal RFCs](#feature-proposals-vs-formal-rfcs)
- [Moderation](#moderation)

---

## Categories

| Category | Description |
|---|---|
| [Announcements](https://github.com/NimRTC/NimRTC/discussions/categories/Announcements) | Release notes, security advisories, project news. Maintainer-posted only. |
| [General](https://github.com/NimRTC/NimRTC/discussions/categories/General) | Open discussion — introductions, architecture questions, project direction. |
| [Ideas](https://github.com/NimRTC/NimRTC/discussions/categories/Ideas) | RFC-light feature proposals. Use this before opening a formal RFC. |
| [Show and tell](https://github.com/NimRTC/NimRTC/discussions/categories/Show-and-tell) | Community-built apps, profiles, integrations, demos. |
| [Q&A](https://github.com/NimRTC/NimRTC/discussions/categories/Q&A) | Usage questions and API help. |

---

### Announcements

**Purpose:** Release notes, security advisories, and official project news.

**Who posts:** Maintainers only.

**Guidelines:** Replies are open. Keep discussion on-topic. Off-topic comments may be moved to General.

**Example topics:**
- v0.11.0 Beta released
- Security: CVE-YYYY-XXXX in dependency X
- Upcoming breaking change in v0.12.0

---

### General

**Purpose:** Open Q&A, introductions, and conversation that doesn't fit elsewhere.

**Who posts:** Anyone — users evaluating NimRTC, contributors, maintainers.

**Guidelines:** One thread per topic. Search before posting a new thread. For bug reports, use Issues instead.

**Example topics:**
- "How does ICE candidate gathering work internally?"
- "First time evaluating NimRTC — impressions from a libwebrtc user"
- "Questions about the plugin architecture after reading ADR-009"

---

### Ideas

**Purpose:** Feature proposals before they become formal RFCs. Use this category to
socialise an idea, gather early feedback, and gauge interest before writing a full RFC.

**Who posts:** Anyone.

**Guidelines:** Describe the problem you are solving and a rough proposed solution.
Do not open a full RFC draft here — start a GitHub Discussion first. If the idea
gains traction, a maintainer will suggest moving it to the formal RFC process
(see `.github/DISCUSSIONS.md` for the RFC template).

**Example topics:**
- "Add a built-in TURN fallback to reduce external dependency on third-party TURN servers"
- "Profile variant for IoT low-power mode (reduced bitrate, longer keepalive)"
- "Expose BWE statistics via a public stats API"

---

### Show and tell

**Purpose:** Share what you have built with NimRTC — apps, profiles, integrations, demos,
blog posts, conference talks, or video walkthroughs.

**Who posts:** Anyone.

**Guidelines:** Brief description + link to repo, demo, or write-up. Works in progress
welcome. Do not post questions here; use Q&A instead.

**Example topics:**
- "NimRTC-based IoT intercom running on Raspberry Pi 5"
- "Custom agent-gateway profile for healthcare voice monitoring"
- "Blog post: Replacing libwebrtc with NimRTC in an existing C++ desktop app"

---

### Q&A

**Purpose:** Usage questions, API help, and troubleshooting.

**Who posts:** Anyone.

**Guidelines:** One question per thread. Search before posting — the question may already
be answered. Provide enough context: NimRTC version, platform, CMake options, and a
minimal reproducer if applicable.

**Example topics:**
- "How do I configure the PCM tap for Whisper streaming?"
- "DTLS handshake times out on Windows but works on Linux — why?"
- "What is the correct way to configure ICE with a public STUN server?"

---

## Posting guidelines

1. **Be specific.** Include version numbers, platform, and compiler where relevant.
2. **One topic per thread.** Threads with mixed topics are hard to follow and search.
3. **Search before posting.** Check both Discussions and Issues.
4. **Use the right category.** Issues are for tracked bugs and feature requests;
   Discussions are for conversation.
5. **Be respectful.** NimRTC has contributors from many time zones and backgrounds.
6. **Tag your post.** Use the category's tag when available (e.g. `[Q&A]`, `[Idea]`).

---

## Feature proposals vs. formal RFCs

For **small to medium** feature ideas, start an **Ideas** discussion. A maintainer will
respond with "LGTM" (consider for implementation), "Needs RFC" (requires formal design),
or close with explanation.

For **architecturally significant** changes — new plugin interfaces, API surface changes,
or cross-module design — the Ideas thread may be escalated to a **formal RFC**. See
[`docs/rfcs/`](docs/rfcs/) for the existing RFCs and the RFC template. RFCs go through
**Draft → Review → Final** status stages and are merged into the documentation when Final.

---

## Moderation

Maintainers may:
- Close threads that are duplicates, off-topic, or inactive for >6 months.
- Move posts to a more appropriate category.
- Lock threads exhibiting prolonged incivility.

If you believe a moderation action was in error, open a private issue or contact a
maintainer directly.
