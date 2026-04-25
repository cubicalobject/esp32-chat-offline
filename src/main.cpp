#include <Wire.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

// ── E-Paper pins ──────────────────────────────────────────
#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4

// ── Display ───────────────────────────────────────────────
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT>* displayPtr = nullptr;
#define display (*displayPtr)
U8G2_FOR_ADAFRUIT_GFX u8f;

// ── AP + DNS ──────────────────────────────────────────────
const char* AP_SSID  = "ESP32-Chat";
const char* AP_PASS  = "chatroom1";
const char* MDNS_NAME = "chat";          // → http://chat.local
DNSServer   dnsServer;
#define DNS_PORT 53

// ── Server + WebSocket ────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket  ws("/ws");

// ── NVS ───────────────────────────────────────────────────
Preferences prefs;

// ── Account storage ───────────────────────────────────────
// Stored in NVS — survives reboots forever until deleted
// Key scheme:
//   "ac_count"   → number of accounts
//   "ac_name_N"  → username for account N
//   "ac_pass_N"  → password for account N
#define MAX_ACCOUNTS 24
struct Account {
  String name;
  String password;
};
Account accounts[MAX_ACCOUNTS];
int accountCount = 0;

// ── Active users (logged-in sessions in RAM) ──────────────
#define MAX_USERS 8
struct User {
  uint32_t clientId;
  String   name;
  bool     active;
};
User users[MAX_USERS];

// ── Captive-portal "signed in" IP allowlist ──────────────
// Sign-in (popup) and chat-join (browser) are decoupled: the popup
// authenticates and stores {ip → username}, then the chat page in
// the user's real browser performs `joinChat`, which logs the user
// in by IP without re-asking for credentials. The same allowlist
// also drives OS captive-probe responses so the "Sign in to
// network" badge clears.
#define MAX_SIGNED_IPS 16
struct SignedIP { IPAddress ip; String name; };
SignedIP signedIPs[MAX_SIGNED_IPS];
int signedIPCount = 0;

int findSignedIPIndex(const IPAddress& ip) {
  for (int i = 0; i < signedIPCount; i++)
    if (signedIPs[i].ip == ip) return i;
  return -1;
}
bool isSignedIn(const IPAddress& ip) { return findSignedIPIndex(ip) >= 0; }
String lookupSignedName(const IPAddress& ip) {
  int i = findSignedIPIndex(ip);
  return i >= 0 ? signedIPs[i].name : String();
}
void markSignedIn(const IPAddress& ip, const String& name) {
  int i = findSignedIPIndex(ip);
  if (i >= 0) { signedIPs[i].name = name; return; }
  if (signedIPCount < MAX_SIGNED_IPS) {
    signedIPs[signedIPCount++] = {ip, name};
  } else {
    for (int j = 0; j < MAX_SIGNED_IPS-1; j++) signedIPs[j] = signedIPs[j+1];
    signedIPs[MAX_SIGNED_IPS-1] = {ip, name};
  }
}
void clearSignedIn(const IPAddress& ip) {
  int i = findSignedIPIndex(ip);
  if (i < 0) return;
  for (int j = i; j < signedIPCount-1; j++) signedIPs[j] = signedIPs[j+1];
  signedIPCount--;
}

// ── Chat history ──────────────────────────────────────────
#define MAX_HISTORY 12
struct Msg { String from; String text; };
Msg history[MAX_HISTORY];
int historyCount = 0;

// ── E-Paper buffer ────────────────────────────────────────
#define EPD_LINES 13
String epdLines[EPD_LINES];
int    epdLineCount = 0;
int    epdUserCount = 0;
bool          needsRedraw = false;
unsigned long lastDraw    = 0;
#define REDRAW_INTERVAL_MS 2500

// ─────────────────────────────────────────────────────────
// Account NVS helpers
// ─────────────────────────────────────────────────────────
void saveAccountsNVS() {
  prefs.begin("chat", false);
  prefs.putUChar("ac_count", (uint8_t)accountCount);
  for (int i = 0; i < accountCount; i++) {
    prefs.putString(("ac_name_" + String(i)).c_str(), accounts[i].name);
    prefs.putString(("ac_pass_" + String(i)).c_str(), accounts[i].password);
  }
  prefs.end();
  Serial.println("[NVS] Saved " + String(accountCount) + " accounts.");
}

void loadAccountsNVS() {
  prefs.begin("chat", true);
  int count = (int)prefs.getUChar("ac_count", 0);
  if (count > MAX_ACCOUNTS) count = MAX_ACCOUNTS;
  accountCount = 0;
  for (int i = 0; i < count; i++) {
    String name = prefs.getString(("ac_name_" + String(i)).c_str(), "");
    String pass = prefs.getString(("ac_pass_" + String(i)).c_str(), "");
    if (name.length() == 0) continue;
    accounts[accountCount++] = {name, pass};
    Serial.println("[NVS] Loaded account: " + name);
  }
  prefs.end();
  Serial.println("[NVS] " + String(accountCount) + " accounts loaded.");
}

// ─────────────────────────────────────────────────────────
// Account helpers
// ─────────────────────────────────────────────────────────
Account* findAccount(const String& name) {
  for (int i = 0; i < accountCount; i++)
    if (accounts[i].name.equalsIgnoreCase(name)) return &accounts[i];
  return nullptr;
}

bool createAccount(const String& name, const String& password) {
  if (accountCount >= MAX_ACCOUNTS) return false;
  if (findAccount(name)) return false;   // name already exists
  accounts[accountCount++] = {name, password};
  saveAccountsNVS();
  Serial.println("[Account] Created: " + name);
  return true;
}

bool checkPassword(const String& name, const String& password) {
  Account* a = findAccount(name);
  if (!a) return false;
  return a->password == password;
}

void printAccounts() {
  Serial.println("=== Accounts ===");
  for (int i = 0; i < accountCount; i++)
    Serial.println("  " + accounts[i].name);
  Serial.println("================");
}

// ─────────────────────────────────────────────────────────
// User (active session) helpers
// ─────────────────────────────────────────────────────────
User* findUser(uint32_t id) {
  for (int i = 0; i < MAX_USERS; i++)
    if (users[i].active && users[i].clientId == id) return &users[i];
  return nullptr;
}
bool nameOnline(const String& name) {
  for (int i = 0; i < MAX_USERS; i++)
    if (users[i].active && users[i].name.equalsIgnoreCase(name)) return true;
  return false;
}
int countActive() {
  int n = 0;
  for (int i = 0; i < MAX_USERS; i++) if (users[i].active) n++;
  return n;
}
bool loginUser(uint32_t id, const String& name) {
  if (nameOnline(name)) return false;
  for (int i = 0; i < MAX_USERS; i++)
    if (!users[i].active) { users[i] = {id, name, true}; return true; }
  return false;
}
void logoutUser(uint32_t id) {
  for (int i = 0; i < MAX_USERS; i++)
    if (users[i].active && users[i].clientId == id)
      users[i].active = false;
}

// ─────────────────────────────────────────────────────────
// JSON helpers
// ─────────────────────────────────────────────────────────
String buildUserList() {
  String list = ",\"users\":["; bool first = true;
  for (int i = 0; i < MAX_USERS; i++) {
    if (!users[i].active) continue;
    if (!first) list += ",";
    list += "\"" + users[i].name + "\""; first = false;
  }
  return list + "]";
}
String makeJson(const char* type, const String& from,
                const String& text, const String& extra = "") {
  return "{\"type\":\"" + String(type) + "\",\"from\":\"" + from +
         "\",\"text\":\"" + text + "\"" + extra + "}";
}
void broadcast(const String& json) { ws.textAll(json); }
void broadcastUserList() { broadcast("{\"type\":\"users\"" + buildUserList() + "}"); }
void storeHistory(const String& from, const String& text) {
  if (historyCount < MAX_HISTORY) history[historyCount++] = {from, text};
  else {
    for (int i = 0; i < MAX_HISTORY-1; i++) history[i] = history[i+1];
    history[MAX_HISTORY-1] = {from, text};
  }
}
void sendHistory(uint32_t id) {
  for (int i = 0; i < historyCount; i++)
    ws.text(id, makeJson("msg", history[i].from, history[i].text));
}

// ─────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────
void pushLine(const String& from, const String& text) {
  String line = from + ": " + text;
  if (epdLineCount < EPD_LINES) epdLines[epdLineCount++] = line;
  else {
    for (int i = 0; i < EPD_LINES-1; i++) epdLines[i] = epdLines[i+1];
    epdLines[EPD_LINES-1] = line;
  }
  needsRedraw = true;
}
void drawEPaper() {
  display.setRotation(0);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8f.setFont(u8g2_font_6x13_t_cyrillic);
    u8f.setForegroundColor(GxEPD_BLACK);
    u8f.setBackgroundColor(GxEPD_WHITE);
    u8f.setCursor(2, 13); u8f.print("ESP32 Chat");
    String uc = String(epdUserCount) + " online";
    u8f.setCursor(198 - u8f.getUTF8Width(uc.c_str()), 13);
    u8f.print(uc.c_str());
    display.drawLine(0, 17, 199, 17, GxEPD_BLACK);
    if (epdLineCount == 0) {
      u8f.setCursor(30, 105); u8f.print("No messages yet");
    } else {
      for (int i = 0; i < epdLineCount; i++) {
        String line = epdLines[i];
        while (line.length() > 0 && u8f.getUTF8Width(line.c_str()) > 196) {
          int cut = line.length()-1;
          while (cut > 0 && (line[cut] & 0xC0) == 0x80) cut--;
          line = line.substring(0, cut);
        }
        u8f.setCursor(2, 30 + i*12); u8f.print(line.c_str());
      }
    }
    display.drawLine(0, 189, 199, 189, GxEPD_BLACK);
    u8f.setFont(u8g2_font_5x7_t_cyrillic);
    u8f.setCursor(2, 198);
    u8f.print("WiFi: "); u8f.print(AP_SSID); u8f.print("  192.168.4.1");
  } while (display.nextPage());
}
void drawBootScreen(const String& ip) {
  display.setRotation(0);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8f.setFont(u8g2_font_6x13_t_cyrillic);
    u8f.setForegroundColor(GxEPD_BLACK);
    u8f.setBackgroundColor(GxEPD_WHITE);
    u8f.setCursor(30, 20);  u8f.print("ESP32 Chat");
    display.drawLine(0, 25, 199, 25, GxEPD_BLACK);
    u8f.setCursor(2, 50);   u8f.print("Connect to WiFi:");
    u8f.setCursor(2, 66);   u8f.print("SSID: "); u8f.print(AP_SSID);
    u8f.setCursor(2, 82);   u8f.print("Pass: "); u8f.print(AP_PASS);
    display.drawLine(0, 90, 199, 90, GxEPD_BLACK);
    u8f.setCursor(2, 110);  u8f.print("Browser opens auto!");
    u8f.setFont(u8g2_font_9x15_t_cyrillic);
    u8f.setCursor(10, 132); u8f.print("chat.local");
    u8f.setFont(u8g2_font_6x13_t_cyrillic);
    u8f.setCursor(2, 150);  u8f.print("or "); u8f.print(ip.c_str());
    u8f.setCursor(2, 170);  u8f.print("Waiting for users...");
  } while (display.nextPage());
}

// ─────────────────────────────────────────────────────────
// WebSocket events
// ─────────────────────────────────────────────────────────
void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {

  if (type == WS_EVT_CONNECT) {
    // Just tell the client we're ready — it will show login/register UI
    ws.text(c->id(), "{\"type\":\"ready\"}");

  } else if (type == WS_EVT_DISCONNECT) {
    User* u = findUser(c->id());
    if (u) {
      pushLine("--", u->name + " left");
      String leave = makeJson("system", "Server",
                              u->name + " left.", buildUserList());
      logoutUser(c->id());
      epdUserCount = countActive();
      broadcast(leave);
      broadcastUserList();
    }

  } else if (type == WS_EVT_DATA) {
    String msg = "";
    msg.reserve(len+1);
    for (size_t i = 0; i < len; i++) msg += (char)data[i];

    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, msg)) return;
    const char* mtype = doc["type"] | "";

    // ── Register new account (popup auth only — no chat join) ──
    if (strcmp(mtype, "register") == 0) {
      String name = String(doc["name"] | "");
      String pass = String(doc["pass"] | "");
      name.trim(); pass.trim();

      // Sanitise name
      name.replace("\"",""); name.replace("<",""); name.replace(">","");

      if (name.length() < 1 || name.length() > 16) {
        ws.text(c->id(), "{\"type\":\"error\",\"text\":\"Name must be 1–16 characters.\"}");
        return;
      }
      if (pass.length() < 4 || pass.length() > 32) {
        ws.text(c->id(), "{\"type\":\"error\",\"text\":\"Password must be 4–32 characters.\"}");
        return;
      }
      if (findAccount(name)) {
        ws.text(c->id(), "{\"type\":\"error\",\"text\":\"Username already taken.\"}");
        return;
      }
      if (!createAccount(name, pass)) {
        ws.text(c->id(), "{\"type\":\"error\",\"text\":\"Account limit reached.\"}");
        return;
      }
      markSignedIn(c->remoteIP(), name);
      ws.text(c->id(), "{\"type\":\"signedin\",\"name\":\"" + name + "\",\"new\":true}");
      printAccounts();

    // ── Sign in to existing account (popup auth only — no chat join) ──
    } else if (strcmp(mtype, "login") == 0) {
      String name = String(doc["name"] | "");
      String pass = String(doc["pass"] | "");
      name.trim(); pass.trim();

      if (!findAccount(name)) {
        ws.text(c->id(), "{\"type\":\"error\",\"text\":\"No account with that name.\"}");
        return;
      }
      if (!checkPassword(name, pass)) {
        ws.text(c->id(), "{\"type\":\"error\",\"text\":\"Wrong password.\"}");
        return;
      }
      // Use the canonical-cased stored name
      Account* a = findAccount(name);
      String storedName = a ? a->name : name;
      markSignedIn(c->remoteIP(), storedName);
      ws.text(c->id(), "{\"type\":\"signedin\",\"name\":\"" + storedName + "\"}");

    // ── Join chat from already-signed-in IP (chat page) ───
    } else if (strcmp(mtype, "joinChat") == 0) {
      IPAddress ip = c->remoteIP();
      String name = lookupSignedName(ip);
      if (name.length() == 0) {
        ws.text(c->id(), "{\"type\":\"error\",\"code\":\"nosignin\",\"text\":\"Please sign in first.\"}");
        return;
      }
      // Takeover: if this name is already in chat (popup, other tab,
      // stale WS), reassign the slot to this WS and close the old one.
      bool takeover = false;
      for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && users[i].name.equalsIgnoreCase(name)) {
          uint32_t oldId = users[i].clientId;
          if (oldId == c->id()) break; // same WS, nothing to do
          users[i].clientId = c->id();
          AsyncWebSocketClient* old = ws.client(oldId);
          if (old) {
            old->text("{\"type\":\"taken\"}");
            old->close();
          }
          takeover = true;
          break;
        }
      }
      if (!takeover) {
        if (!loginUser(c->id(), name)) {
          ws.text(c->id(), "{\"type\":\"error\",\"text\":\"Chat room is full (max 8).\"}");
          return;
        }
      }
      ws.text(c->id(), "{\"type\":\"loggedin\",\"name\":\"" + name + "\"}");
      sendHistory(c->id());
      // Always send the new client an up-to-date user list
      ws.text(c->id(), "{\"type\":\"users\"" + buildUserList() + "}");
      if (!takeover) {
        broadcast(makeJson("system","Server", name + " joined!", buildUserList()));
        broadcastUserList();
        pushLine("--", name + " joined");
        epdUserCount = countActive();
        needsRedraw = true;
      }

    // ── Logout: clear chat + IP allowlist entry ───────────
    } else if (strcmp(mtype, "logout") == 0) {
      User* u = findUser(c->id());
      if (u) {
        String name = u->name;
        logoutUser(c->id());
        epdUserCount = countActive();
        pushLine("--", name + " left");
        broadcast(makeJson("system","Server", name + " left.", buildUserList()));
        broadcastUserList();
      }
      clearSignedIn(c->remoteIP());
      ws.text(c->id(), "{\"type\":\"loggedout\"}");

    // ── Chat message ──────────────────────────────────────
    } else if (strcmp(mtype, "msg") == 0) {
      User* u = findUser(c->id());
      if (!u) {
        ws.text(c->id(), "{\"type\":\"error\",\"text\":\"Not logged in.\"}");
        return;
      }
      String text = String(doc["text"] | "");
      text.trim(); text.replace("\"","'");
      if (text.length() == 0 || text.length() > 200) return;

      String toName = String(doc["to"] | "");
      toName.trim();

      if (toName.length() > 0) {
        // DM: send only to sender + recipient
        uint32_t targetId = 0; bool found = false;
        for (int i = 0; i < MAX_USERS; i++) {
          if (users[i].active && users[i].name.equalsIgnoreCase(toName)) {
            targetId = users[i].clientId; toName = users[i].name; found = true; break;
          }
        }
        if (!found) {
          ws.text(c->id(), "{\"type\":\"error\",\"text\":\"" + toName + " is not online.\"}");
          return;
        }
        String dmJson = makeJson("msg", u->name, text, ",\"to\":\"" + toName + "\",\"dm\":true");
        ws.text(c->id(), dmJson);
        if (targetId != c->id()) ws.text(targetId, dmJson);
      } else {
        // Public message — scan for @mentions
        String mentionExtra = ""; bool firstM = true;
        for (int i = 0; i < MAX_USERS; i++) {
          if (!users[i].active) continue;
          String needleLow = "@" + users[i].name; needleLow.toLowerCase();
          String textLow = text; textLow.toLowerCase();
          if (textLow.indexOf(needleLow) >= 0) {
            if (firstM) { mentionExtra += ",\"mentions\":["; firstM = false; }
            else mentionExtra += ",";
            mentionExtra += "\"" + users[i].name + "\"";
          }
        }
        if (!firstM) mentionExtra += "]";
        storeHistory(u->name, text);
        broadcast(makeJson("msg", u->name, text, mentionExtra));
        pushLine(u->name, text);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────
// Sign-in page HTML  (served at /)
// ─────────────────────────────────────────────────────────
const char SIGNIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>ESP32 Sign In</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
:root{
  --bg:#0d0d0d;--surface:#141414;--border:#222;
  --text:#e8e8e8;--muted:#555;--accent:#7c6af7;--err:#ef4444;
}
body{background:var(--bg);color:var(--text);
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  height:100dvh;display:flex;flex-direction:column;overflow:hidden;}
#auth{flex:1;display:flex;flex-direction:column;align-items:center;
  justify-content:center;padding:28px 24px;gap:0;}
#auth h1{font-size:22px;font-weight:600;margin-bottom:6px;}
#auth .sub{font-size:13px;color:var(--muted);margin-bottom:24px;text-align:center;}
.tabs{display:flex;width:100%;max-width:300px;margin-bottom:20px;
  background:var(--surface);border-radius:10px;padding:3px;gap:3px;}
.tab{flex:1;padding:9px;border:none;border-radius:8px;font-size:13px;
  font-weight:500;cursor:pointer;background:transparent;
  color:var(--muted);transition:all 0.2s;}
.tab.active{background:var(--accent);color:#fff;}
.field{width:100%;max-width:300px;margin-bottom:10px;}
.field input{width:100%;padding:13px 14px;background:var(--surface);
  border:1px solid var(--border);border-radius:10px;color:var(--text);
  font-size:15px;outline:none;transition:border-color 0.2s;}
.field input:focus{border-color:var(--accent);}
.field input.error{border-color:var(--err);}
.field label{display:block;font-size:11px;color:var(--muted);
  margin-bottom:5px;padding-left:2px;letter-spacing:0.05em;text-transform:uppercase;}
#authBtn{width:100%;max-width:300px;padding:13px;border:none;
  border-radius:10px;background:var(--accent);color:#fff;
  font-size:15px;font-weight:500;cursor:pointer;margin-top:4px;}
#authBtn:active{opacity:0.8;}
#authBtn:disabled{opacity:0.5;cursor:default;}
#authErr{font-size:13px;color:var(--err);min-height:18px;
  text-align:center;margin-top:8px;max-width:300px;}
#authNote{font-size:12px;color:#333;text-align:center;
  margin-top:10px;max-width:300px;line-height:1.5;}

#done{display:none;flex:1;flex-direction:column;align-items:center;
  justify-content:center;padding:28px 24px;text-align:center;}
#done .check{width:64px;height:64px;border-radius:50%;
  background:var(--accent);color:#fff;display:flex;align-items:center;
  justify-content:center;font-size:32px;margin-bottom:18px;}
#done h2{font-size:20px;font-weight:600;margin-bottom:6px;}
#done .who{font-size:13px;color:var(--muted);margin-bottom:26px;}
#done .step{font-size:14px;color:var(--text);max-width:300px;
  line-height:1.55;margin-bottom:14px;}
#done .url{display:block;margin-top:8px;color:var(--accent);
  font-family:monospace;font-size:16px;word-break:break-all;}
#done .alt{font-size:12px;color:#444;margin-top:24px;}
#done .alt a{color:var(--muted);text-decoration:underline;}
</style>
</head>
<body>
<div id="auth">
  <h1>ESP32 Sign In</h1>
  <p class="sub">Local chat — no internet needed.</p>

  <div class="tabs">
    <button class="tab active" id="tabLogin"  onclick="switchTab('login')">Log in</button>
    <button class="tab"        id="tabReg"    onclick="switchTab('register')">Register</button>
  </div>

  <div class="field">
    <label>Username</label>
    <input id="nameInput" type="text" maxlength="16"
      placeholder="Your name..." autocomplete="username" spellcheck="false">
  </div>
  <div class="field">
    <label>Password</label>
    <input id="passInput" type="password" maxlength="32"
      placeholder="Password..." autocomplete="current-password">
  </div>

  <button id="authBtn" disabled>Connecting...</button>
  <div id="authErr"></div>
  <div id="authNote">Don't have an account? Switch to Register.</div>
</div>

<div id="done">
  <div class="check">&#10003;</div>
  <h2 id="doneTitle">Signed in</h2>
  <div class="who" id="doneWho"></div>
  <div class="step">
    Open your phone's browser and go to
    <span class="url">http://chat.local</span>
    <span style="display:block;font-size:11px;color:#444;margin-top:6px;">
      (or <span style="color:#7c6af7;font-family:monospace;">http://192.168.4.1</span>)
    </span>
  </div>
  <div class="alt">
    On a desktop browser? <a href="/chat">Open chat here</a>
  </div>
</div>

<script>
let ws;
let currentTab = 'login';

const authEl   = document.getElementById('auth');
const doneEl   = document.getElementById('done');
const doneTitle= document.getElementById('doneTitle');
const doneWho  = document.getElementById('doneWho');
const nameInput = document.getElementById('nameInput');
const passInput = document.getElementById('passInput');
const authBtn   = document.getElementById('authBtn');
const authErr   = document.getElementById('authErr');
const authNote  = document.getElementById('authNote');

function switchTab(tab) {
  currentTab = tab;
  document.getElementById('tabLogin').className = 'tab' + (tab==='login'   ? ' active' : '');
  document.getElementById('tabReg')  .className = 'tab' + (tab==='register'? ' active' : '');
  authBtn.textContent = tab === 'login' ? 'Log in' : 'Create account';
  authNote.textContent = tab === 'login'
    ? "Don't have an account? Switch to Register."
    : 'Already have an account? Switch to Log in.';
  passInput.autocomplete = tab === 'login' ? 'current-password' : 'new-password';
  authErr.textContent = '';
  nameInput.classList.remove('error');
  passInput.classList.remove('error');
}

function connect() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onclose = () => {
    if (doneEl.style.display !== 'flex') {
      authBtn.disabled = true;
      authBtn.textContent = 'Reconnecting...';
    }
    setTimeout(connect, 2000);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = e => handle(JSON.parse(e.data));
}
connect();

function showDone(name, isNew) {
  authEl.style.display = 'none';
  doneEl.style.display = 'flex';
  doneTitle.textContent = isNew ? 'Account created' : 'Signed in';
  doneWho.textContent = 'as ' + name;
}

function handle(d) {
  if (d.type === 'ready') {
    authBtn.disabled = false;
    authBtn.textContent = currentTab === 'login' ? 'Log in' : 'Create account';
    nameInput.focus();

  } else if (d.type === 'signedin') {
    showDone(d.name, !!d.new);

  } else if (d.type === 'error') {
    authErr.textContent = d.text;
    if (d.text.toLowerCase().includes('password')) passInput.classList.add('error');
    else nameInput.classList.add('error');
  }
}

authBtn.onclick = () => {
  const name = nameInput.value.trim();
  const pass = passInput.value;
  authErr.textContent = '';
  nameInput.classList.remove('error');
  passInput.classList.remove('error');

  if (!name) { nameInput.classList.add('error'); authErr.textContent = 'Enter a username.'; return; }
  if (!pass) { passInput.classList.add('error'); authErr.textContent = 'Enter a password.'; return; }
  if (ws.readyState === 1)
    ws.send(JSON.stringify({ type: currentTab, name, pass }));
};

[nameInput, passInput].forEach(el => {
  el.addEventListener('input', () => {
    el.classList.remove('error'); authErr.textContent = '';
  });
  el.addEventListener('keydown', e => { if (e.key === 'Enter') authBtn.click(); });
});
</script>
</body>
</html>
)rawliteral";

// ─────────────────────────────────────────────────────────
// Chat HTML  (served at /chat — requires sessionStorage creds)
// ─────────────────────────────────────────────────────────
const char CHAT_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>ESP32 Chat</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
:root{
  --bg:#0d0d0d;--surface:#141414;--border:#222;
  --text:#e8e8e8;--muted:#555;--accent:#7c6af7;
  --system:#3a3a3a;--them:#181818;--err:#ef4444;
}
body{background:var(--bg);color:var(--text);
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  height:100dvh;display:flex;flex-direction:column;overflow:hidden;}
#chat{flex:1;display:flex;flex-direction:column;overflow:hidden;}
#chatHeader{padding:12px 16px;border-bottom:1px solid var(--border);
  display:flex;align-items:center;justify-content:space-between;
  background:var(--surface);}
.hdr-left{display:flex;flex-direction:column;}
#chatHeader .title{font-size:15px;font-weight:500;}
#chatHeader .whoami{font-size:11px;color:var(--muted);}
#logoutBtn{padding:6px 12px;border:1px solid var(--border);border-radius:8px;
  background:transparent;color:var(--muted);font-size:12px;cursor:pointer;}
#onlineBar{padding:8px 14px;background:var(--surface);
  border-bottom:1px solid var(--border);font-size:12px;color:var(--muted);
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.online-name{cursor:pointer;color:var(--accent);font-weight:500;}
.online-name:hover{text-decoration:underline;}
#dmBar{padding:6px 14px;background:#12091f;border-bottom:1px solid #3a1f6e;
  display:none;align-items:center;gap:8px;font-size:13px;color:#c084fc;}
#dmBar .dm-to{flex:1;}
#dmBar button{background:none;border:none;color:var(--muted);font-size:18px;
  cursor:pointer;line-height:1;padding:0 4px;}
#dmBar button:hover{color:var(--text);}
#messages{flex:1;overflow-y:auto;padding:12px 14px;
  display:flex;flex-direction:column;gap:6px;scroll-behavior:smooth;}
.bubble-wrap{display:flex;flex-direction:column;max-width:82%;}
.bubble-wrap.me{align-self:flex-end;align-items:flex-end;}
.bubble-wrap.them{align-self:flex-start;align-items:flex-start;}
.bubble-wrap.system{align-self:center;align-items:center;max-width:100%;}
.sender{font-size:11px;color:var(--muted);margin-bottom:3px;padding:0 4px;}
.bubble{padding:9px 13px;border-radius:16px;font-size:14px;
  line-height:1.45;word-break:break-word;}
.me .bubble{background:var(--accent);color:#fff;border-bottom-right-radius:4px;}
.them .bubble{background:var(--them);border:1px solid var(--border);
  border-bottom-left-radius:4px;}
.system .bubble{background:var(--system);color:var(--muted);font-size:12px;
  border-radius:8px;padding:5px 12px;}
.me.dm .bubble{background:#9333ea;color:#fff;border-bottom-right-radius:4px;}
.them.dm .bubble{background:#1e0f33;border:1px solid #6d28d9;
  color:var(--text);border-bottom-left-radius:4px;}
.mention-tag{color:#c084fc;font-weight:600;}
#inputRow{display:flex;gap:8px;padding:10px 12px;
  border-top:1px solid var(--border);background:var(--surface);
  padding-bottom:max(10px,env(safe-area-inset-bottom));}
#msgInput{flex:1;padding:11px 14px;background:var(--bg);
  border:1px solid var(--border);border-radius:22px;
  color:var(--text);font-size:15px;outline:none;}
#msgInput:focus{border-color:var(--accent);}
#sendBtn{width:44px;height:44px;border-radius:50%;border:none;
  background:var(--accent);color:#fff;font-size:18px;
  cursor:pointer;flex-shrink:0;}
</style>
</head>
<body>
<div id="chat">
  <div id="chatHeader">
    <div class="hdr-left">
      <span class="title">ESP32 Chat</span>
      <span class="whoami" id="whoami"></span>
    </div>
    <button id="logoutBtn" onclick="logout()">Log out</button>
  </div>
  <div id="onlineBar">Online: —</div>
  <div id="dmBar">
    <span class="dm-to" id="dmLabel"></span>
    <button onclick="clearDM()" title="Cancel DM">&#x2715;</button>
  </div>
  <div id="messages"></div>
  <div id="inputRow">
    <input id="msgInput" type="text" maxlength="200"
      placeholder="Message / Сообщение..." autocomplete="off">
    <button id="sendBtn">&#10148;</button>
  </div>
</div>

<script>
let ws, myName = '', registered = false, welcomedOnce = false, retryCount = 0;
let kicked = false, dmTarget = '';
const messages  = document.getElementById('messages');
const msgInput  = document.getElementById('msgInput');
const sendBtn   = document.getElementById('sendBtn');
const onlineBar = document.getElementById('onlineBar');
const whoami    = document.getElementById('whoami');
const dmBar     = document.getElementById('dmBar');
const dmLabel   = document.getElementById('dmLabel');

if ('Notification' in window) Notification.requestPermission();

function connect() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onclose = () => { registered = false; if (kicked) return; setTimeout(connect, 2000); };
  ws.onerror = () => ws.close();
  ws.onmessage = e => handle(JSON.parse(e.data));
}
connect();

function sendJoin() {
  if (ws.readyState === 1) ws.send(JSON.stringify({ type: 'joinChat' }));
}

function handle(d) {
  if (d.type === 'ready') {
    setTimeout(sendJoin, 250);

  } else if (d.type === 'loggedin') {
    registered = true; retryCount = 0; myName = d.name;
    whoami.textContent = 'Logged in as ' + d.name;
    if (!welcomedOnce) { addSystem('Welcome, ' + d.name + '! \u2713'); welcomedOnce = true; }
    msgInput.focus();

  } else if (d.type === 'error') {
    if (d.code === 'nosignin') { location.replace('/'); return; }
    if (d.text && d.text.toLowerCase().includes('already in chat') && retryCount < 4) {
      retryCount++; setTimeout(sendJoin, 400); return;
    }
    addSystem('Error: ' + (d.text || 'unknown'));

  } else if (d.type === 'loggedout') {
    location.replace('/');

  } else if (d.type === 'taken') {
    kicked = true; registered = false; msgInput.disabled = true; sendBtn.disabled = true;
    addSystem('This chat was opened in another tab \u2014 this tab is now inactive.');

  } else if (d.type === 'msg') {
    const isDM = !!d.dm;
    const cls = isDM ? (d.from === myName ? 'me dm' : 'them dm')
                     : (d.from === myName ? 'me' : 'them');
    addBubble(d.from, d.text, cls, d.to, d.mentions);
    if (isDM && d.from !== myName)
      notify('DM from ' + d.from, d.text);
    else if (!isDM && d.mentions && d.mentions.indexOf(myName) >= 0)
      notify(d.from + ' mentioned you', d.text);

  } else if (d.type === 'system') {
    addSystem(d.text);
    if (d.users) updateOnline(d.users);

  } else if (d.type === 'users') {
    updateOnline(d.users);
  }
}

function setDM(name) {
  if (name === myName) return;
  dmTarget = name;
  dmLabel.textContent = '\u2192 ' + name;
  dmBar.style.display = 'flex';
  msgInput.placeholder = 'DM \u2192 ' + name + '...';
  msgInput.focus();
}
function clearDM() {
  dmTarget = '';
  dmBar.style.display = 'none';
  msgInput.placeholder = 'Message / \u0421\u043e\u043e\u0431\u0449\u0435\u043d\u0438\u0435...';
}

function updateOnline(u) {
  onlineBar.innerHTML = '';
  onlineBar.appendChild(document.createTextNode('Online: '));
  if (!u || !u.length) { onlineBar.appendChild(document.createTextNode('just you')); return; }
  u.forEach((name, i) => {
    if (i > 0) onlineBar.appendChild(document.createTextNode(', '));
    const span = document.createElement('span');
    span.className = 'online-name';
    span.textContent = name;
    span.onclick = () => setDM(name);
    onlineBar.appendChild(span);
  });
}

function notify(title, body) {
  if (!('Notification' in window) || Notification.permission !== 'granted') return;
  new Notification(title, { body, tag: 'esp32chat' });
}

function addBubble(from, text, cls, to, mentions) {
  const wrap = document.createElement('div');
  wrap.className = 'bubble-wrap ' + cls;
  const isDM = cls.includes('dm'), isMe = cls.startsWith('me');
  if (isMe && isDM && to) {
    const s = document.createElement('div');
    s.className = 'sender'; s.textContent = 'DM \u2192 ' + to; wrap.appendChild(s);
  } else if (!isMe) {
    const s = document.createElement('div');
    s.className = 'sender'; s.textContent = isDM ? from + ' \u2192 you' : from;
    wrap.appendChild(s);
  }
  const b = document.createElement('div');
  b.className = 'bubble';
  renderTextInto(b, text, mentions);
  wrap.appendChild(b); messages.appendChild(wrap);
  messages.scrollTop = messages.scrollHeight;
}

function renderTextInto(el, text, mentions) {
  if (!mentions || !mentions.length) { el.textContent = text; return; }
  const mentionSet = {};
  mentions.forEach(n => { mentionSet[n.toLowerCase()] = true; });
  text.split(/(@\S+)/).forEach(part => {
    if (part.charAt(0) === '@' && mentionSet[part.slice(1).toLowerCase()]) {
      const span = document.createElement('span');
      span.className = 'mention-tag'; span.textContent = part; el.appendChild(span);
    } else {
      el.appendChild(document.createTextNode(part));
    }
  });
}

function addSystem(text) {
  const wrap = document.createElement('div');
  wrap.className = 'bubble-wrap system';
  const b = document.createElement('div');
  b.className = 'bubble'; b.textContent = text;
  wrap.appendChild(b); messages.appendChild(wrap);
  messages.scrollTop = messages.scrollHeight;
}

function logout() {
  registered = false;
  if (ws.readyState === 1) {
    ws.send(JSON.stringify({ type: 'logout' }));
    setTimeout(() => location.replace('/'), 200);
  } else { location.replace('/'); }
}

function sendMsg() {
  const text = msgInput.value.trim();
  if (!text || !registered) return;
  const msg = { type: 'msg', text };
  if (dmTarget) msg.to = dmTarget;
  ws.send(JSON.stringify(msg));
  msgInput.value = '';
}
sendBtn.onclick    = sendMsg;
msgInput.onkeydown = e => { if (e.key === 'Enter') sendMsg(); };
</script>
</body>
</html>
)rawliteral";

// ─────────────────────────────────────────────────────────
// Captive portal
// ─────────────────────────────────────────────────────────
const char CAPTIVE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta http-equiv="refresh" content="0;url=http://192.168.4.1/">
<title>ESP32 Chat</title></head>
<body style="background:#0d0d0d;color:#e8e8e8;font-family:sans-serif;
display:flex;align-items:center;justify-content:center;height:100vh;margin:0;">
<div style="text-align:center;">
  <p style="font-size:18px;margin-bottom:8px;">ESP32 Sign In</p>
  <p style="font-size:13px;color:#555;">Opening sign-in page...</p>
  <p style="margin-top:16px;font-size:12px;color:#333;">
    If it doesn't open automatically,<br>go to
    <span style="color:#7c6af7;">http://chat.local</span>
  </p>
</div></body></html>
)rawliteral";

// ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  displayPtr = new GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT>(
    GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
  );
  display.init(115200, true, 2, false);
  u8f.begin(display);

  for (int i = 0; i < MAX_USERS; i++) users[i].active = false;

  loadAccountsNVS();
  printAccounts();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(500);

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("AP IP: " + apIP.toString());

  dnsServer.start(DNS_PORT, "*", apIP);

  // mDNS so devices can reach us at http://chat.local
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://" + String(MDNS_NAME) + ".local");
  } else {
    Serial.println("[mDNS] failed to start");
  }

  drawBootScreen(apIP.toString());

  // Captive portal URLs.
  //
  // Behaviour is per-IP: until that client has signed in we serve the
  // CAPTIVE_HTML redirect (so the OS popup opens the sign-in page).
  // After sign-in we return the OS-specific success body so the
  // "Sign in to network" badge clears.
  auto apple = [](AsyncWebServerRequest* req) {
    if (isSignedIn(req->client()->remoteIP()))
      req->send(200, "text/html",
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    else req->send_P(200, "text/html", CAPTIVE_HTML);
  };
  auto android = [](AsyncWebServerRequest* req) {
    if (isSignedIn(req->client()->remoteIP())) req->send(204);
    else req->send_P(200, "text/html", CAPTIVE_HTML);
  };
  auto ncsi = [](AsyncWebServerRequest* req) {
    if (isSignedIn(req->client()->remoteIP()))
      req->send(200, "text/plain", "Microsoft NCSI");
    else req->send_P(200, "text/html", CAPTIVE_HTML);
  };
  auto connecttest = [](AsyncWebServerRequest* req) {
    if (isSignedIn(req->client()->remoteIP()))
      req->send(200, "text/plain", "Microsoft Connect Test");
    else req->send_P(200, "text/html", CAPTIVE_HTML);
  };
  auto firefox = [](AsyncWebServerRequest* req) {
    if (isSignedIn(req->client()->remoteIP()))
      req->send(200, "text/plain", "success");
    else req->send_P(200, "text/html", CAPTIVE_HTML);
  };
  server.on("/hotspot-detect.html",       HTTP_GET, apple);
  server.on("/library/test/success.html", HTTP_GET, apple);
  server.on("/generate_204",              HTTP_GET, android);
  server.on("/gen_204",                   HTTP_GET, android);
  server.on("/connecttest.txt",           HTTP_GET, connecttest);
  server.on("/ncsi.txt",                  HTTP_GET, ncsi);
  server.on("/redirect",                  HTTP_GET, apple);
  server.on("/canonical.html",            HTTP_GET, firefox);
  server.on("/success.txt",               HTTP_GET, firefox);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (isSignedIn(req->client()->remoteIP())) {
      req->redirect("/chat");
      return;
    }
    req->send_P(200, "text/html", SIGNIN_HTML);
  });
  server.on("/chat", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", CHAT_HTML);
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("http://192.168.4.1/");
  });

  server.begin();
  Serial.println("[Chat] Ready. Accounts: " + String(accountCount));
}

// ─────────────────────────────────────────────────────────
void loop() {
  dnsServer.processNextRequest();
  ws.cleanupClients();
  if (needsRedraw && (millis() - lastDraw >= REDRAW_INTERVAL_MS)) {
    drawEPaper();
    needsRedraw = false;
    lastDraw = millis();
  }
  delay(10);
}