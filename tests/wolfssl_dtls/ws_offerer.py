#!/usr/bin/env python3
"""
Python WebSocket offerer — acts as Chrome's WebRTC offerer.
Creates an SDP offer, sends it via signaling, receives answer.
Used for NimRTC ↔ Chrome DTLS interop testing.
"""
import asyncio, json, sys, time
import websockets

SDP_OFFER = """v=0
o=- 0 0 IN IP4 127.0.0.1
s=NimRTC-Test
t=0 0
a=group:BUNDLE 0
a=msid-semantic: WMS test
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 0.0.0.0
a=ice-ufrag:test
a=ice-pwd:test
a=ice-options:trickle
a=fingerprint:sha-256 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00
a=setup:actpass
a=mid:0
a=sctp-port:5000
a=max-message-size:1073741823
"""

async def run_test(room_url, timeout=25):
    print(f'Connecting to {room_url}...')
    async with websockets.connect(room_url) as ws:
        print('Connected!')
        
        # Create and send offer
        offer = {
            'type': 'offer',
            'sdp': SDP_OFFER,
            'from': 'python_offerer'
        }
        await ws.send(json.dumps(offer))
        print('Offer sent!')
        
        # Wait for answer
        print('Waiting for answer...')
        try:
            msg = await asyncio.wait_for(ws.recv(), timeout=timeout)
            answer = json.loads(msg)
            print(f'Got answer! type={answer.get("type")} sdp_len={len(answer.get("sdp",""))}')
            
            # Send ICE candidates
            candidates = [
                {'type': 'candidate', 'candidate': 'candidate:1 1 UDP 2130706431 127.0.0.1 9 typ host', 'sdpMid': '0', 'sdpMLineIndex': 0},
                {'type': 'candidate', 'candidate': 'candidate:2 1 UDP 2130706431 ::1 9 typ host', 'sdpMid': '0', 'sdpMLineIndex': 0},
            ]
            for c in candidates:
                await ws.send(json.dumps(c))
                print(f'Sent ICE candidate')
            
            print('Python offerer: SUCCESS - offer/answer exchanged')
            return True
        except asyncio.TimeoutError:
            print(f'No answer received in {timeout}s')
            return False

if __name__ == '__main__':
    room = sys.argv[1] if len(sys.argv) > 1 else 'ws://localhost:8765/python_test'
    success = asyncio.run(run_test(room))
    sys.exit(0 if success else 1)
