# Generates src/third_party/libjuice/patches/0002-turn-tls-transport.patch from the
# working-tree diff of the vendored libjuice tree.
#
# The patch paths are rewritten from the NimRTC-relative
# "src/third_party/libjuice/src/..." to the libjuice-relative form, so the
# artifact applies to a checkout of libjuice itself (like 0001 does):
#   src/third_party/libjuice/src/include/juice/juice.h -> include/juice/juice.h
#   src/third_party/libjuice/src/src/agent.c           -> src/agent.c
#
# The diff is taken against the NimRTC repository's HEAD, so it describes the
# DELTA introduced by the TURN-over-TLS work on top of the tree as it stands
# after PR #34 (TURN over TCP) and PR #50 (0001-so-reuseaddr).  It does not
# apply to a pristine upstream checkout.

$ErrorActionPreference = 'Stop'

$repo    = Split-Path -Parent $PSScriptRoot
$outFile = Join-Path $repo 'src/third_party/libjuice/patches/0002-turn-tls-transport.patch'

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
Subject: [PATCH] libjuice: a TURN-over-TLS transport seam, without a plaintext
 fallback

Upstream base commit: 77daa8befd828046169adbb012d8de928bfdcdd4

Applies on top of: the NimRTC libjuice tree after PR #34 (TURN over TCP,
which added src/turn_tcp.c) and PR #50 (0001-so-reuseaddr).  It does not
apply to a pristine upstream checkout.

Upstream libjuice knows the TURN transport enum value
JUICE_TURN_TRANSPORT_TLS but has no way to actually speak it: the
allocation path logs "TURN over TLS is not supported, falling back to
TCP" and opens a *plain* TCP control connection to the server.  That
fallback is the defect this patch removes.  A TURN server reached over
TLS expects the client to speak TURN inside a TLS session; a plain TCP
connection to the same port either fails (a TLS-only listener) or, on a
server that also listens in the clear, silently sends the TURN
credentials in the clear.

libjuice owns the TURN protocol and its self-delimiting stream framing
(RFC 8656 sections 12.4 / 12.5), but it does not own a TLS stack, and a
vendored MPL-2.0 tree is the wrong place to carry one.  So the host
supplies the TLS client through a narrow ops seam, and libjuice uses it
for the *control connection* only.  The relayed transport address
allocated by the server stays UDP, as RFC 6062 requires.

The seam is juice_turn_tls_ops_t in include/juice/juice.h:

  - connect  wraps an already-connected socket in a TLS session
  - handshake advances the handshake, one step at a time
  - read     / write transfer bytes
  - close    releases the session

The implementation must be stream oriented: a write may transfer fewer
bytes than asked for, and a read may return fewer bytes than requested.
libjuice's framing is self-delimiting, so partial messages are handled
by the caller.

The ops are consulted for JUICE_TURN_TRANSPORT_TLS only.  A NULL ops
pointer, or a NULL connect, means "TLS is not available", and the TURN
entry then fails immediately: it does NOT fall back to plain TCP.  The
agent_turn_state_t::failed flag records that, so the entry fails at
once instead of waiting for the retransmission timeout.

The connection itself becomes three-phase, because a TLS session can
only be created once the TCP connection has completed, and the
handshake may need several round trips:

  1. the non-blocking TCP connect, as before
  2. the TLS session, created lazily from the connected socket
  3. the TLS handshake, advanced incrementally

Once the session exists, turn_tcp_stream_recv() and flush_locked() go
through the TLS ops rather than recv()/send() on the raw socket.  The
session owns the socket from that point on, and closes it.

The public juice_turn_server_t gains a server_name field: the name
presented as SNI and verified against the server's certificate.  NULL
means "use the host name", which is the right default when the host
name is also the name in the certificate.

NimRTC's host side of the seam is src/modules/ice/src/turn_tls.cpp,
which implements the ops on top of wolfSSL (TLS 1.2, CA verification,
SAN dNSName check, SNI).

'@

Set-Content -Path $outFile -Value ($header + $diff) -NoNewline -Encoding utf8
Write-Host "wrote $outFile"
