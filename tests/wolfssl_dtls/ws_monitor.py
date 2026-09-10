#!/usr/bin/env python3
import asyncio, websockets, json, sys

async def monitor():
    room = 'chrome_test_room'
    print(f'Connecting to ws://localhost:8765/{room}...')
    try:
        async with websockets.connect(f'ws://localhost:8765/{room}') as ws:
            print('Connected!')
            msg = await asyncio.wait_for(ws.recv(), timeout=5)
            print(f'Got: {msg[:300]}')
    except asyncio.TimeoutError:
        print('Timeout - no messages received')
    except Exception as e:
        print(f'Error: {e}')

asyncio.run(monitor())
