import asyncio
import io
import json
import sys
import websockets

# Force UTF-8 output so card suit symbols don't crash on Windows
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', line_buffering=True)
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', line_buffering=True)

URL = "ws://192.168.4.1/ws"

async def expect(ws, type_name, timeout=2.0):
    seen_types = []
    while True:
        raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
        d = json.loads(raw)
        seen_types.append(d.get("type"))
        if d.get("type") == type_name:
            return d, seen_types

async def drain(ws, timeout=0.4):
    msgs = []
    try:
        while True:
            raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
            msgs.append(json.loads(raw))
    except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
        pass
    return msgs

async def main():
    # Sign in as test_1
    async with websockets.connect(URL) as ws:
        await expect(ws, "ready")
        await ws.send(json.dumps({"type":"login","name":"test_1","pass":"test1+"}))
        await expect(ws, "signedin")
        print("[setup] signed in as test_1")

    # Open game WS
    async with websockets.connect(URL) as ws:
        await expect(ws, "ready")
        await ws.send(json.dumps({"type":"joinChat"}))
        await expect(ws, "loggedin")
        print("[setup] joined chat as test_1")

        # Cleanup any old game
        await ws.send(json.dumps({"type":"leaveGame"}))
        await drain(ws)

        # Host a new game
        await ws.send(json.dumps({"type":"hostGame","deck":36,"jokers":False}))
        d, _ = await expect(ws, "hostedGame")
        code = d["code"]
        print(f"\n>>> GAME CODE: {code} <<<")
        print(">>> On your phone: open http://chat.local/durak, enter this code, click Join. <<<\n")

        # Wait for test_2 to join (lobbyUpdate with 2 players)
        print("[wait] waiting for test_2 to join (60s timeout)...")
        try:
            while True:
                raw = await asyncio.wait_for(ws.recv(), timeout=60.0)
                m = json.loads(raw)
                if m.get("type") == "lobbyUpdate":
                    players = m.get("players", [])
                    print(f"[lobby] players now: {players}")
                    if len(players) >= 2:
                        print("[lobby] test_2 joined!")
                        break
        except asyncio.TimeoutError:
            print("[wait] timed out waiting for test_2")
            return

        # Start the game
        print("\n[start] starting game...")
        await ws.send(json.dumps({"type":"startGame"}))
        # First we'll see gameStarted + gameState
        # We're test_1 — see if we got a hand
        gs = None
        for _ in range(10):
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=3.0)
                m = json.loads(raw)
                print(f"[event] {m.get('type')}")
                if m.get("type") == "gameState":
                    gs = m
                    break
            except asyncio.TimeoutError:
                break

        if gs is None:
            print("[error] no gameState received")
            return

        print(f"\n[my-hand] {gs['hand']}")
        print(f"[trump] {gs['trump']} (card: {gs['trumpCard']})")
        print(f"[opp] {gs['opp']}")
        print(f"[deck-left] {gs['deck']}")
        print(f"[attacker] {gs['atk']}, [defender] {gs['def']}, [me] {gs['me']}")

        # If I'm the attacker, attack with the lowest trump or first card
        if gs['atk'] == gs['me']:
            print("\n[me=attacker] picking a card to attack with...")
            # Pick the lowest trump if any, else lowest card
            hand = gs['hand']
            trump = gs['trump']
            trumps = [c for c in hand if c.startswith(trump)]
            non_trumps = [c for c in hand if not c.startswith(trump)]
            pick = (sorted(trumps)[0] if trumps else sorted(non_trumps)[0])
            print(f"[me] attacking with: {pick}")
            await ws.send(json.dumps({"type":"attack","card":pick}))
        else:
            print("\n[me=defender] waiting for opponent to attack...")

        # Stream events for 90s so user can play their turn on phone
        print("\n[stream] streaming game events for 90s — play on your phone now")
        end = asyncio.get_event_loop().time() + 90
        while asyncio.get_event_loop().time() < end:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=2.0)
                m = json.loads(raw)
                t = m.get("type")
                if t == "gameState":
                    print(f"\n[gameState] table={m['table']} my-hand={m['hand']} atk={m['atk']} def={m['def']}")
                elif t == "system":
                    print(f"[system] {m.get('text')}")
                elif t == "gameOver":
                    print(f"\n=== GAME OVER — DURAK: {m['loser']} ===")
                    return
                else:
                    print(f"[{t}] {m}")
            except asyncio.TimeoutError:
                pass
        print("\n[stream] 90s elapsed — ending test")

asyncio.run(main())
