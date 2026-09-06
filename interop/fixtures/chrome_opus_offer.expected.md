# Chrome Opus SDP Offer — Expected Parsing Results

## SDP origin

| Field | Value | Parsed by NimRTC |
|-------|-------|-----------------|
| version (v=) | 0 | ✅ RFC 4566 §5.1 |
| origin (o=) | `- 1234567890 1234567890 IN IP4 0.0.0.0` | ✅ sdp::Parser |
| session name (s=) | `-` | ✅ |
| timing (t=) | `0 0` | ✅ |

## Media (m=audio)

| Field | Value | Expected |
|-------|-------|---------|
| port | 9 (discard) | ✅ NimRTC ignores port 9 |
| protocol | `UDP/TLS/RTP/SAVPF` | ✅ dtls::DtlsRole detection |
| payload type | 111 | ✅ Opus codec |
| BUNDLE group | `a=group:BUNDLE 0` | ✅ session handles BUNDLE |
| rtcp-mux | `a=rtcp-mux` | ✅ single-port mode |
| rtcp-rsize | `a=rtcp-rsize` | ✅ reduced-size RTCP |

## ICE

| Field | Value | Expected |
|-------|-------|---------|
| ice-ufrag | `ChromeUfrag` | ✅ ice::IceTransport::set_remote_description |
| ice-pwd | `ChromePassword123456789012345678` | ✅ same |
| trickle | `a=ice-options:trickle` | ✅ libjuice trickle ICE |
| candidate | (not in this fixture) | ✅ libjuice generates its own |

## DTLS

| Field | Value | Expected |
|-------|-------|---------|
| fingerprint | `sha-256 AA:BB:...` | ✅ dtls::DtlsSession verifies peer fingerprint |
| setup | `actpass` | ✅ NimRTC sets Client role |
| certificate | (self-signed, generated at runtime) | ✅ |

## Opus

| Field | Value | Expected |
|-------|-------|---------|
| rtpmap:111 | `opus/48000/2` | ✅ opus::Encoder/Decoder init |
| fmtp:111 | `minptime=10;useinbandfec=1;stereo=0` | ✅ passed to opus encoder config |
| sprop-stereo | `0` | ✅ |
| extmap (1) | `ssrc-audio-level` | ✅ RFC 6464, NimRTC passes through |
| extmap (2) | `abs-send-time` | ✅ RFC 5104 §3.5.2 |

## SSRC

| Field | Value | Expected |
|-------|-------|---------|
| ssrc:1000 | `cname:chrome-cname` | ✅ rtp::PacketBuilder mirrors SSRC |
| msid | `chrome-audio audio-track` | ✅ session tracks msid forJB init |
