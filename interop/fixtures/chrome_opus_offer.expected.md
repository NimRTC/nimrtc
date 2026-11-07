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

## Direction

| Field | Value | Expected |
|-------|-------|---------|
| direction | `a=sendrecv` | ✅ the page captured the fake microphone (`sendAudio=1`), so Chrome sends Opus **and** receives it — this is the offer that exercises the Chrome → NimRTC direction (spec V-3) |
| msid | `a=msid:chrome-audio audio-track` | ✅ the captured track's id, used to bind the inbound RTP stream to the media section |

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
| setup | `actpass` | ✅ NimRTC as answerer takes Server role (passive; Chrome124+ BoringSSL rejects anonymous Client CertReq workaround) |
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
