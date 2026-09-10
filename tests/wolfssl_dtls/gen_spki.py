import asyncio, json
from signaling_proxy import NimRTCSubprocess
from pathlib import Path

async def gen_spki():
    # Just start and immediately stop to generate SPKI
    sub = NimRTCSubprocess(
        Path('D:/MyOpen/NimRTC/build/examples/Debug/demo-p2p.exe'),
        answerer=False, duration=2, no_stun=True
    )
    await sub.start()
    await asyncio.sleep(1)
    sub.stop()

asyncio.run(gen_spki())
