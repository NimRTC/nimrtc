# Generates src/third_party/libjuice/patches/0003-turn-allocation-failure-reason.patch
# from the working-tree diff of the vendored libjuice tree.
#
# The patch paths are rewritten from the NimRTC-relative
# "src/third_party/libjuice/src/..." to the libjuice-relative form, so the
# artifact applies to a checkout of libjuice itself (like 0001 and 0002 do):
#   src/third_party/libjuice/src/include/juice/juice.h -> include/juice/juice.h
#   src/third_party/libjuice/src/src/agent.c           -> src/agent.c
#
# The diff is taken against the NimRTC repository's HEAD, so it describes the
# DELTA introduced by the N4 connectivity-diagnostics work on top of the tree
# as it stands after PR #34 (TURN over TCP), PR #50 (0001-so-reuseaddr) and
# PR #139 (0002-turn-tls-transport).  It does not apply to a pristine
# upstream checkout.

$ErrorActionPreference = 'Stop'

$repo    = Split-Path -Parent $PSScriptRoot
$outFile = Join-Path $repo 'src/third_party/libjuice/patches/0003-turn-allocation-failure-reason.patch'

$lines = & git -C $repo diff --src-prefix=a/ --dst-prefix=b/ -- src/third_party/libjuice/
if ($LASTEXITCODE -ne 0) { throw 'git diff failed' }

$diff = ($lines -join "`n") + "`n"

# Strip the NimRTC-relative prefix so the paths are libjuice-relative.
$diff = $diff.Replace('a/src/third_party/libjuice/src/', 'a/')
$diff = $diff.Replace('b/src/third_party/libjuice/src/', 'b/')
$diff = $diff.Replace('diff --git a/src/third_party/libjuice/src/', 'diff --git a/')
$diff = $diff.Replace('diff --git b/src/third_party/libjuice/src/', 'diff --git b/')

$header = @'
From: NimRTC contributors <bot@nimrtc.local>
Subject: [PATCH] libjuice: a TURN allocation failure reason the host can read

Upstream base commit: 77daa8befd828046169adbb012d8de928bfdcdd4

Applies on top of: the NimRTC libjuice tree after PR #34 (TURN over TCP,
which added src/turn_tcp.c), PR #50 (0001-so-reuseaddr) and PR #139
(0002-turn-tls-transport).  It does not apply to a pristine upstream
checkout.

Upstream libjuice reports a failed TURN allocation only through the log.
The Allocate response's error code is consumed inside
agent_process_turn_allocate(), the entry is marked
AGENT_STUN_ENTRY_STATE_FAILED, and the code is gone.  A host that wants
to tell "the server refused my credentials" from "the server is
unreachable" from "the transport could not be opened" cannot: all three
look identical from the outside, and each calls for a different action
(refresh the credential, try another path, fix the TLS client).

The patch adds one classification field and one read function.  It does
not change any behaviour: the same branches that already logged and
failed now also record *why*.

The seam is juice_turn_failure_t in include/juice/juice.h:

  JUICE_TURN_FAILURE_NONE         no allocation has failed
  JUICE_TURN_FAILURE_UNAUTHORIZED 401: the server refused the credentials
  JUICE_TURN_FAILURE_STALE_NONCE  438: the nonce went stale
  JUICE_TURN_FAILURE_TRANSPORT    the transport could not be opened at all
  JUICE_TURN_FAILURE_ALLOCATE     any other Allocate error

and juice_get_turn_failure(juice_agent_t *agent), which reads the
category the agent recorded.

The classification is deliberately done HERE, in the only place that
knows the context, and the host gets the *category* rather than the raw
STUN error code.  The raw code table is libjuice's business; a
host-facing surface should not leak it, and the caller of this seam
wants "was it the credentials" rather than "was it 401".

Two distinct sites are worth naming, because they are different facts
that used to look the same:

  - the 401 branch, when the realm is already known, is a refused
    credential: the server answered the *authenticated* request with
    401 (JUICE_TURN_FAILURE_UNAUTHORIZED);

  - the agent_turn_state_t::failed flag, set when the transport could
    not be opened at all (for example TURN over TLS with no usable TLS
    client), means the server was never reached
    (JUICE_TURN_FAILURE_TRANSPORT).

A 438 that cannot be acted on -- the server asked for a fresh nonce
and did not give one -- is JUICE_TURN_FAILURE_STALE_NONCE, and every
other Allocate error is JUICE_TURN_FAILURE_ALLOCATE.

The agent's field is zero-initialised by the existing calloc, so a
fresh agent reports JUICE_TURN_FAILURE_NONE.

NimRTC's host side of the seam is src/modules/ice/src/ice.cpp, which
reads the category through the ICE plugin seam and lets the engine
publish it as the transport plane's failure reason.

'@

Set-Content -Path $outFile -Value ($header + $diff) -NoNewline -Encoding utf8
Write-Host "wrote $outFile"
