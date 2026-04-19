# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 offline chat server. The board brings up its own WiFi AP (`ESP32-Chat` / `chatroom1`), runs a captive-portal HTTP + WebSocket server at `192.168.4.1`, and mirrors the chat on a 1.54" e-paper display. No internet is involved at runtime.

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

## Architecture

**Three tiers of user state** (do not conflate them when editing):

1. **Accounts** — persistent, stored in NVS (`Preferences` namespace `"chat"`). Key scheme: `ac_count` + `ac_name_N` / `ac_pass_N`. Cap: `MAX_ACCOUNTS = 24`. Passwords are plaintext — noted, not yet hashed.
2. **Users** — in-RAM active WebSocket sessions, keyed by `AsyncWebSocketClient::id()`. Cap: `MAX_USERS = 8` (this is the chat-room size, not account count).
3. **History** — in-RAM ring buffer of last `MAX_HISTORY = 12` messages, replayed to each newly-logged-in client.

**WebSocket protocol** (JSON over `/ws`). Inbound `type`: `register` | `login` | `msg`. Outbound `type`: `ready` | `loggedin` | `error` | `msg` | `system` | `users`. All message building is done via string concat in `makeJson` / `buildUserList` — there is no outbound JSON serializer, so any new field must be added there carefully (quote-escaping is manual).

**Web UI** is a single HTML+CSS+JS blob in a `PROGMEM` raw string literal (`CHAT_HTML`) inside `main.cpp`. Editing the UI = editing that literal. A second literal `CAPTIVE_HTML` is served to OS captive-portal probe URLs (`/generate_204`, `/hotspot-detect.html`, etc.) so phones auto-open the chat.

**E-paper rendering** uses `GxEPD2` + `U8g2_for_Adafruit_GFX` (needed for the Cyrillic font `u8g2_font_6x13_t_cyrillic`). Pins: `CS=5, DC=17, RST=16, BUSY=4`. Redraws are throttled by a `needsRedraw` dirty flag + `REDRAW_INTERVAL_MS = 2500` min interval — keep that throttle when adding redraw triggers; full-refresh e-paper wears with frequent updates. Line buffer `epdLines[EPD_LINES=13]` is a separate ring from `history[]`.

**DNS + captive portal** — `DNSServer` wildcards all names to the AP IP; `server.onNotFound` redirects stray requests. Together these make any URL the phone tries resolve to the chat page.

## Libraries (pinned in `platformio.ini`)

- `ESP32Async/AsyncTCP` + `ESP32Async/ESPAsyncWebServer` — async HTTP + WebSocket
- `ZinggJM/GxEPD2 @ 1.5.8` — e-paper driver (pinned exact version)
- `adafruit/Adafruit GFX Library` + `olikraus/U8g2` + `olikraus/U8g2_for_Adafruit_GFX` — fonts/graphics (U8g2 needed for Cyrillic)
- `bblanchon/ArduinoJson ^6.21.5` — **v6 API** (`DynamicJsonDocument`). Do not upgrade to v7 without porting the deserializer in `onWsEvent`.

## Conventions observed in the code

- Input sanitization on register: strips `"`, `<`, `>` from names; in `msg` handler `"` in text is replaced with `'` to keep the manual JSON builder safe. Preserve this if adding new text fields.
- `loop()` must stay non-blocking: it services `dnsServer.processNextRequest()`, `ws.cleanupClients()`, and the throttled e-paper redraw. Any long work belongs off the loop path.
