import asyncio
import json
import websockets

URL = "ws://192.168.4.1/ws"

async def drain(ws, timeout=0.4):
    msgs = []
    try:
        while True:
            raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
            msgs.append(json.loads(raw))
    except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
        pass
    return msgs

async def expect(ws, type_name, timeout=2.0):
    """Wait for a message of type_name, return it AND a list of msgs seen before."""
    seen_types = []
    while True:
        raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
        d = json.loads(raw)
        seen_types.append(d.get("type"))
        if d.get("type") == type_name:
            return d, seen_types

async def main():
    print("\n=== TEST 1: Sign in as test_1 (default mod) ===")
    async with websockets.connect(URL) as ws:
        await expect(ws, "ready")
        print("  ws ready")
        signed_in = False
        for pw in ["test1+", "pw1234", "test", "test1234"]:
            await ws.send(json.dumps({"type":"register","name":"test_1","pass":pw}))
            msg = await asyncio.wait_for(ws.recv(), timeout=2.0)
            d = json.loads(msg)
            if d.get("type") == "signedin":
                print(f"  registered test_1 with pw='{pw}'")
                signed_in = True
                break
            # else try login with this pw
            await ws.send(json.dumps({"type":"login","name":"test_1","pass":pw}))
            msg = await asyncio.wait_for(ws.recv(), timeout=2.0)
            d = json.loads(msg)
            if d.get("type") == "signedin":
                print(f"  logged in as test_1 with pw='{pw}'")
                signed_in = True
                break
            print(f"  pw '{pw}' failed: {d.get('text')}")
        if not signed_in:
            print("  FAILED: Could not sign in as test_1. Manually log in with the correct pw or wipe NVS.")
            return

    print("\n=== TEST 2: Open chat WS, joinChat, check mod ===")
    async with websockets.connect(URL) as ws:
        await expect(ws, "ready")
        await ws.send(json.dumps({"type":"joinChat"}))
        d, _seen = await expect(ws, "loggedin")
        print(f"  loggedin as {d['name']}")
        await drain(ws)
        await ws.send(json.dumps({"type":"checkMod"}))
        d, _seen = await expect(ws, "modStatus")
        print(f"  modStatus: isMod={d['isMod']}")
        assert d["isMod"] == True, "test_1 should be a moderator"

        print("\n=== TEST 3a: Cleanup any leftover game from previous run ===")
        await ws.send(json.dumps({"type":"leaveGame"}))
        await drain(ws)

        print("\n=== TEST 3b: Get lobbies (should be empty) ===")
        await ws.send(json.dumps({"type":"getLobbies"}))
        d, _seen = await expect(ws, "lobbies")
        print(f"  lobbies: {d['games']}")
        assert d["games"] == [], f"expected no games, got {d['games']}"

        print("\n=== TEST 4: Host a game ===")
        await ws.send(json.dumps({"type":"hostGame","deck":36,"jokers":False}))
        d, seen = await expect(ws, "hostedGame")
        code = d["code"]
        print(f"  hosted game code={code} (msgs seen: {seen})")
        assert len(code) == 8 and code.isdigit(), f"expected 8-digit code, got {code}"
        assert "lobbyUpdate" in seen, f"expected lobbyUpdate before hostedGame, got {seen}"
        assert "lobbies" in seen, f"expected lobbies broadcast before hostedGame, got {seen}"

        print("\n=== TEST 5: Try to host again (should fail) ===")
        await ws.send(json.dumps({"type":"hostGame","deck":36,"jokers":False}))
        d, _seen = await expect(ws, "error")
        print(f"  error (expected): {d['text']}")
        assert "Already" in d["text"] or "already" in d["text"]

        print("\n=== TEST 6: Try to start game with only 1 player ===")
        await ws.send(json.dumps({"type":"startGame"}))
        d, _seen = await expect(ws, "error")
        print(f"  error (expected): {d['text']}")
        assert "2+" in d["text"] or "Need" in d["text"]

        print("\n=== TEST 7: Leave the game (host leaves => game removed) ===")
        await ws.send(json.dumps({"type":"leaveGame"}))
        msgs = await drain(ws)
        print(f"  msgs: {[m['type'] for m in msgs]}")

        print("\n=== TEST 8: Verify game is gone ===")
        await ws.send(json.dumps({"type":"getLobbies"}))
        d, _seen = await expect(ws, "lobbies")
        print(f"  lobbies after leave: {d['games']}")
        assert d["games"] == [], "expected no games after host leaves"

        print("\n=== TEST 9: Host with jokers + 24-card deck ===")
        await ws.send(json.dumps({"type":"hostGame","deck":24,"jokers":True}))
        d, _seen = await expect(ws, "hostedGame")
        code = d["code"]
        print(f"  hosted code={code} (24-card + jokers)")
        await drain(ws)
        await ws.send(json.dumps({"type":"getLobbies"}))
        d, _seen = await expect(ws, "lobbies")
        g = d["games"][0]
        assert g["deck"] == 24 and g["jokers"] == True
        print(f"  lobby reflects: deck={g['deck']} jokers={g['jokers']}")

        print("\n=== TEST 10: Join own game (should fail) ===")
        await ws.send(json.dumps({"type":"joinGame","code":code}))
        d, _seen = await expect(ws, "error")
        print(f"  error (expected): {d['text']}")
        assert "Already" in d["text"] or "already" in d["text"]

        print("\n=== TEST 11: Cleanup leave ===")
        await ws.send(json.dumps({"type":"leaveGame"}))
        await drain(ws)

    print("\n=== ALL TESTS PASSED ===")

asyncio.run(main())
