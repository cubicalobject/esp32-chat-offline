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

// ── Moderators ────────────────────────────────────────────
#define MAX_MODS 8
char modNames[MAX_MODS][17];
int modCount = 0;

// ── Durak game state ──────────────────────────────────────
#define MAX_GAMES 3
#define MAX_GAME_PLAYERS 8
#define MAX_DECK_SIZE 56

struct AttackSlot { uint8_t atk; uint8_t def; bool defended; };

struct DurakGame {
  bool       active;
  bool       inProgress;
  char       code[9];
  char       host[17];
  char       players[MAX_GAME_PLAYERS][17];
  int        playerCount;
  int        deckSize;
  bool       useJokers;
  uint8_t    deck[MAX_DECK_SIZE];
  int        deckTop;
  int        deckTotal;
  uint8_t    hands[MAX_GAME_PLAYERS][12];
  int        handSizes[MAX_GAME_PLAYERS];
  bool       outOfGame[MAX_GAME_PLAYERS];
  int        activePlayers;
  uint8_t    trumpCard;
  uint8_t    trumpSuit;
  int        attackerIdx;
  int        defenderIdx;
  AttackSlot table[6];
  int        tableCount;
  bool       defenderTaking;
  // Players (other than the defender) who have declared "Бито" for the
  // current "all-defended" state. Reset whenever a new card hits the table
  // (attack, perevod) or a round ends. Round only ends when every active
  // non-defender has passed.
  bool       passed[MAX_GAME_PLAYERS];
  char       loser[17];
  bool       done;
};
DurakGame games[MAX_GAMES];

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
// Moderator NVS helpers
// ─────────────────────────────────────────────────────────
void saveModsNVS() {
  prefs.begin("durak", false);
  prefs.putUChar("mod_count", (uint8_t)modCount);
  for (int i = 0; i < modCount; i++)
    prefs.putString(("mod_" + String(i)).c_str(), modNames[i]);
  prefs.end();
}
void loadModsNVS() {
  prefs.begin("durak", true);
  int cnt = (int)prefs.getUChar("mod_count", 0);
  modCount = 0;
  for (int i = 0; i < cnt && modCount < MAX_MODS; i++) {
    String n = prefs.getString(("mod_" + String(i)).c_str(), "");
    if (n.length() > 0 && n.length() <= 16)
      n.toCharArray(modNames[modCount++], 17);
  }
  prefs.end();
  if (modCount == 0) { strncpy(modNames[modCount++], "test_1", 17); saveModsNVS(); }
  Serial.println("[Mods] " + String(modCount) + " loaded.");
}
bool isModerator(const String& name) {
  for (int i = 0; i < modCount; i++)
    if (name.equalsIgnoreCase(modNames[i])) return true;
  return false;
}

// ─────────────────────────────────────────────────────────
// Card helpers
// ─────────────────────────────────────────────────────────
// Suit symbols followed by U+FE0E (text variation selector) — forces phones to
// render glyphs as text (so CSS color applies) instead of as black emoji.
static const char* SUIT_SYMS[]  = {
  "\xe2\x99\xa0\xef\xb8\x8e",  // ♠ + VS15
  "\xe2\x99\xa5\xef\xb8\x8e",  // ♥ + VS15
  "\xe2\x99\xa6\xef\xb8\x8e",  // ♦ + VS15
  "\xe2\x99\xa3\xef\xb8\x8e"   // ♣ + VS15
};
static const char* RANKS_16[]   = {"J","Q","K","A"};
static const char* RANKS_36[]   = {"6","7","8","9","10","J","Q","K","A"};
static const char* RANKS_52[]   = {"2","3","4","5","6","7","8","9","10","J","Q","K","A"};

inline uint8_t cSuit(uint8_t c) { return c >> 4; }
inline uint8_t cRank(uint8_t c) { return c & 0x0F; }

String cardStr(uint8_t c, int ds) {
  if (c == 0xFF) return "Jkr";
  const char** r = (ds==16)?RANKS_16:(ds==36)?RANKS_36:RANKS_52;
  return String(SUIT_SYMS[cSuit(c)]) + r[cRank(c)];
}
bool canDefend(uint8_t atk, uint8_t def, uint8_t trump) {
  if (atk == 0xFF) return true;
  if (def == 0xFF) return true;
  uint8_t as=cSuit(atk),ar=cRank(atk),ds2=cSuit(def),dr=cRank(def);
  if (ds2==as && dr>ar) return true;
  if (ds2==trump && as!=trump) return true;
  return false;
}
void buildAndShuffle(DurakGame& g) {
  int rc = (g.deckSize==16)?4:(g.deckSize==36)?9:13;
  g.deckTotal = 0;
  for (int s=0;s<4;s++) for (int r=0;r<rc;r++) g.deck[g.deckTotal++]=(uint8_t)((s<<4)|r);
  if (g.useJokers) { g.deck[g.deckTotal++]=0xFF; g.deck[g.deckTotal++]=0xFF; }
  for (int i=g.deckTotal-1;i>0;i--) {
    int j=random(i+1); uint8_t t=g.deck[i]; g.deck[i]=g.deck[j]; g.deck[j]=t;
  }
  // Trump = last card in the deck. Never let it be a joker or an Ace.
  uint8_t aceR = (uint8_t)(rc-1);
  auto badTrump = [&](uint8_t card) {
    if (card==0xFF) return true;                  // joker
    if ((card & 0x0F) == aceR) return true;       // Ace
    return false;
  };
  if (badTrump(g.deck[g.deckTotal-1])) {
    for (int k=g.deckTotal-2;k>=0;k--) {
      if (!badTrump(g.deck[k])) {
        uint8_t t=g.deck[g.deckTotal-1]; g.deck[g.deckTotal-1]=g.deck[k]; g.deck[k]=t;
        break;
      }
    }
  }
}
String genCode() {
  String c; for (int i=0;i<8;i++) c+=String(random(10)); return c;
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
// Durak game logic
// ─────────────────────────────────────────────────────────
int nextActive(DurakGame& g, int from) {
  int cur = (from+1) % g.playerCount;
  for (int i=0;i<g.playerCount;i++) { if (!g.outOfGame[cur]) return cur; cur=(cur+1)%g.playerCount; }
  return from;
}
DurakGame* findPlayerGame(const String& name) {
  for (int i=0;i<MAX_GAMES;i++) {
    if (!games[i].active) continue;
    for (int p=0;p<games[i].playerCount;p++)
      if (name.equalsIgnoreCase(games[i].players[p])) return &games[i];
  }
  return nullptr;
}
int playerIdx(DurakGame& g, const String& name) {
  for (int i=0;i<g.playerCount;i++) if (name.equalsIgnoreCase(g.players[i])) return i;
  return -1;
}
uint32_t clientForName(const String& name) {
  for (int i=0;i<MAX_USERS;i++)
    if (users[i].active && users[i].name.equalsIgnoreCase(name)) return users[i].clientId;
  return 0;
}

String buildLobbiesJson() {
  String j="{\"type\":\"lobbies\",\"games\":["; bool first=true;
  for (int i=0;i<MAX_GAMES;i++) {
    if (!games[i].active||games[i].inProgress) continue;
    if (!first) j+=",";
    j+="{\"code\":\""+String(games[i].code)+"\",\"host\":\""+games[i].host+
       "\",\"players\":"+String(games[i].playerCount)+
       ",\"deck\":"+String(games[i].deckSize)+
       ",\"jokers\":"+(games[i].useJokers?"true":"false")+"}";
    first=false;
  }
  return j+"]}";
}
void broadcastLobbies() { ws.textAll(buildLobbiesJson()); }

void sendLobbyUpdate(DurakGame& g) {
  String j="{\"type\":\"lobbyUpdate\",\"code\":\""+String(g.code)+
           "\",\"host\":\""+g.host+"\",\"players\":[";
  for (int p=0;p<g.playerCount;p++) { if(p)j+=","; j+="\""+String(g.players[p])+"\""; }
  j+="],\"deck\":"+String(g.deckSize)+",\"jokers\":"+(g.useJokers?"true":"false")+"}";
  for (int p=0;p<g.playerCount;p++) { uint32_t cid=clientForName(g.players[p]); if(cid)ws.text(cid,j); }
  broadcastLobbies();
}

// Forward decls for helpers defined below.
int passedCount(const DurakGame& g);
int activeNonDefenderCount(const DurakGame& g);

void sendGameState(DurakGame& g) {
  uint8_t ts=g.trumpSuit;
  String trumpSym = (ts<4) ? String(SUIT_SYMS[ts]) : String("?");
  for (int p=0;p<g.playerCount;p++) {
    if (g.outOfGame[p]) continue;
    uint32_t cid=clientForName(g.players[p]);
    if (!cid) continue;
    String j="{\"type\":\"gameState\"";
    j+=",\"hand\":[";
    for (int i=0;i<g.handSizes[p];i++) { if(i)j+=","; j+="\""+cardStr(g.hands[p][i],g.deckSize)+"\""; }
    j+="],\"opp\":[";
    bool first=true;
    for (int q=0;q<g.playerCount;q++) {
      if (q==p) continue;
      if (!first) j+=",";
      j+="{\"n\":\""+String(g.players[q])+"\",\"out\":"+(g.outOfGame[q]?"true":"false")+
         ",\"passed\":"+(g.passed[q]?"true":"false")+"}";
      first=false;
    }
    j+="],\"table\":[";
    for (int i=0;i<g.tableCount;i++) {
      if(i)j+=",";
      j+="{\"a\":\""+cardStr(g.table[i].atk,g.deckSize)+"\"";
      if (g.table[i].defended) j+=",\"d\":\""+cardStr(g.table[i].def,g.deckSize)+"\"";
      j+="}";
    }
    j+="]";
    j+=",\"trump\":\""+trumpSym+"\"";
    j+=",\"trumpCard\":\""+cardStr(g.trumpCard,g.deckSize)+"\"";
    j+=",\"atk\":\""+String(g.players[g.attackerIdx])+"\"";
    j+=",\"def\":\""+String(g.players[g.defenderIdx])+"\"";
    j+=",\"deck\":"+String(g.deckTotal-g.deckTop);
    j+=",\"taking\":"+(g.defenderTaking?String("true"):String("false"));
    j+=",\"me\":\""+String(g.players[p])+"\"";
    j+=",\"iPassed\":"+(g.passed[p]?String("true"):String("false"));
    j+=",\"passN\":"+String(passedCount(g));
    j+=",\"passT\":"+String(activeNonDefenderCount(g));
    j+="}";
    ws.text(cid,j);
  }
}

void drawCardsDurak(DurakGame& g) {
  for (int i=0;i<g.playerCount;i++) {
    int idx=(g.attackerIdx+i)%g.playerCount;
    if (g.outOfGame[idx]) continue;
    while (g.handSizes[idx]<6 && g.deckTop<g.deckTotal)
      g.hands[idx][g.handSizes[idx]++]=g.deck[g.deckTop++];
  }
}
void checkOutOfGame(DurakGame& g) {
  if (g.deckTop<g.deckTotal) return;
  for (int i=0;i<g.playerCount;i++) {
    if (!g.outOfGame[i] && g.handSizes[i]==0) {
      g.outOfGame[i]=true; g.activePlayers--;
      String msg="{\"type\":\"system\",\"text\":\""+String(g.players[i])+" вышел из игры!\"}";
      for (int p=0;p<g.playerCount;p++) { uint32_t cid=clientForName(g.players[p]); if(cid)ws.text(cid,msg); }
    }
  }
}
void endGame(DurakGame& g) {
  if (g.loser[0]==0) {
    for (int i=0;i<g.playerCount;i++)
      if (!g.outOfGame[i] && g.handSizes[i]>0) { strncpy(g.loser,g.players[i],17); break; }
  }
  g.done=true;
  String j="{\"type\":\"gameOver\",\"loser\":\""+String(g.loser)+"\"}";
  for (int p=0;p<g.playerCount;p++) { uint32_t cid=clientForName(g.players[p]); if(cid)ws.text(cid,j); }
  String loserText = String(g.loser) + " - дурак! \xf0\x9f\x83\x8f";
  broadcast(makeJson("system", "Server", loserText));
  storeHistory("Server", loserText);   // persist in chat history
  pushLine("Server", loserText);       // show on e-paper
  g.active=false;
  broadcastLobbies();
  Serial.println("[Durak] Over. Loser: "+String(g.loser));
}
void clearPasses(DurakGame& g) {
  for (int i=0;i<MAX_GAME_PLAYERS;i++) g.passed[i]=false;
}
// Returns true if every active player except the defender has declared "Бито".
bool allOthersPassed(const DurakGame& g) {
  for (int i=0;i<g.playerCount;i++) {
    if (i==g.defenderIdx) continue;
    if (g.outOfGame[i]) continue;
    if (!g.passed[i]) return false;
  }
  return true;
}
int activeNonDefenderCount(const DurakGame& g) {
  int n=0;
  for (int i=0;i<g.playerCount;i++) {
    if (i==g.defenderIdx) continue;
    if (g.outOfGame[i]) continue;
    n++;
  }
  return n;
}
int passedCount(const DurakGame& g) {
  int n=0;
  for (int i=0;i<g.playerCount;i++) {
    if (i==g.defenderIdx) continue;
    if (g.outOfGame[i]) continue;
    if (g.passed[i]) n++;
  }
  return n;
}

void advanceTurn(DurakGame& g, bool tookCards) {
  g.tableCount=0; g.defenderTaking=false;
  clearPasses(g);
  drawCardsDurak(g); checkOutOfGame(g);
  if (g.activePlayers<=1) { endGame(g); return; }
  if (tookCards) {
    // defender took cards: they are skipped this round
    g.attackerIdx = nextActive(g, g.defenderIdx);
  } else {
    // Бито: defender successfully defended → they become the new attacker
    g.attackerIdx = (!g.outOfGame[g.defenderIdx]) ? g.defenderIdx
                                                  : nextActive(g, g.defenderIdx);
  }
  g.defenderIdx = nextActive(g, g.attackerIdx);
  sendGameState(g);
}
void startDurakGame(DurakGame& g) {
  buildAndShuffle(g);
  g.deckTop=0;
  for (int i=0;i<g.playerCount;i++) { g.handSizes[i]=0; g.outOfGame[i]=false; }
  g.activePlayers=g.playerCount; g.tableCount=0; g.defenderTaking=false;
  g.done=false; g.loser[0]=0;
  clearPasses(g);
  for (int i=0;i<6;i++)
    for (int p=0;p<g.playerCount;p++)
      if (g.deckTop<g.deckTotal) g.hands[p][g.handSizes[p]++]=g.deck[g.deckTop++];
  g.trumpCard = (g.deckTop<g.deckTotal) ? g.deck[g.deckTotal-1] : 0xFF;
  g.trumpSuit = (g.trumpCard==0xFF) ? 4 : cSuit(g.trumpCard);
  int best=0; uint8_t bestR=0xFF;
  for (int p=0;p<g.playerCount;p++)
    for (int i=0;i<g.handSizes[p];i++) {
      uint8_t c=g.hands[p][i];
      if (c!=0xFF && cSuit(c)==g.trumpSuit && cRank(c)<bestR) { bestR=cRank(c); best=p; }
    }
  g.attackerIdx=best; g.defenderIdx=nextActive(g,best);
  g.inProgress=true;
  String sm="{\"type\":\"gameStarted\",\"code\":\""+String(g.code)+"\"}";
  for (int p=0;p<g.playerCount;p++) { uint32_t cid=clientForName(g.players[p]); if(cid)ws.text(cid,sm); }
  sendGameState(g);
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

    // ── Durak: host game ──────────────────────────────────
    } else if (strcmp(mtype,"hostGame")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Сначала войдите.\"}"); return; }
      if (!isModerator(name)) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Только для модераторов.\"}"); return; }
      if (findPlayerGame(name)) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Вы уже в игре.\"}"); return; }
      int slot=-1;
      for (int i=0;i<MAX_GAMES;i++) if (!games[i].active) { slot=i; break; }
      if (slot<0) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Максимум 3 игры одновременно.\"}"); return; }
      int ds=doc["deck"]|36;
      if (ds!=16&&ds!=36&&ds!=52) ds=36;
      bool jokers=doc["jokers"]|false;
      DurakGame& g=games[slot];
      memset(&g,0,sizeof(DurakGame));
      g.active=true; g.inProgress=false;
      String code=genCode(); code.toCharArray(g.code,9);
      name.toCharArray(g.host,17);
      name.toCharArray(g.players[0],17);
      g.playerCount=1; g.deckSize=ds; g.useJokers=jokers;
      sendLobbyUpdate(g);
      ws.text(c->id(),"{\"type\":\"hostedGame\",\"code\":\""+String(g.code)+"\"}");
      Serial.println("[Durak] Hosted by "+name+" code="+String(g.code));

    // ── Durak: join game ──────────────────────────────────
    } else if (strcmp(mtype,"joinGame")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Сначала войдите.\"}"); return; }
      if (findPlayerGame(name)) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Вы уже в игре.\"}"); return; }
      String code=String(doc["code"]|"");
      DurakGame* gp=nullptr;
      for (int i=0;i<MAX_GAMES;i++)
        if (games[i].active&&!games[i].inProgress&&String(games[i].code)==code) { gp=&games[i]; break; }
      if (!gp) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Игра не найдена.\"}"); return; }
      if (gp->playerCount>=MAX_GAME_PLAYERS) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Игра заполнена.\"}"); return; }
      name.toCharArray(gp->players[gp->playerCount++],17);
      sendLobbyUpdate(*gp);

    // ── Durak: leave game ─────────────────────────────────
    } else if (strcmp(mtype,"leaveGame")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) return;
      DurakGame* gp=findPlayerGame(name);
      if (!gp) return;
      if (gp->inProgress) { strncpy(gp->loser,name.c_str(),17); endGame(*gp); return; }
      int idx=playerIdx(*gp,name);
      if (idx>=0) { for(int i=idx;i<gp->playerCount-1;i++) memcpy(gp->players[i],gp->players[i+1],17); gp->playerCount--; }
      if (gp->playerCount==0||name.equalsIgnoreCase(gp->host)) { gp->active=false; broadcastLobbies(); }
      else sendLobbyUpdate(*gp);

    // ── Durak: start game ─────────────────────────────────
    } else if (strcmp(mtype,"startGame")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) return;
      DurakGame* gp=findPlayerGame(name);
      if (!gp||!name.equalsIgnoreCase(gp->host)) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Вы не хост.\"}"); return; }
      if (gp->playerCount<2) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Нужно 2 или более игроков.\"}"); return; }
      startDurakGame(*gp);

    // ── Durak: mod status check ───────────────────────────
    } else if (strcmp(mtype,"checkMod")==0) {
      String name=lookupSignedName(c->remoteIP());
      bool mod=(name.length()>0)&&isModerator(name);
      ws.text(c->id(),"{\"type\":\"modStatus\",\"isMod\":"+(mod?String("true"):String("false"))+"}");

    // ── Durak: get lobbies ────────────────────────────────
    } else if (strcmp(mtype,"getLobbies")==0) {
      ws.text(c->id(),buildLobbiesJson());

    // ── Durak: resync — am I already in a game? ───────────
    } else if (strcmp(mtype,"getMyGame")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) return;
      DurakGame* gp=findPlayerGame(name);
      if (!gp) { ws.text(c->id(),"{\"type\":\"noGame\"}"); return; }
      if (gp->inProgress) {
        ws.text(c->id(),"{\"type\":\"gameStarted\"}");
        sendGameState(*gp);
      } else {
        sendLobbyUpdate(*gp);
      }

    // ── Durak: attack (podkidnoy — any non-defender can throw in) ─────
    } else if (strcmp(mtype,"attack")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) return;
      DurakGame* gp=findPlayerGame(name);
      if (!gp||!gp->inProgress||gp->done) return;
      int pi=playerIdx(*gp,name);
      if (pi==gp->defenderIdx) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Защитник не может атаковать.\"}"); return; }
      if (gp->tableCount==0 && pi!=gp->attackerIdx) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Только основной атакующий начинает раунд.\"}"); return; }
      if (gp->tableCount>=6) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Максимум 6 карт.\"}"); return; }
      if (gp->tableCount>=gp->handSizes[gp->defenderIdx]) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"У защитника недостаточно карт.\"}"); return; }
      String cardS=String(doc["card"]|"");
      int ci=-1;
      for (int i=0;i<gp->handSizes[pi];i++) if (cardStr(gp->hands[pi][i],gp->deckSize)==cardS) { ci=i; break; }
      if (ci<0) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Карта не найдена.\"}"); return; }
      if (gp->tableCount>0) {
        uint8_t myR=(gp->hands[pi][ci]==0xFF)?0xFF:cRank(gp->hands[pi][ci]);
        bool ok=false;
        for (int i=0;i<gp->tableCount;i++) {
          if (((gp->table[i].atk==0xFF)?0xFF:cRank(gp->table[i].atk))==myR) { ok=true; break; }
          if (gp->table[i].defended && ((gp->table[i].def==0xFF)?0xFF:cRank(gp->table[i].def))==myR) { ok=true; break; }
        }
        if (!ok) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Достоинство должно совпадать с картами на столе.\"}"); return; }
      }
      uint8_t card=gp->hands[pi][ci];
      for (int i=ci;i<gp->handSizes[pi]-1;i++) gp->hands[pi][i]=gp->hands[pi][i+1]; gp->handSizes[pi]--;
      gp->table[gp->tableCount++]={card,0,false};
      clearPasses(*gp);  // new card on table — everyone gets to vote again
      sendGameState(*gp);

    // ── Durak: defend ─────────────────────────────────────
    } else if (strcmp(mtype,"defend")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) return;
      DurakGame* gp=findPlayerGame(name);
      if (!gp||!gp->inProgress||gp->done) return;
      int pi=playerIdx(*gp,name);
      if (pi!=gp->defenderIdx) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Не ваш ход.\"}"); return; }
      int slot=doc["slot"]|-1;
      if (slot<0||slot>=gp->tableCount||gp->table[slot].defended) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Неверный слот.\"}"); return; }
      String cardS=String(doc["card"]|"");
      int ci=-1;
      for (int i=0;i<gp->handSizes[pi];i++) if (cardStr(gp->hands[pi][i],gp->deckSize)==cardS) { ci=i; break; }
      if (ci<0) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Карта не найдена.\"}"); return; }
      if (!canDefend(gp->table[slot].atk,gp->hands[pi][ci],gp->trumpSuit)) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Эту карту не побить.\"}"); return; }
      gp->table[slot].def=gp->hands[pi][ci]; gp->table[slot].defended=true;
      for (int i=ci;i<gp->handSizes[pi]-1;i++) gp->hands[pi][i]=gp->hands[pi][i+1]; gp->handSizes[pi]--;
      sendGameState(*gp);

    // ── Durak: pass (any non-defender votes "Бито") ───────
    } else if (strcmp(mtype,"pass")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) return;
      DurakGame* gp=findPlayerGame(name);
      if (!gp||!gp->inProgress||gp->done) return;
      int pi=playerIdx(*gp,name);
      if (pi==gp->defenderIdx) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Защитник не голосует.\"}"); return; }
      if (gp->outOfGame[pi]) return;
      if (gp->tableCount==0) return;
      bool allDef=true;
      for (int i=0;i<gp->tableCount;i++) if (!gp->table[i].defended) { allDef=false; break; }
      if (!allDef) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Не все карты побиты.\"}"); return; }
      gp->passed[pi]=true;
      if (allOthersPassed(*gp)) advanceTurn(*gp,false);
      else sendGameState(*gp);

    // ── Durak: perevod ────────────────────────────────────
    } else if (strcmp(mtype,"perevod")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) return;
      DurakGame* gp=findPlayerGame(name);
      if (!gp||!gp->inProgress||gp->done) return;
      int pi=playerIdx(*gp,name);
      if (pi!=gp->defenderIdx) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Не ваш ход.\"}"); return; }
      for (int i=0;i<gp->tableCount;i++) if (gp->table[i].defended) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Перевод невозможен после защиты.\"}"); return; }
      String cardS=String(doc["card"]|"");
      int ci=-1;
      for (int i=0;i<gp->handSizes[pi];i++) if (cardStr(gp->hands[pi][i],gp->deckSize)==cardS) { ci=i; break; }
      if (ci<0) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Карта не найдена.\"}"); return; }
      uint8_t myCard=gp->hands[pi][ci];
      uint8_t myR=(myCard==0xFF)?0xFF:cRank(myCard);
      bool rankOk=false;
      for (int i=0;i<gp->tableCount;i++) if (((gp->table[i].atk==0xFF)?0xFF:cRank(gp->table[i].atk))==myR) { rankOk=true; break; }
      if (!rankOk) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Достоинство должно совпадать с атакой.\"}"); return; }
      // In 2-player perevod, nextDef is the original attacker (roles just swap).
      int nextDef=nextActive(*gp,pi);
      if (nextDef==pi) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"Нет другого игрока.\"}"); return; }
      if (gp->tableCount+1>gp->handSizes[nextDef]) { ws.text(c->id(),"{\"type\":\"error\",\"text\":\"У следующего игрока недостаточно карт.\"}"); return; }
      uint8_t card=gp->hands[pi][ci];
      for (int i=ci;i<gp->handSizes[pi]-1;i++) gp->hands[pi][i]=gp->hands[pi][i+1]; gp->handSizes[pi]--;
      gp->table[gp->tableCount++]={card,0,false};
      gp->attackerIdx=pi; gp->defenderIdx=nextDef;
      clearPasses(*gp);
      sendGameState(*gp);

    // ── Durak: take ───────────────────────────────────────
    } else if (strcmp(mtype,"take")==0) {
      String name=lookupSignedName(c->remoteIP());
      if (name.length()==0) return;
      DurakGame* gp=findPlayerGame(name);
      if (!gp||!gp->inProgress||gp->done) return;
      int pi=playerIdx(*gp,name);
      if (pi!=gp->defenderIdx) return;
      for (int i=0;i<gp->tableCount;i++) {
        if (gp->handSizes[pi]<12) gp->hands[pi][gp->handSizes[pi]++]=gp->table[i].atk;
        if (gp->table[i].defended&&gp->handSizes[pi]<12) gp->hands[pi][gp->handSizes[pi]++]=gp->table[i].def;
      }
      advanceTurn(*gp,true);
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
    Open your phone's browser and tap
    <a href="http://chat.local/chat" class="url">http://chat.local</a>
    <span style="display:block;font-size:11px;color:#444;margin-top:6px;">
      (or <a href="http://192.168.4.1/chat" style="color:#7c6af7;font-family:monospace;text-decoration:underline;">http://192.168.4.1</a>)
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
    <div style="display:flex;gap:8px;">
      <a href="/durak" style="padding:6px 12px;border:1px solid var(--border);border-radius:8px;
        background:transparent;color:var(--accent);font-size:12px;text-decoration:none;">&#9824;&#xFE0E; Дурак</a>
      <button id="logoutBtn" onclick="logout()">Log out</button>
    </div>
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
// Durak page HTML  (served at /durak)
// ─────────────────────────────────────────────────────────
const char DURAK_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Durak</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
:root{--bg:#0d0d0d;--surface:#141414;--border:#222;--text:#e8e8e8;--muted:#777;
  --accent:#7c6af7;--red:#ef4444;--green:#22c55e;
  --r-atk:#f59e0b;       /* main attacker — amber */
  --r-def:#3b82f6;       /* defender — blue */
  --r-thrower:#22c55e;   /* throw-in eligible — green */
}
body{background:var(--bg);color:var(--text);
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  font-size:16px;
  height:100dvh;display:flex;flex-direction:column;overflow:hidden;}
header{padding:12px 16px;background:var(--surface);border-bottom:1px solid var(--border);
  display:flex;align-items:center;justify-content:space-between;}
header .title{font-size:20px;font-weight:600;}
header a{color:var(--muted);font-size:15px;text-decoration:none;}
header a:hover{color:var(--text);}
.section-title{font-size:14px;color:var(--muted);text-transform:uppercase;
  letter-spacing:.06em;margin-bottom:10px;}
.game-card{background:var(--surface);border:1px solid var(--border);border-radius:10px;
  padding:14px 16px;margin-bottom:10px;display:flex;align-items:center;gap:10px;}
.game-card .info{flex:1;}
.game-card .code{font-family:monospace;font-size:19px;font-weight:600;color:var(--accent);}
.game-card .meta{font-size:14px;color:var(--muted);margin-top:3px;}
.btn{padding:11px 18px;border:none;border-radius:8px;font-size:16px;font-weight:500;
  cursor:pointer;background:var(--accent);color:#fff;}
.btn:active{opacity:.8;} .btn:disabled{opacity:.4;cursor:default;}
.btn.secondary{background:var(--surface);border:1px solid var(--border);color:var(--text);}
.btn.danger{background:var(--red);}
.btn.sm{padding:8px 14px;font-size:14px;}
input[type=text],select{width:100%;padding:13px 14px;background:var(--surface);
  border:1px solid var(--border);border-radius:8px;color:var(--text);font-size:16px;
  outline:none;margin-bottom:10px;}
input[type=text]:focus,select:focus{border-color:var(--accent);}
.row{display:flex;gap:8px;align-items:center;}
.row label{font-size:15px;color:var(--muted);}
.err{font-size:15px;color:var(--red);margin-bottom:8px;min-height:18px;}
#roomCode{font-family:monospace;font-size:36px;font-weight:700;color:var(--accent);
  letter-spacing:4px;text-align:center;padding:18px 0;}
.player-pill{display:inline-block;padding:6px 12px;background:var(--surface);
  border:1px solid var(--border);border-radius:20px;font-size:15px;margin:4px;}
#trumpBar{padding:8px 16px;background:#12091f;border-bottom:1px solid #3a1f6e;
  font-size:15px;color:#c084fc;display:flex;gap:14px;flex-wrap:wrap;align-items:center;}
.role-pill{padding:3px 10px;border-radius:12px;font-size:13px;font-weight:600;
  border:1px solid currentColor;}
.role-pill.atk{color:var(--r-atk);}
.role-pill.def{color:var(--r-def);}
.role-pill.thrower{color:var(--r-thrower);}
.role-pill.spectator{color:var(--muted);}
#oppRow{padding:8px 14px;border-bottom:1px solid var(--border);
  display:flex;gap:10px;overflow-x:auto;}
.opp-box{text-align:center;min-width:64px;padding:6px 8px;border-radius:10px;
  border:2px solid var(--border);background:#0f0f0f;}
.opp-box .opp-name{font-size:13px;font-weight:600;white-space:nowrap;overflow:hidden;
  text-overflow:ellipsis;max-width:80px;}
.opp-box .opp-role{font-size:10px;text-transform:uppercase;letter-spacing:.05em;
  margin-top:2px;}
.opp-box .opp-back{font-size:26px;line-height:1;margin-top:3px;color:#9ca3af;}
.opp-box.r-atk{border-color:var(--r-atk);}
.opp-box.r-atk .opp-name,.opp-box.r-atk .opp-role{color:var(--r-atk);}
.opp-box.r-def{border-color:var(--r-def);}
.opp-box.r-def .opp-name,.opp-box.r-def .opp-role{color:var(--r-def);}
.opp-box.r-thrower{border-color:var(--r-thrower);}
.opp-box.r-thrower .opp-name,.opp-box.r-thrower .opp-role{color:var(--r-thrower);}
.opp-box.passed{background:#0d2010;}
.opp-box.passed::after{content:'\2713 Бито';display:block;font-size:11px;
  color:var(--r-thrower);margin-top:2px;}
.opp-box.out{opacity:.35;border-color:var(--border);}
#tableArea{flex:1;overflow-y:auto;padding:12px 16px;display:flex;flex-wrap:wrap;gap:10px;
  align-content:flex-start;}
.slot{background:var(--surface);border:1px solid var(--border);border-radius:8px;
  padding:8px 10px;min-width:60px;text-align:center;cursor:pointer;}
.slot.selected{border-color:var(--accent);box-shadow:0 0 0 2px rgba(124,106,247,0.3);}
.slot .atk{font-size:18px;font-weight:600;}
.slot .def{font-size:15px;color:var(--green);margin-top:3px;}
#statusBar{padding:10px 16px;font-size:15px;color:var(--text);font-weight:500;
  border-top:1px solid var(--border);background:#101010;}
#handArea{padding:10px 14px 4px;display:flex;gap:8px;overflow-x:auto;
  border-top:1px solid var(--border);}
.card-btn{padding:12px 14px;background:var(--surface);border:1px solid var(--border);
  border-radius:8px;font-size:20px;font-weight:600;cursor:pointer;white-space:nowrap;flex-shrink:0;}
.card-btn.selected{border-color:var(--accent);background:#1a1040;
  box-shadow:0 0 0 2px rgba(124,106,247,0.3);}
.card-btn{color:#e8e8e8;}
.card-btn.red{color:#f87171;}
.atk{color:#e8e8e8;}
.atk.red{color:#f87171;}
.def{color:#22c55e;}
#actionBar{padding:10px 14px 12px;display:flex;gap:8px;flex-wrap:wrap;
  border-top:1px solid var(--border);}
</style>
</head>
<body>
<header>
  <span class="title">&#9824;&#xFE0E; Дурак</span>
  <a href="/chat">&#8592; Чат</a>
</header>

<div id="viewLobby" style="flex:1;overflow-y:auto;padding:14px;">
  <div class="section-title">Открытые игры</div>
  <div id="gamesList"><p style="color:var(--muted);font-size:13px;">Нет открытых игр.</p></div>
  <div id="modPanel" style="display:none;margin-top:14px;">
    <div class="section-title">Создать новую игру</div>
    <div class="err" id="hostErr"></div>
    <label style="font-size:13px;color:var(--muted);display:block;margin-bottom:4px;">Размер колоды</label>
    <select id="deckSel"><option value="36">36 карт (6&ndash;Т)</option><option value="52">52 карты (2&ndash;Т)</option><option value="16">16 карт (В&ndash;Т)</option></select>
    <div class="row" style="margin-bottom:10px;">
      <input type="checkbox" id="jokersCb" style="width:auto;margin:0;">
      <label for="jokersCb">Добавить 2 джокера (бьют любую защиту, любая карта бьёт их)</label>
    </div>
    <button class="btn" onclick="hostGame()">Создать игру</button>
  </div>
  <div style="margin-top:14px;">
    <div class="section-title">Войти по коду</div>
    <div class="err" id="joinErr"></div>
    <div class="row">
      <input type="text" id="codeInput" placeholder="8-значный код..." maxlength="8"
        style="margin:0;flex:1;" oninput="this.value=this.value.replace(/\D/g,'')">
      <button class="btn sm" onclick="joinGame()">Войти</button>
    </div>
  </div>
</div>

<div id="viewRoom" style="display:none;flex:1;overflow-y:auto;padding:14px;">
  <div class="section-title">Код игры &mdash; поделитесь с друзьями</div>
  <div id="roomCode"></div>
  <div style="text-align:center;font-size:12px;color:var(--muted);margin-top:-8px;margin-bottom:14px;">
    Войти можно на странице <b>chat.local/durak</b>
  </div>
  <div class="section-title" style="display:flex;justify-content:space-between;align-items:baseline;">
    <span>Игроки</span>
    <span id="roomCount" style="font-weight:600;color:var(--text);text-transform:none;letter-spacing:0;"></span>
  </div>
  <div id="roomPlayers" style="margin-bottom:8px;"></div>
  <div style="font-size:12px;color:var(--muted);" id="roomMeta"></div>
  <div id="roomWaiting" style="margin-top:14px;padding:10px;background:var(--surface);border:1px dashed var(--border);border-radius:8px;text-align:center;font-size:13px;color:var(--muted);"></div>
  <div class="err" id="roomErr" style="margin-top:8px;"></div>
  <div style="margin-top:14px;display:flex;gap:8px;flex-wrap:wrap;">
    <button class="btn" id="startBtn" onclick="startGame()" style="display:none;">Начать игру</button>
    <button class="btn danger sm" onclick="leaveGame()">Выйти</button>
  </div>
</div>

<div id="viewGame" style="display:none;flex:1;flex-direction:column;">
  <div id="trumpBar">
    <span id="trumpLabel">Козырь: &mdash;</span>
    <span id="deckLabel">Колода: &mdash;</span>
    <span id="myRolePill" class="role-pill spectator">Вы: &mdash;</span>
  </div>
  <div id="oppRow"></div>
  <div id="tableArea"></div>
  <div id="gameErr" style="display:none;padding:6px 14px;background:#3b0f0f;color:#fca5a5;font-size:12px;border-top:1px solid #5a1a1a;"></div>
  <div id="statusBar">Ожидание...</div>
  <div id="handArea"></div>
  <div id="actionBar"></div>
</div>

<script>
let ws, myName='', isMod=false;
let gameState=null, selectedCard=null, selectedSlot=-1;
let inRoom=false, myRoomCode='';

function show(id) {
  ['viewLobby','viewRoom','viewGame'].forEach(v=>{
    const el=document.getElementById(v);
    el.style.display=(v===id)?'flex':'none';
    if (v===id) el.style.flexDirection='column';
  });
}

function connect() {
  ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onclose=()=>setTimeout(connect,2000);
  ws.onerror=()=>ws.close();
  ws.onmessage=e=>handle(JSON.parse(e.data));
}
connect();

function send(obj){ if(ws.readyState===1) ws.send(JSON.stringify(obj)); }

function handle(d) {
  if (d.type==='ready') {
    send({type:'joinChat'});
    send({type:'getLobbies'});
  } else if (d.type==='loggedin') {
    myName=d.name;
    send({type:'checkMod'});
    send({type:'getMyGame'});
  } else if (d.type==='noGame') {
    // not in a game — stay on lobby view
  } else if (d.type==='modStatus') {
    isMod=!!d.isMod;
    document.getElementById('modPanel').style.display=isMod?'block':'none';
  } else if (d.type==='error') {
    if (d.code==='nosignin') { location.replace('/'); return; }
    const txt=d.text||'Error';
    document.getElementById('hostErr').textContent=txt;
    document.getElementById('joinErr').textContent=txt;
    document.getElementById('roomErr').textContent=txt;
    const ge=document.getElementById('gameErr');
    if (ge) {
      ge.textContent=txt;
      ge.style.display='block';
      clearTimeout(window._gameErrT);
      window._gameErrT=setTimeout(()=>{ ge.style.display='none'; }, 3500);
    }
  } else if (d.type==='lobbies') {
    renderLobbies(d.games);
  } else if (d.type==='hostedGame') {
    myRoomCode=d.code; inRoom=true;
    show('viewRoom');
  } else if (d.type==='lobbyUpdate') {
    renderRoom(d);
    if (!inRoom || myRoomCode===d.code) { inRoom=true; myRoomCode=d.code; show('viewRoom'); }
  } else if (d.type==='gameStarted') {
    show('viewGame');
  } else if (d.type==='gameState') {
    gameState=d; renderGame();
  } else if (d.type==='gameOver') {
    show('viewLobby');
    inRoom=false; myRoomCode='';
    send({type:'getLobbies'});
    alert((d.loser===myName?'Вы - дурак':d.loser+' - дурак')+'! 🃏');
  }
}

function renderLobbies(games) {
  const el=document.getElementById('gamesList');
  if (!games||!games.length) { el.innerHTML='<p style="color:var(--muted);font-size:13px;">Нет открытых игр.</p>'; return; }
  el.innerHTML='';
  games.forEach(g=>{
    const div=document.createElement('div');
    div.className='game-card';
    div.innerHTML=`<div class="info"><div class="code">${g.code}</div>
      <div class="meta">Хост: ${g.host} &nbsp;|&nbsp; ${g.players}/8 игроков &nbsp;|&nbsp; ${g.deck} карт${g.jokers?' + джокеры':''}</div>
      </div><button class="btn sm" onclick="joinByCode('${g.code}')">Войти</button>`;
    el.appendChild(div);
  });
}

function renderRoom(d) {
  document.getElementById('roomCode').textContent=d.code;
  const players=d.players||[];
  document.getElementById('roomCount').textContent=`${players.length}/8`;
  const pp=document.getElementById('roomPlayers');
  pp.innerHTML='';
  players.forEach(n=>{
    const span=document.createElement('span');
    span.className='player-pill';
    let label=n;
    if (n===d.host) label+=' (хост)';
    if (n===myName) label+=' &mdash; вы';
    span.innerHTML=label;
    pp.appendChild(span);
  });
  document.getElementById('roomMeta').textContent=`Колода: ${d.deck} карт${d.jokers?' + джокеры':''}`;
  const isHost=(myName===d.host);
  const sb=document.getElementById('startBtn');
  sb.style.display=isHost?'inline-block':'none';
  sb.disabled=players.length<2;
  sb.textContent=players.length<2?'Нужно ещё игроков':`Начать игру (${players.length})`;
  const w=document.getElementById('roomWaiting');
  if (isHost) {
    w.textContent=players.length<2
      ? 'Ожидание игроков... поделитесь кодом выше.'
      : 'Можно начинать! Или дождитесь ещё игроков.';
  } else {
    w.textContent='Ожидайте, пока хост начнёт игру.';
  }
}

function suitColor(card) {
  return (card.startsWith('♥')||card.startsWith('♦'))?'red':'';
}

function renderGame() {
  const d=gameState;
  if (!d) return;
  document.getElementById('trumpLabel').textContent=`Козырь: ${d.trump} (${d.trumpCard})`;
  document.getElementById('deckLabel').textContent=`Колода: ${d.deck}`;

  // Show "you are: <role>" pill
  const isAtk=(d.me===d.atk), isDef=(d.me===d.def);
  const myRolePill=document.getElementById('myRolePill');
  if (myRolePill) {
    let cls='spectator', txt='наблюдатель';
    if (isAtk) { cls='atk'; txt='атакующий'; }
    else if (isDef) { cls='def'; txt='защитник'; }
    else if (d.table && d.table.length>0) { cls='thrower'; txt='подкидной'; }
    myRolePill.className='role-pill '+cls;
    myRolePill.textContent='Вы: '+txt;
  }

  const opp=document.getElementById('oppRow');
  opp.innerHTML='';
  (d.opp||[]).forEach(o=>{
    const div=document.createElement('div');
    let role='', label='';
    if (o.n===d.atk) { role='r-atk'; label='атакующий'; }
    else if (o.n===d.def) { role='r-def'; label='защитник'; }
    else if (d.table && d.table.length>0) { role='r-thrower'; label='подкидной'; }
    const passedCls=(o.passed?' passed':'');
    div.className='opp-box '+role+(o.out?' out':'')+passedCls;
    const badge=o.out
      ? '<div style="font-size:11px;color:var(--muted);">выбыл</div>'
      : '<div class="opp-back">&#x1F0A0;</div>';
    div.innerHTML=`<div class="opp-name">${o.n}</div>`
      + (label?`<div class="opp-role">${label}</div>`:'')
      + badge;
    opp.appendChild(div);
  });

  const ta=document.getElementById('tableArea');
  ta.innerHTML='';
  (d.table||[]).forEach((slot,i)=>{
    const div=document.createElement('div');
    div.className='slot'+(selectedSlot===i?' selected':'');
    div.innerHTML=`<div class="atk ${suitColor(slot.a)}">${slot.a}</div>`;
    if (slot.d) div.innerHTML+=`<div class="def ${suitColor(slot.d)}">${slot.d}</div>`;
    div.onclick=()=>selectSlot(i);
    ta.appendChild(div);
  });

  const canThrowIn = !isDef && !isAtk && d.table.length > 0;  // podkidnoy
  const allDef = d.table.length>0 && d.table.every(s=>s.d);
  const voteSuffix = (allDef && (d.passT||0)>0) ? ` (Бито: ${d.passN||0}/${d.passT||0})` : '';
  let status='';
  if (isAtk && allDef && d.iPassed) status='Вы готовы &mdash; ждём остальных';
  else if (isAtk && allDef) status='Все карты побиты — нажмите «Бито» или подкиньте ещё';
  else if (isAtk) status='Ваш ход — атакуйте';
  else if (isDef && allDef) status='Защита успешна — ждём, пока все скажут «Бито»';
  else if (isDef) status='Ваш ход — защищайтесь';
  else if (canThrowIn && allDef && d.iPassed) status='Вы готовы &mdash; ждём остальных';
  else if (canThrowIn && allDef) status='Все карты побиты — подкиньте или нажмите «Бито»';
  else if (canThrowIn) status=`${d.atk} атакует — можете подкинуть`;
  else status=`${d.atk} атакует ${d.def}`;
  document.getElementById('statusBar').innerHTML=status+voteSuffix;

  const ha=document.getElementById('handArea');
  ha.innerHTML='';
  (d.hand||[]).forEach(card=>{
    const btn=document.createElement('button');
    btn.className='card-btn'+(suitColor(card)?' red':'')+(selectedCard===card?' selected':'');
    btn.textContent=card;
    btn.onclick=()=>selectCard(card);
    ha.appendChild(btn);
  });

  renderActions(isAtk, isDef, d);
}

function selectCard(card) {
  selectedCard=(selectedCard===card)?null:card;
  renderGame();
}
function selectSlot(i) {
  selectedSlot=(selectedSlot===i)?-1:i;
  renderGame();
}

function renderActions(isAtk, isDef, d) {
  const ab=document.getElementById('actionBar');
  ab.innerHTML='';
  const canThrowIn = !isDef && !isAtk && d.table.length > 0;  // podkidnoy
  const allDef = d.table.length>0 && d.table.every(s=>s.d);
  if (isAtk || canThrowIn) {
    if (selectedCard) {
      const label = isAtk ? 'Атаковать' : 'Подкинуть';
      ab.innerHTML+=`<button class="btn" onclick="doAttack()">${label} (${selectedCard})</button>`;
    }
    // Any non-defender votes "Бито"; the round ends only when everyone agrees.
    if (allDef && !d.iPassed) {
      const cls = selectedCard ? 'btn secondary' : 'btn';
      ab.innerHTML+=`<button class="${cls}" onclick="doPass()">Бито (${(d.passN||0)+1}/${d.passT||0})</button>`;
    }
  }
  if (isDef) {
    if (selectedSlot>=0 && selectedCard) {
      ab.innerHTML+=`<button class="btn" onclick="doDefend()">Защитить слот ${selectedSlot+1}</button>`;
    }
    // perevod requires a card selected, no slot defended yet, and at least
    // one active opponent to receive the transfer (in 2-player, roles swap).
    const activeOpps = (d.opp||[]).filter(o=>!o.out).length;
    const canPerevod = selectedCard && d.table.length>0
                       && !d.table.some(s=>s.d) && activeOpps >= 1;
    if (canPerevod) {
      ab.innerHTML+=`<button class="btn secondary" onclick="doPerevod()">Перевод</button>`;
    }
    ab.innerHTML+=`<button class="btn danger sm" onclick="doTake()">Взять</button>`;
  }
  ab.innerHTML+=`<button class="btn danger sm" onclick="leaveGame()">&#x2715; Выйти</button>`;
}

function doAttack() { if(selectedCard){ send({type:'attack',card:selectedCard}); selectedCard=null; } }
function doDefend() { if(selectedCard&&selectedSlot>=0){ send({type:'defend',slot:selectedSlot,card:selectedCard}); selectedCard=null; selectedSlot=-1; } }
function doPerevod() { if(selectedCard){ send({type:'perevod',card:selectedCard}); selectedCard=null; } }
function doPass() { send({type:'pass'}); }
function doTake() { send({type:'take'}); }

function hostGame() {
  document.getElementById('hostErr').textContent='';
  send({type:'hostGame', deck:parseInt(document.getElementById('deckSel').value), jokers:document.getElementById('jokersCb').checked});
}
function joinGame() {
  const code=document.getElementById('codeInput').value.trim();
  if (code.length!==8) { document.getElementById('joinErr').textContent='Enter 8-digit code.'; return; }
  document.getElementById('joinErr').textContent='';
  send({type:'joinGame',code});
}
function joinByCode(code) { send({type:'joinGame',code}); }
function startGame() { send({type:'startGame'}); }
function leaveGame() { send({type:'leaveGame'}); inRoom=false; myRoomCode=''; show('viewLobby'); send({type:'getLobbies'}); }
</script>
</body>
</html>
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
  loadModsNVS();
  for (int i=0;i<MAX_GAMES;i++) games[i].active=false;

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
  server.on("/durak", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!isSignedIn(req->client()->remoteIP())) { req->redirect("/"); return; }
    req->send_P(200, "text/html", DURAK_HTML);
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