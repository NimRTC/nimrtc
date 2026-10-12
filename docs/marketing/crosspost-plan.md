# Cross-post Plan — NimRTC v0.11.0 Show HN

> **Anchor:** T+0 = the moment the HN main-feed post goes live.
> **Window:** Tue 13 Oct / Wed 14 Oct 2026, 9:00–11:00 PT (US-Pacific
> morning, peak HN traffic).
> **Do not retitle the HN post** after it goes live — HN's "controversy"
> ranking penalises edits, and the title is locked (see
> `show-hn-post.md` §"Selected title").
>
> **Decision rule before each row:** if the *previous* post is not
> trending (HN: <30 net votes / 4 h, lobste.rs: <5 net votes / 2 h,
> Reddit: <50 net upvotes / 12 h), **skip that row and re-evaluate**.
> Karma-farming a dead thread is worse than not posting at all.

---

## Time-ordered schedule

| URL (channel) | Title | Delay (T+X) | Link target |
|---|---|---|---|
| https://news.ycombinator.com/submit **(HN main, "Show HN")** | `Show HN: NimRTC – A from-scratch C++20 WebRTC engine, ~5.6MB static lib` | T+0 (Tue/Wed 9-11am PT) | https://github.com/NimRTC/NimRTC |
| (same HN post, follow-up comment) | `Update:` — 30-second demo clip + build-from-scratch command + 1-line "if you want to try it" | T+6 h, **only if** HN post has >50 net votes | docs/demos/demo-agent-gateway-30s.mp4 + `README.md` §Quick Start |
| https://twitter.com/compose/post **(Twitter / X thread, 7 tweets)** | T1: "Just shipped v0.11.0 — a from-scratch C++20 WebRTC engine, 5.6MB static lib, 4-platform CI green. Show HN today → [link]" | T+0 (right after HN goes live, ≤ 5 min) | HN post URL + repo + `#showhn` |
| https://lobste.rs/new **(lobste.rs)** | `NimRTC – A from-scratch C++20 WebRTC engine, ~5.6MB static lib` | T+4 h, **only if** HN is trending (>30 net votes) | https://github.com/NimRTC/NimRTC |
| https://www.reddit.com/r/cpp/submit **(r/cpp)** | `NimRTC: embeddable C++20 WebRTC engine, 4-platform CI green` | T+24 h, **only if** HN still on front page | repo + HN post URL (comment body, not title) |
| https://www.reddit.com/r/programming/submit **(r/programming)** | `NimRTC: a from-scratch C++20 WebRTC engine` | T+24 h | HN post URL |
| https://www.reddit.com/r/China_Developers/submit **(/r/China_Developers)** | `NimRTC: C++20 WebRTC engine, GMSSL-ready, 4-platform green` | T+48 h | repo + `docs/blog/2026-10-10-why-nimrtc.zh.md` |
| https://www.v2ex.com/newtopic **(V2EX)** | `一个从零写的 C++20 WebRTC 引擎` | T+48 h | repo + 中文 blog |
| https://dev.to/new **(DEV.to)** | use `docs/blog/2026-10-10-why-nimrtc.md` as the post body | T+72 h | blog post URL (self-link) |
| https://hashnode.com/dashboard **(Hashnode)** | (cross-import of the DEV.to post) | T+72 h | blog post URL |

---

## DO NOT

- **Do not retitle the HN post.** Edits tank the "controversy" score.
  If title is wrong, leave it; fix the *next* post.
- **Do not cross-post to lobste.rs in the same hour as HN** if your HN
  post is still on the front page — the audience overlap is too high and
  it looks spammy. T+4 h is the floor.
- **Do not post to r/cpp before HN has cooled.** r/cpp's regulars WILL
  check HN first; if HN flopped, the r/cpp post becomes a public record
  of the flop. T+24 h gives HN time to recover from "controversy"
  reshuffling.
- **Do not post to r/netsec, r/cryptography, or any "security"-tagged
  subreddit** during the launch window. The "GM/T cryptography" story
  is a magnet for "but is it audited?" threads, and v0.11.0 has no
  third-party audit. Save the security discussion for a dedicated post
  with the audit doc attached.
- **Do not post to Hacker News "Ask HN"** in the same week. HN
  penalises authors who post multiple threads inside 7 days.
- **Do not DM individual HN users asking for upvotes** — instant
  ban, no appeal.
- **Do not auto-tweet from a CI bot.** The first tweet must be a
  hand-written thread, not "Build #1234 passed".

---

## Per-channel notes

### Hacker News (T+0)

- Submit **as text**, not as a link — the README has too many sections
  to fit a "link" preview well, and a text post lets you control the
  first impression.
- If you submit as a link, link the **repo root**, not a specific
  commit / branch / README anchor.
- The first 5 minutes determine the "controversy" seed. Stay online,
  answer the 5 tier-1 questions in `hn-replies-cheatsheet.md`.
- If the post sits at 0 votes after 30 minutes, do **not** post
  "bump" or "Update:" — that flags the post. Wait for organic upvotes
  or accept the loss.

### Twitter / X (T+0, ≤ 5 min after HN)

- 7-tweet thread, image in T1: hero shot of the live demo or a
  benchmark table. No "🚀" in technical tweets.
- T1 has the hook + HN link. T2–T6: 1 feature per tweet (PCM tap,
  Profile compilation, GMSSL seam, 4-platform CI, comparison matrix).
  T7: repo + `Show HN` + "feedback welcome" + 1 hashtag.
- Pin the thread to your profile for 48 h.
- Do **not** quote-tweet yourself on HN. HN sees this and downranks.

### lobste.rs (T+4 h, conditional)

- lobste.rs is invite-only for new accounts. If your account is <30
  days old, skip and post only on HN.
- Tag: `webrtc`, `cpp`, `audio`, `video`. lobste.rs auto-suggests
  tags; pick at most 3.
- lobste.rs culture is anti-hype; rewrite the first paragraph to be
  drier than the HN version. Drop the "🚀", drop "embeddable" — use
  "in-process, no dynamic linking required".

### Reddit r/cpp, r/programming (T+24 h)

- **Cross-link to the HN post in the comment body, not the title.**
  Reddit title rules forbid "Show HN" prefixes; use a plain descriptive
  title.
- r/cpp in particular is hostile to "yet another X" posts. Open
  with a 2-sentence "why I wrote this"; don't dump the README.
- Reply to every comment in r/cpp within 4 hours for the first 24 h.

### V2EX, /r/China_Developers (T+48 h)

- Use the ZH blog post (`docs/blog/2026-10-10-why-nimrtc.zh.md`) as
  the body. V2EX in particular reads Markdown badly — use the
  `<!--markdown-->...<!--/markdown-->` tag if V2EX flags it.
- On V2EX, the relevant node is `程序员` (`/go/programmer`) or
  `分享创造` (`/go/create`). Pick `分享创造` — `程序员` will redirect
  you to the GitHub and dismiss.

### DEV.to / Hashnode (T+72 h)

- The DEV.to algorithm de-prioritises posts that link out to HN
  within 48 h. T+72 h is the floor; T+96 h is safer.
- Cross-link DEV.to and Hashnode. Canonical URL is the DEV.to post
  (better SEO); Hashnode is a duplicate with `<link rel="canonical">`.

---

## Failure-mode playbook

| Symptom | Action |
|---|---|
| HN post flagged/killed within 30 min | Do nothing. HN sometimes kills the first resubmit; resubmit tomorrow at the same window. |
| HN post <20 votes after 4 h | Skip lobste.rs. Wait 1 week, then repost with title #2 (`I rebuilt WebRTC's media plane in C++20`) at the same window. |
| HN post >200 votes after 12 h | Add a 2nd "Update:" comment with a 30-second demo clip and a pointer to a 5-line hello world. Consider cutting a v0.11.1 patch release to capitalise on attention. |
| Lobste.rs front-paged | Cross-post the lobste.rs thread to r/cpp at T+24 h instead of the HN thread — lobste.rs comments are more relevant to r/cpp's audience. |
| A commenter files a credible bug | Acknowledge publicly in-thread within 4 h. If the fix is <1 day, ship v0.11.1 and add a 3rd "Update:" with the fix. |
| A commenter asks about iOS/Android | Reply with Tier-2 #7 from `hn-replies-cheatsheet.md`. Do not promise a date. |
| A Chinese-channel post goes viral before HN | Accept the win; HN becomes a secondary surface, not the primary. |
| "AI wrote this" accusations | Don't engage. The codebase and commit history are public; the accusation will be answered by other commenters, not by you. |

---

## Tooling

- **HN notification:** `https://hn.algolia.com/api/v1/search?query=NimRTC`
  polled every 5 min for the first 6 h, then every 30 min for the next
  18 h. The `hn-replies-cheatsheet.md` 5-question set is your
  triage order.
- **Star watch:** GitHub repo → Watch → "Custom" → "Releases only"
  for the launch week. A spike in stars within 24 h post-launch
  correlates with HN success — useful for triggering the
  "v0.11.1 fast cut" decision.
- **Cross-link audit:** T+96 h, do a final sweep: every post should
  have a working link to the repo + (for non-HN posts) to the
  canonical blog URL. Dead links in cross-posts are a common
  embarrassment.
