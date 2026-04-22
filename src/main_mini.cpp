#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

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
  if (findAccount(name)) return false;
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
// WebSocket events
// ─────────────────────────────────────────────────────────
void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {

  if (type == WS_EVT_CONNECT) {
    ws.text(c->id(), "{\"type\":\"ready\"}");

  } else if (type == WS_EVT_DISCONNECT) {
    User* u = findUser(c->id());
    if (u) {
      String leave = makeJson("system", "Server",
                              u->name + " left.", buildUserList());
      logoutUser(c->id());
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

    // ── Register new account ──────────────────────────────
    if (strcmp(mtype, "register") == 0) {
      String name = String(doc["name"] | "");
      String pass = String(doc["pass"] | "");
      name.trim(); pass.trim();

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

    // ── Sign in to existing account ───────────────────────
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
      Account* a = findAccount(name);
      String storedName = a ? a->name : name;
      markSignedIn(c->remoteIP(), storedName);
      ws.text(c->id(), "{\"type\":\"signedin\",\"name\":\"" + storedName + "\"}");

    // ── Join chat from already-signed-in IP ───────────────
    } else if (strcmp(mtype, "joinChat") == 0) {
      IPAddress ip = c->remoteIP();
      String name = lookupSignedName(ip);
      if (name.length() == 0) {
        ws.text(c->id(), "{\"type\":\"error\",\"code\":\"nosignin\",\"text\":\"Please sign in first.\"}");
        return;
      }
      bool takeover = false;
      for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && users[i].name.equalsIgnoreCase(name)) {
          uint32_t oldId = users[i].clientId;
          if (oldId == c->id()) break;
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
      ws.text(c->id(), "{\"type\":\"users\"" + buildUserList() + "}");
      if (!takeover) {
        broadcast(makeJson("system","Server", name + " joined!", buildUserList()));
        broadcastUserList();
      }

    // ── Logout ────────────────────────────────────────────
    } else if (strcmp(mtype, "logout") == 0) {
      User* u = findUser(c->id());
      if (u) {
        String name = u->name;
        logoutUser(c->id());
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
      storeHistory(u->name, text);
      broadcast(makeJson("msg", u->name, text));
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
// Chat HTML  (served at /chat)
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
#messages{flex:1;overflow-y:auto;padding:12px 14px;
  display:flex;flex-direction:column;gap:6px;scroll-behavior:smooth;}
.bubble-wrap{display:flex;flex-direction:column;max-width:82%;}
.bubble-wrap.me{align-self:flex-end;align-items:flex-end;}
.bubble-wrap.them{align-self:flex-start;align-items:flex-start;}
.bubble-wrap.system{align-self:center;align-items:center;max-width:100%;}
.sender{font-size:11px;color:var(--muted);margin-bottom:3px;padding:0 4px;}
.bubble{padding:9px 13px;border-radius:16px;font-size:14px;
  line-height:1.45;word-break:break-word;}
.me   .bubble{background:var(--accent);color:#fff;border-bottom-right-radius:4px;}
.them .bubble{background:var(--them);border:1px solid var(--border);
  border-bottom-left-radius:4px;}
.system .bubble{background:var(--system);color:var(--muted);font-size:12px;
  border-radius:8px;padding:5px 12px;}
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
  <div id="messages"></div>
  <div id="inputRow">
    <input id="msgInput" type="text" maxlength="200"
      placeholder="Message / Сообщение..." autocomplete="off">
    <button id="sendBtn">&#10148;</button>
  </div>
</div>

<script>
let ws, myName = '', registered = false, welcomedOnce = false, retryCount = 0;
let kicked = false;
const messages  = document.getElementById('messages');
const msgInput  = document.getElementById('msgInput');
const sendBtn   = document.getElementById('sendBtn');
const onlineBar = document.getElementById('onlineBar');
const whoami    = document.getElementById('whoami');

function connect() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onclose = () => {
    registered = false;
    if (kicked) return;
    setTimeout(connect, 2000);
  };
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
    registered = true;
    retryCount = 0;
    myName = d.name;
    whoami.textContent = 'Logged in as ' + d.name;
    if (!welcomedOnce) {
      addSystem('Welcome, ' + d.name + '! \u2713');
      welcomedOnce = true;
    }
    msgInput.focus();

  } else if (d.type === 'error') {
    if (d.code === 'nosignin') { location.replace('/'); return; }
    if (d.text && d.text.toLowerCase().includes('already in chat') && retryCount < 4) {
      retryCount++;
      setTimeout(sendJoin, 400);
      return;
    }
    addSystem('Error: ' + (d.text || 'unknown'));

  } else if (d.type === 'loggedout') {
    location.replace('/');

  } else if (d.type === 'taken') {
    kicked = true;
    registered = false;
    msgInput.disabled = true;
    sendBtn.disabled = true;
    addSystem('This chat was opened in another tab — this tab is now inactive.');

  } else if (d.type === 'msg') {
    addBubble(d.from, d.text, d.from === myName ? 'me' : 'them');

  } else if (d.type === 'system') {
    addSystem(d.text);
    if (d.users) updateOnline(d.users);

  } else if (d.type === 'users') {
    updateOnline(d.users);
  }
}

function updateOnline(u) {
  onlineBar.textContent = 'Online: ' + (u.length ? u.join(', ') : 'just you');
}
function addBubble(from, text, cls) {
  const wrap = document.createElement('div');
  wrap.className = 'bubble-wrap ' + cls;
  if (from && cls !== 'me') {
    const s = document.createElement('div');
    s.className = 'sender'; s.textContent = from; wrap.appendChild(s);
  }
  const b = document.createElement('div');
  b.className = 'bubble'; b.textContent = text;
  wrap.appendChild(b); messages.appendChild(wrap);
  messages.scrollTop = messages.scrollHeight;
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
  } else {
    location.replace('/');
  }
}

function sendMsg() {
  const text = msgInput.value.trim();
  if (!text || !registered) return;
  ws.send(JSON.stringify({ type: 'msg', text }));
  msgInput.value = '';
}
sendBtn.onclick    = sendMsg;
msgInput.onkeydown = e => { if (e.key === 'Enter') sendMsg(); };
</script>
</body>
</html>
)rawliteral";

// ─────────────────────────────────────────────────────────
// Captive portal HTML
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

  for (int i = 0; i < MAX_USERS; i++) users[i].active = false;

  loadAccountsNVS();
  printAccounts();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(500);

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("AP IP: " + apIP.toString());

  dnsServer.start(DNS_PORT, "*", apIP);

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://" + String(MDNS_NAME) + ".local");
  } else {
    Serial.println("[mDNS] failed to start");
  }

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
  delay(10);
}
