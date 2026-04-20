# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 offline chat server. The board brings up its own WiFi AP (`ESP32-Chat` / `chatroom1`), runs a captive-portal HTTP + WebSocket server reachable at `192.168.4.1` or `http://chat.local` (mDNS), and mirrors the chat on a 1.54" e-paper display. No internet is involved at runtime.

## Build / flash / monitor

PlatformIO project — environment `esp32dev`, Arduino framework, serial at 115200.

```bash
pio run                 # build
pio run -t upload       # flash connected board
pio device monitor      # serial monitor (115200)
pio run -t clean        # clean build
pio run -t uploadfs     # (unused — no SPIFFS/LittleFS in this project)
```

Everything lives in `src/main.cpp` as a single translation unit. `include/`, `lib/`, `test/` are empty PlatformIO template dirs.

## Headless testing from this dev machine

The captive portal + chat can be driven without a phone, since this machine reaches the internet over Ethernet and the WiFi radio is free. The `ESP32-Chat` profile is already saved on Windows, so no profile XML is needed.

```bash
netsh wlan connect name="ESP32-Chat"            # join the AP (~2-5s)
netsh wlan show interfaces | grep -E "SSID|State"
curl -s http://192.168.4.1/ | head              # sign-in HTML
curl -s http://chat.local/  | head              # mDNS resolves
netsh wlan disconnect                           # release the radio when done
```

For protocol-level verification, write a throwaway `asyncio` + `websockets` script (install once: `py -m pip install websockets`) and drive `ws://192.168.4.1/ws` end-to-end. The happy-path sequence is:

1. recv `{"type":"ready"}`
2. send `{"type":"register"|"login","name":..,"pass":..}` → recv `{"type":"signedin","name":..,"new":bool}`  *(popup auth — does not join chat)*
3. open a **second** WS (simulates the user's real browser), recv `ready`, send `{"type":"joinChat"}` → recv `loggedin` + `users` + system join broadcast *(server logs in by IP, no creds re-sent)*
4. send `{"type":"msg","text":..}` → recv echoed `{"type":"msg",...}`
5. open a **third** WS, send `joinChat` → the second WS receives `{"type":"taken"}` and is closed (takeover)
6. send `{"type":"logout"}` → recv `{"type":"loggedout"}`; subsequent `joinChat` from the same IP returns `{"type":"error","code":"nosignin"}` (logout clears the IP allowlist)

When draining messages, catch both `asyncio.TimeoutError` **and** `websockets.exceptions.ConnectionClosed` — `taken` arrives immediately before the close, and a naive recv loop will miss it.

## Architecture

**Four tiers of user state** (do not conflate them when editing):

1. **Accounts** — persistent, stored in NVS (`Preferences` namespace `"chat"`). Key scheme: `ac_count` + `ac_name_N` / `ac_pass_N`. Cap: `MAX_ACCOUNTS = 24`. Passwords are plaintext — noted, not yet hashed.
2. **Signed-in IPs** — in-RAM `signedIPs[] = {IPAddress, name}`, cap `MAX_SIGNED_IPS = 16`. Populated when the captive popup completes `register`/`login`; cleared by `logout`. Two roles: (a) the chat page's `joinChat` looks up the username by request IP so users don't re-enter creds in their real browser; (b) the OS captive-probe handlers consult this list to return the per-OS success body and clear the "Sign in to network" badge.
3. **Users** — in-RAM active WebSocket *chat* sessions, keyed by `AsyncWebSocketClient::id()`. Cap: `MAX_USERS = 8` (chat-room size, not account count). The `clientId` field can be **reassigned mid-session** by the takeover path in `joinChat`: a fresh WS for the same name swaps in, the old WS gets `{"type":"taken"}` and is closed. The disconnect handler is safe against this because `findUser(oldId)` returns null after the swap.
4. **History** — in-RAM ring buffer of last `MAX_HISTORY = 12` messages, replayed to each client when they `joinChat` (not on popup `login`/`register`, which don't enter the chat room).

**WebSocket protocol** (JSON over `/ws`).
Inbound `type`: `register` | `login` | `joinChat` | `logout` | `msg`. The first two are popup auth only — they validate creds and add to `signedIPs[]` without joining the chat room. `joinChat` is what the chat page sends; the server picks the username from the request IP.
Outbound `type`: `ready` | `signedin` | `loggedin` | `loggedout` | `taken` | `error` | `msg` | `system` | `users`. `signedin` is the popup's success event; `loggedin` is the chat-page join event; `taken` notifies a kicked WS just before close. `error` may carry a `code` field — currently `"nosignin"` (returned by `joinChat` when the request IP isn't in `signedIPs[]`); the chat page uses it to bounce back to `/`.
All outbound message building is string concat in `makeJson` / `buildUserList` — there is no outbound JSON serializer, so any new field must be added there carefully (quote-escaping is manual).

**Web UI** lives in three `PROGMEM` raw string literals inside `main.cpp`:
- `SIGNIN_HTML` (served at `/`) — the captive-popup auth UI; on `signedin` it shows a success screen with the `chat.local` URL, **no auto-redirect to chat** (the popup is intentionally sandboxed away from the chat).
- `CHAT_HTML` (served at `/chat`) — the actual chat UI for the user's real browser; on WS `ready` it sends `joinChat` (no creds, server logs in by IP).
- `CAPTIVE_HTML` — served to OS captive-portal probe URLs (`/generate_204`, `/hotspot-detect.html`, etc.) for clients **not yet** in `signedIPs[]`, so phones auto-open the popup.
Editing any of those UIs = editing that literal.

**E-paper rendering** uses `GxEPD2` + `U8g2_for_Adafruit_GFX` (needed for the Cyrillic font `u8g2_font_6x13_t_cyrillic`). Pins: `CS=5, DC=17, RST=16, BUSY=4`. Redraws are throttled by a `needsRedraw` dirty flag + `REDRAW_INTERVAL_MS = 2500` min interval — keep that throttle when adding redraw triggers; full-refresh e-paper wears with frequent updates. Line buffer `epdLines[EPD_LINES=13]` is a separate ring from `history[]`.

**DNS + mDNS + captive portal** — `DNSServer` wildcards all names to the AP IP, and `MDNS.begin("chat")` advertises `chat.local` on the AP interface. `server.onNotFound` 302s stray HTTP requests to `/` (which itself 302s to `/chat` for already-signed-in IPs). The captive-probe routes (`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`, `/connecttest.txt`, `/canonical.html`, etc.) are **per-IP**: until that client is in `signedIPs[]` they get `CAPTIVE_HTML` (popup opens); after sign-in they get the OS-specific success body (`HTTP 204` for Android, the `<HTML>...Success...</HTML>` blob for Apple, `Microsoft NCSI` for Windows, `success` for Firefox) so the captive badge clears.

## Libraries (pinned in `platformio.ini`)

- `ESP32Async/AsyncTCP` + `ESP32Async/ESPAsyncWebServer` — async HTTP + WebSocket
- `ZinggJM/GxEPD2 @ 1.5.8` — e-paper driver (pinned exact version)
- `adafruit/Adafruit GFX Library` + `olikraus/U8g2` + `olikraus/U8g2_for_Adafruit_GFX` — fonts/graphics (U8g2 needed for Cyrillic)
- `bblanchon/ArduinoJson ^6.21.5` — **v6 API** (`DynamicJsonDocument`). Do not upgrade to v7 without porting the deserializer in `onWsEvent`.

## Conventions observed in the code

- Input sanitization on register: strips `"`, `<`, `>` from names; in `msg` handler `"` in text is replaced with `'` to keep the manual JSON builder safe. Preserve this if adding new text fields.
- `loop()` must stay non-blocking: it services `dnsServer.processNextRequest()`, `ws.cleanupClients()`, and the throttled e-paper redraw. Any long work belongs off the loop path.
