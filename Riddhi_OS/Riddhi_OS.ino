#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <U8g2lib.h>

// ================= USER CONFIG =================
// Uncomment the ONE driver that matches your OLED:
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);   // 1.3" OLED
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE); // 0.96" OLED

const char* AP_SSID = "Riddhi-OS";
const char* AP_PASS = "12345678";

const char* WEATHER_LAT = "13.5503";
const char* WEATHER_LON = "78.5029";

#define IR_PIN 25
#define GMT_OFFSET_SEC 19800            // IST (UTC +5:30)
#define SLEEP_TIMEOUT_MS 45000          // Auto-sleep after 45s idle
#define WEATHER_INTERVAL_MS 900000UL    // 15 mins
#define NUDGE_INTERVAL_MS 2700000UL     // 45 mins
#define NTFY_INTERVAL_MS 35000UL        // 35 sec Love-Box poll
// ===============================================

WebServer server(80);
Preferences prefs;

enum ScreenMode {
  SCR_HOME = 0, SCR_MENU, SCR_CLOCK, SCR_WEATHER, SCR_POMODORO,
  SCR_PET, SCR_PC, SCR_SAVER, SCR_DINO, SCR_HELI, SCR_RACER,
  SCR_REACT, SCR_FISH, SCR_LOCK, SCR_BREATH, SCR_HABITS,
  SCR_COUNTER, SCR_LOVEBOX, SCR_NOTE, SCR_AI, SCR_WIFI_INFO,
  SCR_NUDGE, SCR_NOTIFY
};

enum Mood {
  MOOD_NORMAL = 0, MOOD_HAPPY, MOOD_LOVE, MOOD_ANGRY,
  MOOD_DIZZY, MOOD_SURPRISED, MOOD_SLEEP, MOOD_THINKING, MOOD_CUSTOM
};

ScreenMode currentScreen = SCR_HOME;
Mood currentMood = MOOD_NORMAL;
bool flippedDisplay = false;
bool nightMode = false, manualNight = false;

// Menu
const char* menuItems[] = {
  "Riddhi Face", "Live Clock", "Live Weather", "Pomodoro",
  "Pet Stats & XP", "Daily Habits", "Milestone Counter", "PC Monitor",
  "Screensavers", "Game: Dino Jump", "Game: Heli Cave", "Game: Micro Racer",
  "Game: Reaction", "Game: Pixel Fish", "Game: Timing Lock", "4-7-8 Breathing",
  "Love-Box Mail", "Phone Note", "AI Reply", "Wi-Fi / IP"
};
const int MENU_COUNT = 20;
int menuIndex = 0;
float menuHighlightY = 15.0f;

// Custom AI Face Shapes
struct CustomShape { char type; int p[7]; };
CustomShape customShapes[24];
int customShapeCount = 0;

// Smooth Eye Physics
float eyeCurX = 0, eyeTargetX = 0, eyeCurY = 0, eyeTargetY = 0;
float eyeCurLH = 26, eyeTargetLH = 26, eyeCurRH = 26, eyeTargetRH = 26;
unsigned long lastActivity = 0, nextBlinkAt = 2400, blinkPhaseEnd = 0, nextGlanceAt = 3000, tempMoodUntil = 0;
int blinkStage = 0;

// Persistent Pet, Habits & Counter
int petLevel = 1, petXP = 20, petHappy = 88, petEnergy = 92;
int habitMask = 0, habitStreak = 1; // 4 bits: Gym, Study, Water, Skincare
String counterLabel = "GOAL";
int counterVal = 0;
unsigned long fireworksUntil = 0;
unsigned long lastPetDecay = 0;

// GitHub OTA & Love-Box (ntfy.sh)
String ghRepo = "";          // e.g. "username/Riddhi"
bool triggerGhOta = false;
String ntfyTopic = "";       // e.g. "riddhi-secret-123"
String lastNtfyId = "";
String loveMsg = "No secret mail yet!";
bool loveUnread = false;
unsigned long lastNtfyPoll = 0;

// Nudge, Notify & PC
unsigned long lastNudgeAt = 0;
bool nudgeIsWater = true;
String notifyTitle = "iPhone", notifyMsg = "Connected!";
unsigned long notifyUntil = 0;
int pcCpu = 0, pcRam = 0, pcGpu = 0;
String pcTrack = "Run riddhi_pc.py";
unsigned long lastPcUpdate = 0;

// Screensavers
int saverMode = 0;
int matrixY[16];
float cubeAngle = 0.0f;
float starX[20], starY[20], starZ[20];

// AI, Weather, Pomodoro, Note
String aiReplyText = "Ask me anything or invent a face!";
Mood aiReplyMood = MOOD_HAPPY;
ScreenMode pendingScreen = SCR_HOME;
unsigned long pendingScreenAt = 0;
bool weatherNeedsFetch = false;
String toastText = "";
unsigned long toastUntil = 0;
bool weatherLoaded = false;
float weatherTemp = 0, weatherWind = 0;
int weatherHumidity = 0, weatherCode = 0;
unsigned long lastWeatherFetch = 0;
bool pomoRunning = false, pomoBreak = false;
unsigned long pomoMaxMs = 25UL * 60000UL, pomoRemainingMs = 25UL * 60000UL, pomoLastTick = 0;
String customNote = "Send a lovely note from your phone!";

// Arcade Games State
enum GameState { G_READY = 0, G_PLAY, G_OVER };
GameState dinoState = G_READY, heliState = G_READY, racerState = G_READY;
float dinoY = 56, dinoVel = 0, obsX = 132, gameSpeed = 3.2f;
int obsH = 14, dinoScore = 0, dinoHi = 0, groundOffset = 0;
float heliY = 32, heliVel = 0, wallX = 132;
int wallGapY = 24, heliScore = 0, heliHi = 0;
int playerLane = 1, carLane = 0, racerScore = 0, racerHi = 0;
float carX = 132, racerSpeed = 3.6f;
int reactStage = 0;
unsigned long gameOverAt = 0, reactGoAt = 0, reactResultMs = 0, reactBestMs = 9999;

// Pixel Fishing State
int fishStage = 0; // 0=Ready, 1=Waiting, 2=BITE!, 3=Caught/Missed
unsigned long fishBiteAt = 0, fishExpireAt = 0;
int fishCaught = 0;
String lastFishName = "";

// Timing Lock State
float lockPos = 64.0f, lockDir = 2.4f;
int lockLevel = 1, lockBest = 1;
bool lockOver = false;

// IR Engine
bool irStableState = false, irLastRaw = false, irHoldTriggered = false;
unsigned long irDebounceTime = 0, irPressStart = 0, irLastRelease = 0;
int irWaveCount = 0;
String activeIP = "192.168.4.1";
bool isAPMode = false;

// ================= PERSISTENT STORAGE =================
void saveState() {
  prefs.begin("riddhi", false);
  prefs.putInt("lv", petLevel);
  prefs.putInt("xp", petXP);
  prefs.putInt("hp", petHappy);
  prefs.putInt("en", petEnergy);
  prefs.putInt("hMask", habitMask);
  prefs.putInt("hStr", habitStreak);
  prefs.putString("cLbl", counterLabel);
  prefs.putInt("cVal", counterVal);
  prefs.putInt("dHi", dinoHi);
  prefs.putInt("hHi", heliHi);
  prefs.putInt("rHi", racerHi);
  prefs.putInt("fCnt", fishCaught);
  prefs.putInt("lBst", lockBest);
  prefs.putULong("reBst", reactBestMs);
  prefs.putString("ghRepo", ghRepo);
  prefs.putString("ntfy", ntfyTopic);
  prefs.end();
}

void loadState() {
  prefs.begin("riddhi", true);
  petLevel     = prefs.getInt("lv", 1);
  petXP        = prefs.getInt("xp", 20);
  petHappy     = prefs.getInt("hp", 88);
  petEnergy    = prefs.getInt("en", 92);
  habitMask    = prefs.getInt("hMask", 0);
  habitStreak  = prefs.getInt("hStr", 1);
  counterLabel = prefs.getString("cLbl", "GOAL");
  counterVal   = prefs.getInt("cVal", 0);
  dinoHi       = prefs.getInt("dHi", 0);
  heliHi       = prefs.getInt("hHi", 0);
  racerHi      = prefs.getInt("rHi", 0);
  fishCaught   = prefs.getInt("fCnt", 0);
  lockBest     = prefs.getInt("lBst", 1);
  reactBestMs  = prefs.getULong("reBst", 9999);
  ghRepo       = prefs.getString("ghRepo", "");
  ntfyTopic    = prefs.getString("ntfy", "");
  prefs.end();
}

// ================= LOVELY UI HELPERS =================
void showToast(const String& msg, unsigned long ms = 1500) { toastText = msg; toastUntil = millis() + ms; }

void drawSafeRBox(int x, int y, int w, int h, int r) {
  if (w < 2 || h < 2) return;
  int mr = min(w / 2, h / 2);
  u8g2.drawRBox(x, y, w, h, max(0, min(r, mr)));
}

void drawSafeRFrame(int x, int y, int w, int h, int r) {
  if (w < 2 || h < 2) return;
  int mr = min(w / 2, h / 2);
  u8g2.drawRFrame(x, y, w, h, max(0, min(r, mr)));
}

void drawSparkle(int cx, int cy, int s) {
  u8g2.drawHLine(cx - s, cy, s * 2 + 1);
  u8g2.drawVLine(cx, cy - s, s * 2 + 1);
}

void drawMiniHeart(int x, int y) {
  u8g2.drawPixel(x - 1, y); u8g2.drawPixel(x + 1, y);
  u8g2.drawHLine(x - 2, y + 1, 5);
  u8g2.drawHLine(x - 1, y + 2, 3);
  u8g2.drawPixel(x, y + 3);
}

void drawHeader(const char* title, const char* rightHint = "") {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(3, 9, title);
  if (strlen(rightHint) > 0) {
    int rw = u8g2.getStrWidth(rightHint);
    drawSafeRBox(124 - rw - 4, 1, rw + 6, 10, 3);
    u8g2.setDrawColor(0);
    u8g2.drawStr(124 - rw - 1, 9, rightHint);
    u8g2.setDrawColor(1);
  }
  u8g2.drawHLine(0, 12, 128);
}

void addPetXP(int xp, int happyBoost = 5, int energyDelta = 0) {
  petXP += xp;
  while (petXP >= 100) {
    petXP -= 100; petLevel++;
    showToast("LEVEL UP! Lv." + String(petLevel), 2400);
  }
  petHappy = constrain(petHappy + happyBoost, 0, 100);
  petEnergy = constrain(petEnergy + energyDelta, 0, 100);
  saveState();
}

void drawWrappedText(const String& text, int startY, int lh, int maxL, int maxC) {
  int len = text.length(), idx = 0, ln = 0;
  while (idx < len && ln < maxL) {
    while (idx < len && text[idx] == ' ') idx++;
    if (idx >= len) break;
    int endIdx = min(idx + maxC, len);
    if (endIdx < len && text[endIdx] != ' ') {
      int sp = text.lastIndexOf(' ', endIdx);
      if (sp > idx) endIdx = sp;
    }
    String line = text.substring(idx, endIdx); line.trim();
    u8g2.drawStr(3, startY + (ln * lh), line.c_str());
    idx = endIdx; ln++;
  }
}

bool isGameScreen(ScreenMode s) {
  return (s == SCR_DINO || s == SCR_HELI || s == SCR_RACER || s == SCR_REACT || s == SCR_FISH || s == SCR_LOCK);
}

void drawBootProgress(int pct, const char* label) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso18_tf);
  u8g2.drawStr((128 - u8g2.getStrWidth("RIDDHI")) / 2, 29, "RIDDHI");
  drawSafeRFrame(18, 37, 92, 9, 4);
  if (pct > 3) drawSafeRBox(20, 39, (88 * constrain(pct, 0, 100)) / 100, 5, 2);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr((128 - u8g2.getStrWidth(label)) / 2, 58, label);
  u8g2.sendBuffer();
}

// ================= 1-TAP GITHUB SELF-UPDATE =================
void runGitHubOTA() {
  if (WiFi.status() != WL_CONNECTED || ghRepo.length() < 3) {
    showToast("Set GitHub Repo!", 2000);
    return;
  }
  drawBootProgress(10, "Connecting GitHub...");
  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int cur, int total) {
    static unsigned long lastD = 0;
    if (millis() - lastD > 180 || cur == total) {
      lastD = millis();
      int pct = (total > 0) ? (cur * 100) / total : 50;
      char b[24]; sprintf(b, "GitHub OTA %d%%", pct);
      drawBootProgress(pct, b);
    }
  });

  String url = "https://github.com/" + ghRepo + "/releases/download/latest/Riddhi_OS.ino.bin";
  t_httpUpdate_return ret = httpUpdate.update(client, url);
  if (ret == HTTP_UPDATE_FAILED) {
    drawBootProgress(0, "GitHub OTA Failed!");
    delay(2000);
  }
}

// ================= REMOTE LOVE-BOX (NTFY.SH) =================
String extractJsonStr(const String& json, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int idx = json.indexOf(pat);
  if (idx < 0) return "";
  int st = idx + pat.length();
  int en = json.indexOf("\"", st);
  return (en > st) ? json.substring(st, en) : "";
}

void pollLoveBox() {
  if (WiFi.status() != WL_CONNECTED || ntfyTopic.length() < 2) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(3000);
  String url = "https://ntfy.sh/" + ntfyTopic + "/json?poll=1&since=latest";
  if (http.begin(client, url) && http.GET() == HTTP_CODE_OK) {
    String payload = http.getString();
    String id = extractJsonStr(payload, "id");
    String msg = extractJsonStr(payload, "message");
    if (id.length() > 0 && id != lastNtfyId && msg.length() > 0) {
      bool firstBoot = (lastNtfyId.length() == 0);
      lastNtfyId = id;
      loveMsg = msg;
      if (!firstBoot) {
        loveUnread = true;
        currentScreen = SCR_LOVEBOX;
        lastActivity = millis();
      }
    }
  }
  http.end();
}

// ================= CUSTOM AI FACE =================
void parseCustomShapes(const String& data) {
  customShapeCount = 0;
  int start = 0, len = data.length();
  while (start < len && customShapeCount < 24) {
    int semi = data.indexOf(';', start);
    if (semi < 0) semi = len;
    String tok = data.substring(start, semi); tok.trim();
    if (tok.length() >= 3) {
      CustomShape& s = customShapes[customShapeCount];
      s.type = tok[0];
      for (int i = 0; i < 7; i++) s.p[i] = 0;
      int pi = 0, cp = tok.indexOf(',');
      while (cp >= 0 && pi < 7) {
        int nc = tok.indexOf(',', cp + 1);
        s.p[pi++] = (nc < 0 ? tok.substring(cp + 1) : tok.substring(cp + 1, nc)).toInt();
        cp = nc;
      }
      customShapeCount++;
    }
    start = semi + 1;
  }
}

void renderCustomFace(int by) {
  for (int i = 0; i < customShapeCount; i++) {
    const CustomShape& s = customShapes[i];
    if (s.type == 'R') { u8g2.setDrawColor(s.p[5] ? 1 : 0); drawSafeRBox(s.p[0], s.p[1] + by, s.p[2], s.p[3], s.p[4]); }
    else if (s.type == 'F') { u8g2.setDrawColor(s.p[5] ? 1 : 0); drawSafeRFrame(s.p[0], s.p[1] + by, s.p[2], s.p[3], s.p[4]); }
    else if (s.type == 'D' && s.p[2] > 0) { u8g2.setDrawColor(s.p[3] ? 1 : 0); u8g2.drawDisc(s.p[0], s.p[1] + by, s.p[2]); }
    else if (s.type == 'C' && s.p[2] > 0) { u8g2.setDrawColor(s.p[3] ? 1 : 0); u8g2.drawCircle(s.p[0], s.p[1] + by, s.p[2]); }
    else if (s.type == 'L') { u8g2.setDrawColor(s.p[4] ? 1 : 0); u8g2.drawLine(s.p[0], s.p[1] + by, s.p[2], s.p[3] + by); }
    else if (s.type == 'T') { u8g2.setDrawColor(s.p[6] ? 1 : 0); u8g2.drawTriangle(s.p[0], s.p[1] + by, s.p[2], s.p[3] + by, s.p[4], s.p[5] + by); }
    else if (s.type == 'A' && s.p[2] > 0) {
      u8g2.setDrawColor(s.p[4] ? 1 : 0);
      u8g2.drawCircle(s.p[0], s.p[1] + by, s.p[2], s.p[3] ? (U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT) : (U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT));
    }
  }
  u8g2.setDrawColor(1);
}

// ================= BOOT ANIMATION =================
void playBootAnimation() {
  for (int h = 3; h <= 26; h += 3) {
    u8g2.clearBuffer();
    drawSafeRBox(31, 30 - h / 2, 22, h, 7); drawSafeRBox(75, 30 - h / 2, 22, h, 7);
    if (h > 14) { u8g2.setDrawColor(0); u8g2.drawDisc(38, 30 - h / 2 + 6, 2); u8g2.drawDisc(82, 30 - h / 2 + 6, 2); u8g2.setDrawColor(1); }
    u8g2.sendBuffer(); delay(30);
  }
  for (int f = 0; f < 8; f++) {
    u8g2.clearBuffer();
    int b = (f % 4 < 2) ? -1 : 1;
    u8g2.drawDisc(42, 34 + b, 12, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    u8g2.drawDisc(86, 34 + b, 12, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(42, 37 + b, 9, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    u8g2.drawDisc(86, 37 + b, 9, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    u8g2.setDrawColor(1);
    u8g2.drawCircle(64, 42 + b, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    drawMiniHeart(64, 14 - (f / 2));
    drawSparkle(16, 16, (f % 2 == 0) ? 3 : 2); drawSparkle(112, 18, (f % 2 == 0) ? 2 : 3);
    u8g2.sendBuffer(); delay(70);
  }
}

// ================= RIDDHI FACE ENGINE =================
void drawRiddhiFace() {
  unsigned long now = millis(); float t = now * 0.003f;
  if (tempMoodUntil > 0 && now > tempMoodUntil) { tempMoodUntil = 0; currentMood = (petHappy < 30) ? MOOD_ANGRY : MOOD_NORMAL; }
  if (currentMood == MOOD_NORMAL && (now - lastActivity > SLEEP_TIMEOUT_MS)) currentMood = MOOD_SLEEP;

  eyeCurX += (eyeTargetX - eyeCurX) * 0.20f; eyeCurY += (eyeTargetY - eyeCurY) * 0.20f;
  eyeCurLH += (eyeTargetLH - eyeCurLH) * 0.42f; eyeCurRH += (eyeTargetRH - eyeCurRH) * 0.42f;
  int breath = (int)round(sin(t) * 1.4f), ox = (int)round(eyeCurX), oy = (int)round(eyeCurY) + breath;

  switch (currentMood) {
    case MOOD_NORMAL: {
      if (blinkStage == 0 && now > nextBlinkAt) {
        blinkStage = 1; bool wink = (random(0, 6) == 0);
        eyeTargetLH = 3.0f; eyeTargetRH = wink ? 26.0f : 3.0f; blinkPhaseEnd = now + (wink ? 240 : 95);
      } else if (blinkStage == 1 && now > blinkPhaseEnd) { blinkStage = 2; eyeTargetLH = 26; eyeTargetRH = 26; blinkPhaseEnd = now + 110; }
      else if (blinkStage == 2 && now > blinkPhaseEnd) { blinkStage = 0; nextBlinkAt = now + ((random(0, 5) == 0) ? 260 : random(2600, 5000)); }
      if (now > nextGlanceAt) { eyeTargetX = random(0, 3) ? random(-10, 11) : 0; eyeTargetY = random(-4, 5); nextGlanceAt = now + random(2400, 4800); }
      int lh = (int)round(eyeCurLH), rh = (int)round(eyeCurRH), cy = 28 + oy, lx = 31 + ox, rx = 75 + ox;
      drawSafeRBox(lx, cy - lh / 2, 22, lh, 7); drawSafeRBox(rx, cy - rh / 2, 22, rh, 7);
      u8g2.setDrawColor(0);
      if (lh > 14) { u8g2.drawDisc(lx + 6 + ox / 4, cy - lh / 2 + 6, 2); u8g2.drawPixel(lx + 11 + ox / 4, cy - lh / 2 + 10); }
      if (rh > 14) { u8g2.drawDisc(rx + 6 + ox / 4, cy - rh / 2 + 6, 2); u8g2.drawPixel(rx + 11 + ox / 4, cy - rh / 2 + 10); }
      u8g2.setDrawColor(1);
      u8g2.drawHLine(22 + ox / 2, 41 + oy / 2, 5); u8g2.drawHLine(101 + ox / 2, 41 + oy / 2, 5);
      u8g2.drawCircle(64 + ox / 2, 44 + oy / 2, 4, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    }
    case MOOD_HAPPY: {
      int b = (int)round(sin(t * 2.5f) * 2.0f);
      u8g2.drawDisc(42, 34 + b, 13, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      u8g2.drawDisc(86, 34 + b, 13, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      u8g2.setDrawColor(0); u8g2.drawDisc(42, 37 + b, 10, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      u8g2.drawDisc(86, 37 + b, 10, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT); u8g2.setDrawColor(1);
      u8g2.drawRBox(20, 40 + b, 9, 3, 1); u8g2.drawRBox(99, 40 + b, 9, 3, 1);
      u8g2.drawCircle(64, 42 + b, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      drawSparkle(16, 15, ((now / 240) % 2 == 0) ? 3 : 2); drawSparkle(112, 16, ((now / 240) % 2 == 0) ? 2 : 3);
      break;
    }
    case MOOD_LOVE: {
      int p = ((now / 300) % 2 == 0) ? 1 : 0, cx[2] = {42, 86};
      for (int i = 0; i < 2; i++) { u8g2.drawDisc(cx[i]-5, 27, 6+p); u8g2.drawDisc(cx[i]+5, 27, 6+p); u8g2.drawTriangle(cx[i]-11-p, 29, cx[i]+11+p, 29, cx[i], 42+p); }
      drawMiniHeart(64, 15 - ((now / 110) % 10));
      u8g2.drawCircle(64, 45, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    }
    case MOOD_ANGRY: {
      int sh = ((now / 70) % 2 == 0) ? -1 : 1;
      drawSafeRBox(30+sh, 22, 24, 22, 5); drawSafeRBox(74+sh, 22, 24, 22, 5);
      u8g2.setDrawColor(0); u8g2.drawTriangle(28+sh, 18, 56+sh, 18, 56+sh, 33); u8g2.drawTriangle(72+sh, 18, 100+sh, 18, 72+sh, 33); u8g2.setDrawColor(1);
      u8g2.drawCircle(64, 52, 5, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    }
    case MOOD_DIZZY: {
      int dx = (int)(cos(t*3.2f)*4.5f), dy = (int)(sin(t*3.2f)*4.5f);
      u8g2.drawCircle(42, 31, 12); u8g2.drawDisc(42+dx, 31+dy, 4); u8g2.drawCircle(86, 31, 12); u8g2.drawDisc(86-dx, 31-dy, 4);
      break;
    }
    case MOOD_SURPRISED: {
      u8g2.drawDisc(42, 30, 14); u8g2.drawDisc(86, 30, 14);
      u8g2.setDrawColor(0); u8g2.drawDisc(42, 30, 6); u8g2.drawDisc(86, 30, 6); u8g2.setDrawColor(1);
      u8g2.drawCircle(64, 49, 4);
      break;
    }
    case MOOD_SLEEP: {
      drawSafeRBox(30, 35 + breath, 24, 4, 2); drawSafeRBox(74, 35 + breath, 24, 4, 2);
      int ph = (now / 120) % 18;
      u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(94 + ph / 4, 28 - ph / 2, "z");
      u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(104 + ((ph + 6) % 12) / 3, 22 - ((ph + 6) % 16) / 2, "Z");
      break;
    }
    case MOOD_THINKING: {
      drawSafeRBox(36, 20, 22, 18, 6); drawSafeRBox(80, 20, 22, 18, 6);
      for (int i = 0; i < 3; i++) u8g2.drawDisc(54 + i * 10, 10 - (i == (now / 220) % 3 ? 3 : 0), 2);
      break;
    }
    case MOOD_CUSTOM: renderCustomFace(breath); break;
  }

  if (WiFi.status() == WL_CONNECTED) { u8g2.drawPixel(3, 5); u8g2.drawCircle(3, 5, 2, U8G2_DRAW_UPPER_RIGHT); }
  if (pomoRunning && (now / 500) % 2 == 0) u8g2.drawDisc(12, 4, 2);
  if (loveUnread && (now / 400) % 2 == 0) drawMiniHeart(22, 2);

  if (now < toastUntil && toastText.length() > 0) {
    u8g2.setFont(u8g2_font_5x7_tf); int tw = min(124, u8g2.getStrWidth(toastText.c_str()) + 12); int tx = (128 - tw) / 2;
    u8g2.setDrawColor(0); u8g2.drawRBox(tx - 1, 51, tw + 2, 12, 5); u8g2.setDrawColor(1);
    u8g2.drawRBox(tx, 52, tw, 10, 4); u8g2.setDrawColor(0); u8g2.drawStr(tx + 6, 60, toastText.c_str()); u8g2.setDrawColor(1);
  }
}

// ================= ARCADE PACK (6 GAMES + BREATHING) =================
void handleArcadeTap() {
  unsigned long now = millis(); lastActivity = now;
  if (currentScreen == SCR_DINO) {
    if (dinoState == G_READY || (dinoState == G_OVER && now - gameOverAt > 450)) { dinoY = 56; dinoVel = 0; obsX = 136; obsH = random(11, 17); gameSpeed = 3.2f; dinoScore = 0; dinoState = G_PLAY; }
    else if (dinoState == G_PLAY && dinoY >= 55.0f) dinoVel = -6.8f;
  } else if (currentScreen == SCR_HELI) {
    if (heliState == G_READY || (heliState == G_OVER && now - gameOverAt > 450)) { heliY = 32; heliVel = 0; wallX = 136; wallGapY = random(16, 36); heliScore = 0; heliState = G_PLAY; }
  } else if (currentScreen == SCR_RACER) {
    if (racerState == G_READY || (racerState == G_OVER && now - gameOverAt > 450)) { playerLane = 1; carLane = random(0, 3); carX = 136; racerSpeed = 3.6f; racerScore = 0; racerState = G_PLAY; }
    else if (racerState == G_PLAY) playerLane = (playerLane + 1) % 3;
  } else if (currentScreen == SCR_REACT) {
    if (reactStage == 0 || reactStage == 3) { reactStage = 1; reactGoAt = now + random(1500, 4000); }
    else if (reactStage == 1) { reactResultMs = 0; reactStage = 3; }
    else if (reactStage == 2) { reactResultMs = now - reactGoAt; if (reactResultMs < reactBestMs) reactBestMs = reactResultMs; addPetXP(5, 3, -1); reactStage = 3; }
  } else if (currentScreen == SCR_FISH) {
    if (fishStage == 0 || fishStage == 3) { fishStage = 1; fishBiteAt = now + random(1800, 4500); fishExpireAt = fishBiteAt + 950; }
    else if (fishStage == 1) { lastFishName = "Too Early!"; fishStage = 3; }
    else if (fishStage == 2) {
      const char* fNames[] = {"Goldfish!", "Koi Fish!", "Blue Tang!", "Rare Shark!"};
      lastFishName = fNames[random(0, 4)]; fishCaught++; addPetXP(8, 5, -1); fishStage = 3;
    }
  } else if (currentScreen == SCR_LOCK) {
    if (lockOver) { lockOver = false; lockLevel = 1; lockPos = 64; lockDir = 2.4f; }
    else if (lockPos >= 52 && lockPos <= 76) {
      lockLevel++; if (lockLevel > lockBest) lockBest = lockLevel;
      lockDir = (lockDir > 0 ? -1 : 1) * min(6.5f, 2.2f + lockLevel * 0.45f);
      addPetXP(6, 4, -1);
    } else { lockOver = true; }
  }
}

void drawDino() {
  if (dinoState == G_PLAY) {
    dinoY += dinoVel; dinoVel += 0.62f; if (dinoY >= 56) { dinoY = 56; dinoVel = 0; }
    obsX -= gameSpeed; groundOffset = (groundOffset + (int)gameSpeed) % 16;
    if (obsX < -10) { obsX = 128 + random(14, 46); obsH = random(11, 17); dinoScore++; if (gameSpeed < 6.4f) gameSpeed += 0.12f; }
    if (27 > obsX + 1 && 19 < obsX + 6 && dinoY - 1 > 56 - obsH + 1 && dinoY - 12 < 56) {
      dinoState = G_OVER; gameOverAt = millis(); if (dinoScore > dinoHi) dinoHi = dinoScore; addPetXP(dinoScore, 4, -2);
    }
  }
  char b[16]; sprintf(b, "HI:%02d SC:%02d", dinoHi, dinoScore); drawHeader("DINO", b);
  u8g2.drawHLine(0, 56, 128);
  for (int x = -groundOffset; x < 128; x += 16) u8g2.drawPixel(x, 59);
  int dy = (int)dinoY; u8g2.drawBox(21, dy-14, 7, 5); u8g2.drawBox(18, dy-9, 8, 6); u8g2.drawBox(19, dy-3, 2, 3); u8g2.drawBox(23, dy-3, 2, 3);
  if (dinoState != G_READY) u8g2.drawBox((int)obsX + 2, 56 - obsH, 4, obsH);
  if (dinoState == G_READY) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(16, 36, "Wave:Jump  Hold:Exit"); }
  else if (dinoState == G_OVER) { u8g2.setDrawColor(0); drawSafeRBox(22, 20, 84, 24, 4); u8g2.setDrawColor(1); drawSafeRFrame(22, 20, 84, 24, 4); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(36, 36, "GAME OVER"); }
}

void drawHeli() {
  if (heliState == G_PLAY) {
    heliVel = constrain(heliVel + (irStableState ? -0.55f : 0.48f), -3.5f, 3.5f); heliY += heliVel; wallX -= 3.0f;
    if (wallX < -10) { wallX = 132; wallGapY = random(16, 35); heliScore++; }
    if (heliY < 14 || heliY > 58 || (28 > wallX && 18 < wallX + 10 && (heliY - 3 < wallGapY || heliY + 3 > wallGapY + 22))) {
      heliState = G_OVER; gameOverAt = millis(); if (heliScore > heliHi) heliHi = heliScore; addPetXP(heliScore * 2, 4, -2);
    }
  }
  char b[16]; sprintf(b, "HI:%02d SC:%02d", heliHi, heliScore); drawHeader("HELI", b);
  u8g2.drawHLine(0, 62, 128);
  int hy = (int)heliY; drawSafeRBox(18, hy - 3, 11, 6, 2); u8g2.drawHLine(16, hy - 5, 13);
  if (heliState != G_READY) { u8g2.drawBox((int)wallX, 13, 10, wallGapY - 13); u8g2.drawBox((int)wallX, wallGapY + 22, 10, 40); }
  if (heliState == G_READY) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(10, 36, "Hold IR:Fly Up | Wave:Go"); }
  else if (heliState == G_OVER) { u8g2.setDrawColor(0); drawSafeRBox(26, 20, 76, 24, 4); u8g2.setDrawColor(1); drawSafeRFrame(26, 20, 76, 24, 4); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(38, 36, "CRASHED!"); }
}

void drawRacer() {
  int laneY[3] = {22, 38, 54};
  if (racerState == G_PLAY) {
    carX -= racerSpeed;
    if (carX < -14) { carX = 134; carLane = random(0, 3); racerScore++; if (racerSpeed < 7.0f) racerSpeed += 0.15f; }
    if (carLane == playerLane && carX < 28 && carX + 12 > 14) { racerState = G_OVER; gameOverAt = millis(); if (racerScore > racerHi) racerHi = racerScore; addPetXP(racerScore, 4, -2); }
  }
  char b[16]; sprintf(b, "HI:%02d SC:%02d", racerHi, racerScore); drawHeader("RACER", b);
  for (int x = 0; x < 128; x += 14) { u8g2.drawHLine(x, 30, 6); u8g2.drawHLine(x, 46, 6); }
  drawSafeRBox(14, laneY[playerLane] - 4, 14, 8, 2);
  if (racerState != G_READY) drawSafeRFrame((int)carX, laneY[carLane] - 4, 12, 8, 2);
  if (racerState == G_READY) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(14, 36, "Wave:Lane  Hold:Exit"); }
  else if (racerState == G_OVER) { u8g2.setDrawColor(0); drawSafeRBox(26, 20, 76, 24, 4); u8g2.setDrawColor(1); drawSafeRFrame(26, 20, 76, 24, 4); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(38, 36, "WRECKED!"); }
}

void drawReact() {
  unsigned long now = millis();
  char b[16]; sprintf(b, "BEST:%lums", reactBestMs == 9999 ? 0 : reactBestMs); drawHeader("REACT", b);
  if (reactStage == 0) { u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(18, 38, "Wave to Start!"); }
  else if (reactStage == 1) { if (now >= reactGoAt) reactStage = 2; else { u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(34, 38, "WAIT..."); } }
  else if (reactStage == 2) { drawSafeRBox(4, 16, 120, 44, 6); u8g2.setDrawColor(0); u8g2.setFont(u8g2_font_logisoso16_tf); u8g2.drawStr(20, 45, "WAVE NOW!"); u8g2.setDrawColor(1); }
  else {
    u8g2.setFont(u8g2_font_logisoso16_tf);
    if (reactResultMs == 0) u8g2.drawStr(20, 40, "TOO EARLY!");
    else { char r[16]; sprintf(r, "%lu ms!", reactResultMs); u8g2.drawStr(30, 40, r); }
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(16, 58, "Wave:Retry   Hold:Exit");
  }
}

void drawFishing() {
  unsigned long now = millis();
  char b[16]; sprintf(b, "CAUGHT:%d", fishCaught); drawHeader("FISHING", b);
  u8g2.drawHLine(0, 46, 128); // Water line
  if (fishStage == 0) { u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(16, 32, "Wave to Cast Line!"); }
  else if (fishStage == 1) {
    if (now >= fishBiteAt) fishStage = 2;
    int bob = (int)(sin(now * 0.006f) * 2.0f);
    u8g2.drawLine(10, 16, 64, 42 + bob); u8g2.drawDisc(64, 44 + bob, 4);
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(36, 26, "Waiting...");
  } else if (fishStage == 2) {
    if (now > fishExpireAt) { lastFishName = "Got Away!"; fishStage = 3; }
    int sh = ((now / 60) % 2) ? -2 : 2;
    u8g2.drawLine(10, 16, 64 + sh, 46); u8g2.drawDisc(64 + sh, 48, 5);
    u8g2.setFont(u8g2_font_logisoso16_tf); u8g2.drawStr(24, 34, "REEL NOW!");
  } else {
    u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(24, 34, lastFishName.c_str());
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(16, 58, "Wave:Cast  Hold:Exit");
  }
}

void drawLock() {
  if (!lockOver) {
    lockPos += lockDir;
    if (lockPos < 10) { lockPos = 10; lockDir = fabs(lockDir); }
    if (lockPos > 118) { lockPos = 118; lockDir = -fabs(lockDir); }
  }
  char b[16]; sprintf(b, "LV:%d HI:%d", lockLevel, lockBest); drawHeader("SAFE LOCK", b);
  drawSafeRFrame(8, 28, 112, 14, 4);
  u8g2.drawBox(52, 28, 24, 14); // Target sweet spot
  u8g2.setDrawColor(0); u8g2.drawBox(54, 30, 20, 10); u8g2.setDrawColor(1);
  u8g2.drawBox((int)lockPos - 2, 25, 4, 20); // Sweeping needle
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(16, 58, lockOver ? "MISSED! Wave:Retry" : "Wave in Center Zone!");
}

void drawBreathing() {
  drawHeader("4-7-8 BREATH", "4x:Home");
  unsigned long sec = (millis() / 1000) % 19; // 4s In + 7s Hold + 8s Out
  int r = 6; const char* lbl = "INHALE";
  if (sec < 4) { r = 6 + (int)((millis() % 19000) * 14 / 4000); lbl = "INHALE (4s)"; }
  else if (sec < 11) { r = 20; lbl = "HOLD (7s)"; }
  else { r = 20 - (int)(((millis() % 19000) - 11000) * 14 / 8000); lbl = "EXHALE (8s)"; }
  u8g2.drawCircle(34, 38, constrain(r, 4, 20));
  u8g2.drawDisc(34, 38, constrain(r - 4, 1, 16));
  u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(62, 41, lbl);
}

// ================= NEW LIFESTYLE SCREENS =================
void drawHabits() {
  char b[16]; sprintf(b, "STREAK:%d", habitStreak); drawHeader("HABITS", b);
  const char* hNames[4] = {"Gym", "Study", "Water", "Skincare"};
  u8g2.setFont(u8g2_font_6x10_tf);
  for (int i = 0; i < 4; i++) {
    int x = (i % 2) * 64 + 4, y = (i / 2) * 20 + 28;
    bool done = (habitMask & (1 << i));
    u8g2.drawFrame(x, y - 8, 9, 9);
    if (done) u8g2.drawBox(x + 2, y - 6, 5, 5);
    u8g2.drawStr(x + 13, y, hNames[i]);
  }
  u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(14, 61, "Check off on Phone!");
}

void drawCounter() {
  drawHeader(counterLabel.c_str(), "1x:+1");
  char b[16]; sprintf(b, "%d", counterVal);
  u8g2.setFont(u8g2_font_logisoso24_tn);
  u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 46, b);
  if (millis() < fireworksUntil) {
    drawSparkle(18, 24, ((millis() / 150) % 2) ? 4 : 2);
    drawSparkle(110, 26, ((millis() / 150) % 2) ? 2 : 4);
    drawSparkle(24, 52, 3); drawSparkle(104, 50, 3);
  }
}

void drawLoveBox() {
  if (loveUnread) {
    drawHeader("SECRET MAIL!", "1x:Open");
    int bob = (int)(sin(millis() * 0.008f) * 3.0f);
    drawSafeRFrame(36, 22 + bob, 56, 32, 4);
    u8g2.drawLine(36, 24 + bob, 64, 40 + bob);
    u8g2.drawLine(92, 24 + bob, 64, 40 + bob);
    drawMiniHeart(64, 38 + bob);
  } else {
    drawHeader("LOVE-BOX", "4x:Home");
    u8g2.setFont(u8g2_font_6x10_tf);
    drawWrappedText(loveMsg, 26, 11, 3, 20);
  }
}

// ================= CORE SCREENS =================
void initSavers() {
  for (int i = 0; i < 16; i++) matrixY[i] = random(-60, 0);
  for (int i = 0; i < 20; i++) { starX[i] = random(-64, 64); starY[i] = random(-32, 32); starZ[i] = random(1, 64); }
}

void drawScreensaver() {
  if (saverMode == 0) {
    u8g2.setFont(u8g2_font_5x7_tf);
    for (int i = 0; i < 16; i++) {
      matrixY[i] += 3; if (matrixY[i] > 76) matrixY[i] = random(-20, 0);
      char c[2] = {(char)random(33, 90), 0}; u8g2.drawStr(i * 8, matrixY[i], c);
    }
  } else if (saverMode == 1) {
    cubeAngle += 0.06f; float s = sin(cubeAngle), c = cos(cubeAngle);
    float v[8][3] = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    int px[8], py[8];
    for (int i = 0; i < 8; i++) {
      float x = v[i][0]*c - v[i][2]*s, z = v[i][0]*s + v[i][2]*c;
      float y = v[i][1]*cos(cubeAngle*0.6f) - z*sin(cubeAngle*0.6f);
      z = v[i][1]*sin(cubeAngle*0.6f) + z*cos(cubeAngle*0.6f) + 3.5f;
      px[i] = 64 + (int)(x * 48.0f / z); py[i] = 32 + (int)(y * 48.0f / z);
    }
    int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (int i = 0; i < 12; i++) u8g2.drawLine(px[e[i][0]], py[e[i][0]], px[e[i][1]], py[e[i][1]]);
  } else {
    for (int i = 0; i < 20; i++) {
      starZ[i] -= 2.2f; if (starZ[i] <= 1.0f) { starX[i] = random(-64, 64); starY[i] = random(-32, 32); starZ[i] = 64.0f; }
      int sx = 64 + (int)(starX[i]*40.0f/starZ[i]), sy = 32 + (int)(starY[i]*40.0f/starZ[i]);
      if (sx >= 0 && sx < 128 && sy >= 0 && sy < 64) { if (starZ[i] < 24) u8g2.drawDisc(sx, sy, 1); else u8g2.drawPixel(sx, sy); }
    }
  }
}

void drawPetStats() {
  char lv[12]; sprintf(lv, "LV.%d", petLevel); drawHeader("RIDDHI PET", lv);
  drawMiniHeart(6, 20 + ((millis() / 400) % 2));
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(14, 24, "HAPPY"); drawSafeRFrame(44, 17, 80, 8, 3); drawSafeRBox(46, 19, (76 * petHappy) / 100, 4, 2);
  u8g2.drawStr(14, 38, "ENRGY"); drawSafeRFrame(44, 31, 80, 8, 3); drawSafeRBox(46, 33, (76 * petEnergy) / 100, 4, 2);
  u8g2.drawStr(14, 52, "XP");    drawSafeRFrame(44, 45, 80, 8, 3); drawSafeRBox(46, 47, (76 * petXP) / 100, 4, 2);
  u8g2.drawStr(14, 63, "Saved Permanently!");
}

void drawPcMonitor() {
  drawHeader("PC STATS", (millis() - lastPcUpdate < 15000) ? "LIVE" : "WAIT");
  u8g2.setFont(u8g2_font_5x7_tf);
  char b1[16]; sprintf(b1, "CPU %d%%", pcCpu); u8g2.drawStr(2, 24, b1); drawSafeRFrame(48, 17, 76, 7, 3); drawSafeRBox(50, 19, (72 * constrain(pcCpu,0,100)) / 100, 3, 1);
  char b2[16]; sprintf(b2, "RAM %d%%", pcRam); u8g2.drawStr(2, 35, b2); drawSafeRFrame(48, 28, 76, 7, 3); drawSafeRBox(50, 30, (72 * constrain(pcRam,0,100)) / 100, 3, 1);
  char b3[16]; sprintf(b3, "GPU %dC", pcGpu); u8g2.drawStr(2, 46, b3); drawSafeRFrame(48, 39, 76, 7, 3); drawSafeRBox(50, 41, (72 * constrain(pcGpu,0,100)) / 100, 3, 1);
  u8g2.drawHLine(0, 50, 128); u8g2.drawStr(2, 61, pcTrack.substring(0, 24).c_str());
}

void drawNudge() {
  drawHeader(nudgeIsWater ? "HYDRATION" : "POSTURE", "Wave:OK");
  int bob = (int)(sin(millis() * 0.006f) * 2.0f);
  if (nudgeIsWater) { drawSafeRFrame(14, 22 + bob, 18, 24, 3); u8g2.drawBox(16, 32 + bob, 14, 12); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(42, 32, "Drink Water!"); }
  else { u8g2.drawCircle(23, 24 + bob, 5); u8g2.drawVLine(23, 29 + bob, 14); u8g2.drawLine(14, 33 + bob, 32, 33 + bob); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(42, 32, "Stretch Up!"); }
  u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(42, 46, "Wave for +10 XP");
}

void drawNotify() {
  if (millis() > notifyUntil) { currentScreen = SCR_HOME; return; }
  drawHeader(notifyTitle.c_str(), "ALERT");
  u8g2.setFont(u8g2_font_6x10_tf); drawWrappedText(notifyMsg, 26, 11, 3, 20);
}

float extractCurrentValue(const String& json, const char* key, float fb) {
  int ci = json.indexOf("\"current\":{"); if (ci < 0) return fb;
  int ki = json.indexOf(key, ci); if (ki < 0) return fb;
  int vs = ki + strlen(key), ve = vs;
  while (ve < (int)json.length() && (isDigit(json[ve]) || json[ve] == '.' || json[ve] == '-')) ve++;
  return json.substring(vs, ve).toFloat();
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http; http.setTimeout(3500);
  http.begin(String("http://api.open-meteo.com/v1/forecast?latitude=") + WEATHER_LAT + "&longitude=" + WEATHER_LON + "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m");
  if (http.GET() == HTTP_CODE_OK) {
    String p = http.getString();
    weatherTemp = extractCurrentValue(p, "\"temperature_2m\":", weatherTemp);
    weatherHumidity = (int)extractCurrentValue(p, "\"relative_humidity_2m\":", weatherHumidity);
    weatherCode = (int)extractCurrentValue(p, "\"weather_code\":", weatherCode);
    weatherWind = extractCurrentValue(p, "\"wind_speed_10m\":", weatherWind);
    weatherLoaded = true; lastWeatherFetch = millis();
  }
  http.end();
}

void drawWeather() {
  drawHeader("WEATHER", "1x:Sync");
  if (WiFi.status() != WL_CONNECTED) { u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(8, 38, "Connect Wi-Fi first"); return; }
  if (!weatherLoaded) { fetchWeather(); return; }
  char tb[12]; sprintf(tb, "%.1fC", weatherTemp);
  u8g2.setFont(u8g2_font_logisoso18_tf); u8g2.drawStr(24, 38, tb);
  char sb[28]; sprintf(sb, "RH:%d%%   Wind:%.0fkm/h", weatherHumidity, weatherWind);
  u8g2.drawHLine(0, 50, 128); u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(10, 61, sb);
}

void drawClock() {
  struct tm ti; drawHeader(nightMode ? "NIGHTSTAND" : "CLOCK", "IST");
  if (getLocalTime(&ti, 10)) {
    char tb[12], db[24], ap[6];
    strftime(tb, sizeof(tb), "%I:%M", &ti); strftime(ap, sizeof(ap), "%p", &ti); strftime(db, sizeof(db), "%a, %d %b", &ti);
    u8g2.setFont(u8g2_font_logisoso24_tn); u8g2.drawStr(14, 44, tb);
    u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(98, 34, ap);
    if (!nightMode) { u8g2.drawHLine(16, 49, 96); u8g2.drawStr(32, 61, db); }
  } else { u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(10, 38, "Syncing Wi-Fi time..."); }
}

void drawPomodoro() {
  unsigned long now = millis();
  if (pomoRunning) {
    unsigned long el = now - pomoLastTick; pomoLastTick = now;
    if (pomoRemainingMs > el) pomoRemainingMs -= el;
    else {
      pomoRemainingMs = 0; pomoRunning = false; currentScreen = SCR_HOME; currentMood = MOOD_HAPPY;
      tempMoodUntil = now + 5000; addPetXP(30, 15, -5); showToast("Done! +30 XP", 3000); return;
    }
  }
  char b[10], t[16]; sprintf(b, "%02d:%02d", (int)((pomoRemainingMs / 1000) / 60), (int)((pomoRemainingMs / 1000) % 60));
  sprintf(t, "%s %dM", pomoBreak ? "BREAK" : "FOCUS", (int)(pomoMaxMs / 60000UL));
  drawHeader(t, pomoRunning ? "RUN" : "PAUSE");
  u8g2.setFont(u8g2_font_logisoso22_tn); u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 43, b);
  int bw = pomoMaxMs > 0 ? (int)((116UL * pomoRemainingMs) / pomoMaxMs) : 0;
  drawSafeRFrame(4, 51, 120, 9, 4); if (bw > 2) drawSafeRBox(6, 53, min(116, bw), 5, 2);
}

void drawMenu() {
  drawHeader("RIDDHI v5", "2x:Next");
  int st = (menuIndex >= 4) ? menuIndex - 3 : 0;
  float targetY = 15.0f + (menuIndex - st) * 12.0f;
  menuHighlightY += (targetY - menuHighlightY) * 0.35f;
  drawSafeRBox(2, (int)round(menuHighlightY), 118, 11, 4);
  u8g2.setFont(u8g2_font_6x10_tf);
  for (int i = 0; i < 4 && (st + i) < MENU_COUNT; i++) {
    int idx = st + i, y = 24 + i * 12;
    u8g2.setDrawColor(idx == menuIndex ? 0 : 1);
    u8g2.drawStr(8, y, menuItems[idx]);
  }
  u8g2.setDrawColor(1);
  u8g2.drawVLine(125, 15, 46);
  drawSafeRBox(124, 15 + (menuIndex * 36) / (MENU_COUNT - 1), 3, 10, 1);
}

void drawWifiInfo() {
  drawHeader("PHONE LINK", "Hold:Reset");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(3, 25, isAPMode ? "WiFi: Riddhi-OS" : "http://riddhi.local");
  u8g2.drawStr(3, 38, isAPMode ? "Pass: 12345678" : "or IP address:");
  u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(3, 55, ("http://" + activeIP).c_str());
}

// ================= IR GESTURE ENGINE =================
void triggerGesture(int waves, bool isHold) {
  lastActivity = millis(); pendingScreenAt = 0;
  if (currentScreen == SCR_NUDGE) {
    currentScreen = SCR_HOME; currentMood = MOOD_HAPPY; tempMoodUntil = millis() + 2000;
    addPetXP(10, 10, 10); showToast("Nice! +10 XP", 1600); return;
  }
  if (currentScreen == SCR_NOTIFY) { currentScreen = SCR_HOME; return; }
  if (waves >= 4) { currentScreen = SCR_HOME; currentMood = MOOD_NORMAL; showToast("Home", 800); return; }

  if (isHold) {
    if (isGameScreen(currentScreen)) {
      if (currentScreen == SCR_HELI && heliState == G_PLAY) return;
      currentScreen = SCR_HOME; currentMood = MOOD_NORMAL;
      dinoState = G_READY; heliState = G_READY; racerState = G_READY; reactStage = 0; fishStage = 0; lockOver = false;
      showToast("GG!", 1000);
    } else if (currentScreen == SCR_HOME) {
      currentMood = MOOD_LOVE; tempMoodUntil = millis() + 2600; addPetXP(8, 12, 0); showToast("<3 +8XP", 1400);
    } else if (currentScreen == SCR_POMODORO) {
      pomoRunning = false; pomoBreak = !pomoBreak; pomoMaxMs = (pomoBreak ? 5UL : 25UL) * 60000UL; pomoRemainingMs = pomoMaxMs;
    } else if (currentScreen == SCR_WIFI_INFO) {
      WiFi.disconnect(true, true); delay(500); ESP.restart();
    }
    return;
  }

  if (waves == 3) {
    if (currentScreen == SCR_MENU) {
      ScreenMode mapM[] = {
        SCR_HOME, SCR_CLOCK, SCR_WEATHER, SCR_POMODORO, SCR_PET, SCR_HABITS,
        SCR_COUNTER, SCR_PC, SCR_SAVER, SCR_DINO, SCR_HELI, SCR_RACER,
        SCR_REACT, SCR_FISH, SCR_LOCK, SCR_BREATH, SCR_LOVEBOX, SCR_NOTE, SCR_AI, SCR_WIFI_INFO
      };
      currentScreen = mapM[menuIndex];
      dinoState = G_READY; heliState = G_READY; racerState = G_READY; reactStage = 0; fishStage = 0; lockOver = false;
    } else currentScreen = SCR_MENU;
    return;
  }
  if (waves == 2) {
    if (currentScreen == SCR_MENU) menuIndex = (menuIndex + 1) % MENU_COUNT;
    else if (currentScreen == SCR_HOME) { currentMood = (Mood)(((int)currentMood + 1) % 5); tempMoodUntil = 0; }
    else currentScreen = SCR_MENU;
    return;
  }
  if (waves == 1) {
    if (isGameScreen(currentScreen)) handleArcadeTap();
    else if (currentScreen == SCR_LOVEBOX && loveUnread) { loveUnread = false; addPetXP(5, 10, 0); }
    else if (currentScreen == SCR_COUNTER) { counterVal++; saveState(); if (counterVal % 10 == 0) fireworksUntil = millis() + 2500; }
    else if (currentScreen == SCR_SAVER) saverMode = (saverMode + 1) % 3;
    else if (currentScreen == SCR_WEATHER) fetchWeather();
    else if (currentScreen == SCR_POMODORO) { pomoRunning = !pomoRunning; pomoLastTick = millis(); }
    else if (currentScreen == SCR_MENU) menuIndex = (menuIndex + 1) % MENU_COUNT;
    else if (currentScreen == SCR_HOME) {
      currentMood = MOOD_HAPPY; tempMoodUntil = millis() + 2200; addPetXP(4, 6, 0);
      showToast("Hehe! +4XP", 1200);
    }
  }
}

void updateIRSensor() {
  unsigned long now = millis();
  bool raw = (digitalRead(IR_PIN) == LOW);
  if (raw != irLastRaw) { irDebounceTime = now; irLastRaw = raw; }
  if (now - irDebounceTime > 35 && raw != irStableState) {
    irStableState = raw;
    if (irStableState) {
      irPressStart = now; irHoldTriggered = false;
      if (isGameScreen(currentScreen)) handleArcadeTap();
    } else if (!irHoldTriggered && !isGameScreen(currentScreen)) {
      irWaveCount++; irLastRelease = now;
    }
  }
  if (irStableState && !irHoldTriggered && (now - irPressStart > 950)) {
    irHoldTriggered = true; irWaveCount = 0; triggerGesture(0, true);
  }
  if (!irStableState && irWaveCount > 0 && (now - irLastRelease > 520)) {
    int c = irWaveCount; irWaveCount = 0; triggerGesture(c, false);
  }
}

// ================= 5-PAGE iOS WEB APP UI =================
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Riddhi OS">
<title>Riddhi OS v5</title><style>
:root{--bg:#0b0f19;--card:rgba(24,31,52,.88);--btn:#1e2640;--acc:#38bdf8;--ai:#a855f7;--pink:#f472b6;--ok:#34d399;--red:#fb7185;}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:radial-gradient(circle at top,#1e1b4b 0%,var(--bg) 55%);color:#f8fafc;margin:0;padding:16px 14px 86px;min-height:100vh;}
.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px;padding:0 4px;}
.brand{display:flex;align-items:center;gap:8px;}.dot{width:9px;height:9px;border-radius:50%;background:var(--ok);box-shadow:0 0 10px var(--ok);}
h2{margin:0;font-size:20px;background:linear-gradient(90deg,#fff,#c4b5fd,#f9a8d4);-webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.pill{font-size:11px;font-weight:700;padding:4px 10px;border-radius:20px;background:rgba(56,189,248,.15);color:var(--acc);border:1px solid rgba(56,189,248,.3);}
.page{display:none;animation:fade .18s ease;}.page.active{display:block;}
@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.card{background:var(--card);border:1px solid rgba(255,255,255,.08);border-radius:20px;padding:14px;margin-bottom:12px;box-shadow:0 8px 24px rgba(0,0,0,.28);}
.ai{border-color:rgba(168,85,247,.4);}
.t{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:1.1px;color:#94a3b8;margin-bottom:10px;text-align:left;}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:8px;}.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;}
.row{display:flex;gap:7px;margin-bottom:8px;align-items:center;}
button{background:var(--btn);color:#f1f5f9;border:1px solid rgba(255,255,255,.08);border-radius:14px;padding:11px 8px;font-size:13px;font-weight:600;cursor:pointer;}
button:active{transform:scale(.95);filter:brightness(1.25);}
.acc{background:linear-gradient(135deg,#0284c7,#38bdf8);color:#041524;border:none;}
.aib{background:linear-gradient(135deg,#7c3aed,#ec4899);color:#fff;border:none;}
.ok{background:linear-gradient(135deg,#059669,#34d399);color:#022c22;border:none;}
.pink{background:linear-gradient(135deg,#db2777,#f472b6);color:#fff;border:none;}
.red{background:rgba(244,63,94,.2);border-color:var(--red);color:#fda4af;}
.done{background:rgba(52,211,153,.2);border-color:var(--ok);color:#6ee7b7;}
input,select{width:100%;padding:10px;border-radius:12px;border:1px solid rgba(255,255,255,.12);background:rgba(9,13,22,.75);color:#fff;margin-bottom:8px;font-size:13px;}
.stat{margin-bottom:8px;text-align:left;font-size:12px;color:#cbd5e1;}
.sbar{width:100%;height:7px;background:#0f172a;border-radius:6px;overflow:hidden;margin-top:4px;}
.sfill{height:100%;border-radius:6px;transition:width .3s;}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px;}
.chip{background:rgba(168,85,247,.16);border:1px solid rgba(168,85,247,.4);color:#e9d5ff;padding:6px 10px;border-radius:20px;font-size:11px;font-weight:600;}
#aiLog{text-align:left;background:rgba(9,13,22,.7);border-radius:12px;padding:12px;min-height:48px;font-size:13px;color:#e2e8f0;margin-bottom:10px;line-height:1.4;}
.gcard{text-align:left;padding:11px;border-radius:14px;background:rgba(15,23,42,.7);border:1px solid rgba(255,255,255,.07);display:flex;justify-content:space-between;align-items:center;margin-bottom:7px;}
.gcard b{display:block;font-size:13px;margin-bottom:2px;}.gcard span{font-size:11px;color:#94a3b8;}
.sub{font-size:11px;color:#38bdf8;margin-top:6px;text-align:left;}
.nav{position:fixed;bottom:0;left:0;right:0;background:rgba(11,15,25,.95);backdrop-filter:blur(12px);border-top:1px solid rgba(255,255,255,.08);display:flex;justify-content:space-around;padding:8px 4px 12px;z-index:99;}
.nb{background:none;border:none;color:#64748b;display:flex;flex-direction:column;align-items:center;gap:3px;font-size:10px;padding:4px 10px;border-radius:12px;}
.nb.active{color:#38bdf8;background:rgba(56,189,248,.12);}
.nb span{font-size:16px;}
#toast{position:fixed;top:14px;left:50%;transform:translateX(-50%);background:#38bdf8;color:#041524;padding:6px 14px;border-radius:20px;font-size:12px;font-weight:700;display:none;z-index:100;}
</style></head><body>

<div id="toast">Sent!</div>
<div class="top"><div class="brand"><span class="dot"></span><h2>Riddhi OS v5</h2></div><span id="lvPill" class="pill">LV.1</span></div>

<!-- PAGE 1: HOME -->
<div id="p-home" class="page active">
  <div class="card">
    <div class="t">Riddhi Pet (Saved Permanently)</div>
    <div class="stat">Happiness <span id="vHp" style="float:right">88%</span><div class="sbar"><div id="bHp" class="sfill" style="width:88%;background:#f472b6"></div></div></div>
    <div class="stat">Energy <span id="vEn" style="float:right">92%</span><div class="sbar"><div id="bEn" class="sfill" style="width:92%;background:#34d399"></div></div></div>
    <div class="stat" style="margin:0">Level XP <span id="vXp" style="float:right">20%</span><div class="sbar"><div id="bXp" class="sfill" style="width:20%;background:#38bdf8"></div></div></div>
  </div>
  <div class="card">
    <div class="t">Daily Habits (Streak: <span id="hStr">1</span>)</div>
    <div class="g2">
      <button id="hb0" onclick="togHab(0)">Gym</button>
      <button id="hb1" onclick="togHab(1)">Study</button>
      <button id="hb2" onclick="togHab(2)">Water</button>
      <button id="hb3" onclick="togHab(3)">Skincare</button>
    </div>
  </div>
  <div class="card">
    <div class="t">Moods & Quick Note</div>
    <div class="g3" style="margin-bottom:10px">
      <button onclick="cmd('/mood?m=0','Chill')">Chill</button>
      <button onclick="cmd('/mood?m=1','Happy')">Happy</button>
      <button class="pink" onclick="cmd('/mood?m=2','Love')">Love</button>
      <button onclick="cmd('/mood?m=3','Angry')">Angry</button>
      <button onclick="cmd('/mood?m=4','Dizzy')">Dizzy</button>
      <button onclick="cmd('/mood?m=6','Sleep')">Sleep</button>
    </div>
    <div class="row" style="margin:0">
      <input id="noteTxt" style="margin:0;flex:1" maxlength="60" placeholder="Send note to OLED...">
      <button class="acc" style="width:70px" onclick="sendNote()">Send</button>
    </div>
  </div>
</div>

<!-- PAGE 2: AI -->
<div id="p-ai" class="page">
  <div class="card ai">
    <div class="t">Gemini AI (With Memory & Personas)</div>
    <select id="aiPers">
      <option value="cute">Persona: Cute Desk Buddy</option>
      <option value="coach">Persona: Strict Study Coach</option>
      <option value="gym">Persona: Hype Gym Motivator</option>
      <option value="hacker">Persona: Sarcastic Cyber Hacker</option>
    </select>
    <div id="aiLog">Ask anything, command apps, or invent custom faces!</div>
    <div class="chips">
      <button class="chip" onclick="quickAI('Wear cool sunglasses')">Sunglasses</button>
      <button class="chip" onclick="quickAI('Make a cute cat face')">Cat Face</button>
      <button class="chip" onclick="quickAI('Start 25 min focus timer')">25m Focus</button>
      <button class="chip" onclick="quickAI('Open weather')">Weather</button>
    </div>
    <div class="row">
      <input id="aiP" style="margin:0;flex:1" placeholder="Talk or command Riddhi..." onkeydown="if(event.key==='Enter')askAI()">
      <button class="aib" style="width:66px" onclick="askAI()">Ask</button>
    </div>
    <div class="g3">
      <button id="ttsBtn" style="font-size:11px;padding:8px" onclick="toggleTTS()">Voice: OFF</button>
      <button style="font-size:11px;padding:8px" onclick="saveFavFace()">Save Face</button>
      <button style="font-size:11px;padding:8px" onclick="toggleKB()">API Key</button>
    </div>
    <div id="kb" style="display:none;margin-top:10px;border-top:1px solid rgba(255,255,255,.1);padding-top:10px">
      <input id="ak" type="password" placeholder="Paste Google AI Studio API Key">
      <select id="am"><option value="gemini-3.5-flash-lite">gemini-3.5-flash-lite (Fastest)</option><option value="gemini-3.8-flash">gemini-3.8-flash (Smarter)</option></select>
      <button class="acc" style="width:100%" onclick="saveK()">Save Key</button>
    </div>
  </div>
  <div class="card">
    <div class="t">Favorite AI Faces Gallery</div>
    <div id="favBox" class="chips" style="margin:0"></div>
  </div>
</div>

<!-- PAGE 3: PLAY -->
<div id="p-play" class="page">
  <div class="card">
    <div class="t">IR Arcade Pack (6 Games)</div>
    <div class="gcard"><div><b>Dino Jump</b><span>Wave to jump over cacti</span></div><button class="ok" style="width:68px" onclick="cmd('/scr?s=8','Dino!')">Play</button></div>
    <div class="gcard"><div><b>Heli Cave</b><span>Hold IR to fly up, release to drop</span></div><button class="ok" style="width:68px" onclick="cmd('/scr?s=9','Heli!')">Play</button></div>
    <div class="gcard"><div><b>Micro Racer</b><span>Wave to dodge traffic lanes</span></div><button class="ok" style="width:68px" onclick="cmd('/scr?s=10','Racer!')">Play</button></div>
    <div class="gcard"><div><b>Reaction Test</b><span>Wave fast when screen flashes</span></div><button class="ok" style="width:68px" onclick="cmd('/scr?s=11','React!')">Play</button></div>
    <div class="gcard"><div><b>Pixel Fishing</b><span>Cast, wait for splash, reel in!</span></div><button class="ok" style="width:68px" onclick="cmd('/scr?s=12','Fish!')">Play</button></div>
    <div class="gcard" style="margin:0"><div><b>Timing Lock</b><span>Wave inside the center zone</span></div><button class="ok" style="width:68px" onclick="cmd('/scr?s=13','Lock!')">Play</button></div>
  </div>
  <div class="card">
    <div class="t">Virtual IR Remote</div>
    <div class="g2">
      <button onclick="cmd('/g?w=1','1x')">1x Interact / Jump</button>
      <button onclick="cmd('/g?w=2','2x')">2x Next</button>
      <button class="acc" onclick="cmd('/g?w=3','3x')">3x Menu / OK</button>
      <button class="acc" onclick="cmd('/g?w=4','4x')">4x Home</button>
    </div>
  </div>
</div>

<!-- PAGE 4: APPS -->
<div id="p-apps" class="page">
  <div class="card">
    <div class="t">Desk & Lifestyle Apps</div>
    <div class="g3">
      <button onclick="cmd('/scr?s=0','Home')">Riddhi</button>
      <button onclick="cmd('/scr?s=2','Clock')">Clock</button>
      <button onclick="cmd('/scr?s=3','Weather')">Weather</button>
      <button onclick="cmd('/scr?s=4','Pomo')">Pomodoro</button>
      <button onclick="cmd('/scr?s=15','Habits')">Habits</button>
      <button onclick="cmd('/scr?s=14','Breathe')">4-7-8 Zen</button>
      <button onclick="cmd('/scr?s=17','Mail')">Love-Box</button>
      <button onclick="cmd('/scr?s=6','PC')">PC Stats</button>
      <button onclick="cmd('/night','Night Mode')">Night Dim</button>
    </div>
  </div>
  <div class="card">
    <div class="t">Live Milestone Counter</div>
    <div class="row">
      <input id="cLbl" style="margin:0;flex:1" placeholder="Label (e.g. SUBS)" maxlength="10">
      <input id="cVal" type="number" style="margin:0;width:84px" placeholder="0">
      <button class="acc" style="width:60px" onclick="setCnt()">Set</button>
    </div>
    <div class="g2">
      <button onclick="cmd('/scr?s=16','Counter')">Show on OLED</button>
      <button class="ok" onclick="cmd('/cnt?inc=1','+1!')">+1 Milestone</button>
    </div>
  </div>
  <div class="card">
    <div class="t">Ambient Screensavers</div>
    <div class="g3">
      <button onclick="cmd('/saver?m=0','Matrix')">Matrix</button>
      <button onclick="cmd('/saver?m=1','3D Cube')">3D Cube</button>
      <button onclick="cmd('/saver?m=2','Starfield')">Starfield</button>
    </div>
  </div>
</div>

<!-- PAGE 5: SYSTEM -->
<div id="p-sys" class="page">
  <div class="card">
    <div class="t">1-Tap GitHub Self-Update</div>
    <input id="ghRepo" placeholder="GitHub repo (e.g. yourname/Riddhi)">
    <div class="g2">
      <button onclick="saveGh()">Save Repo</button>
      <button class="aib" onclick="pullGh()">Pull Latest Update</button>
    </div>
    <div class="sub">Pulls Riddhi_OS.ino.bin directly from GitHub Releases!</div>
  </div>
  <div class="card">
    <div class="t">Remote Love-Box (ntfy.sh Topic)</div>
    <div class="row" style="margin:0">
      <input id="ntfyIn" style="margin:0;flex:1" placeholder="Secret topic (e.g. riddhi-mail-99)">
      <button class="pink" style="width:70px" onclick="saveNtfy()">Save</button>
    </div>
    <div class="sub">Anyone can send mail via ntfy.sh/YOUR-TOPIC</div>
  </div>
  <div class="card">
    <div class="t">Manual Web OTA (.bin Upload)</div>
    <input type="file" id="otaFile" accept=".bin">
    <button class="acc" style="width:100%" onclick="uploadOTA()">Flash .bin Firmware</button>
    <div id="otaBar" class="sbar" style="display:none;margin-top:8px"><div id="otaFill" class="sfill" style="width:0%;background:#38bdf8"></div></div>
    <div id="otaSt" class="sub"></div>
  </div>
  <div class="card">
    <div class="t">Wi-Fi & Display</div>
    <button style="width:100%;margin-bottom:8px" onclick="scanWifi()">Scan Nearby 2.4GHz Wi-Fi</button>
    <div id="nets" class="chips"></div>
    <input id="ssid" placeholder="Wi-Fi Name (SSID)"><input id="pass" type="password" placeholder="Wi-Fi Password">
    <div class="g3">
      <button class="acc" onclick="saveW()">Save Wi-Fi</button>
      <button onclick="cmd('/flip','Flipped!')">Flip OLED</button>
      <button class="red" onclick="cmd('/resetwifi','Resetting')">Reset</button>
    </div>
    <div id="wSt" class="sub"></div>
  </div>
</div>

<nav class="nav">
  <button class="nb active" onclick="go('home',this)"><span>&#9733;</span>Home</button>
  <button class="nb" onclick="go('ai',this)"><span>&#10024;</span>AI</button>
  <button class="nb" onclick="go('play',this)"><span>&#9654;</span>Play</button>
  <button class="nb" onclick="go('apps',this)"><span>&#9783;</span>Apps</button>
  <button class="nb" onclick="go('sys',this)"><span>&#9881;</span>System</button>
</nav>

<script>
let ttsOn=false, aiHist=[], lastShapes='', lastFaceName='Custom';
const sk=localStorage.getItem('rk')||''; document.getElementById('ak').value=sk;
if(!sk) document.getElementById('kb').style.display='block';

function go(id,el){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.nb').forEach(b=>b.classList.remove('active'));
  document.getElementById('p-'+id).classList.add('active'); el.classList.add('active');
  if(id==='home'||id==='sys') syncStatus();
  if(id==='ai') renderFavs();
}
function toast(m){const t=document.getElementById('toast');t.innerText=m;t.style.display='block';setTimeout(()=>t.style.display='none',1200);}
function cmd(u,msg){fetch(u);if(msg)toast(msg);setTimeout(syncStatus,300);}
function sendNote(){const v=document.getElementById('noteTxt').value.trim();if(!v)return;cmd('/note?t='+encodeURIComponent(v),'Note Sent!');document.getElementById('noteTxt').value='';}
function togHab(i){cmd('/habit?i='+i,'Habit Updated!');}
function setCnt(){cmd('/cnt?l='+encodeURIComponent(document.getElementById('cLbl').value||'GOAL')+'&v='+encodeURIComponent(document.getElementById('cVal').value||0),'Counter Set!');}
function saveGh(){cmd('/cfg?gh='+encodeURIComponent(document.getElementById('ghRepo').value.trim()),'Repo Saved!');}
function pullGh(){if(confirm('Pull latest Riddhi_OS.ino.bin from GitHub and reboot?')) cmd('/ghota','Updating from GitHub...');}
function saveNtfy(){cmd('/cfg?ntfy='+encodeURIComponent(document.getElementById('ntfyIn').value.trim()),'Love-Box Saved!');}
function toggleKB(){const b=document.getElementById('kb');b.style.display=b.style.display==='none'?'block':'none';}
function toggleTTS(){ttsOn=!ttsOn;document.getElementById('ttsBtn').innerText='Voice: '+(ttsOn?'ON':'OFF');}
function saveK(){localStorage.setItem('rk',document.getElementById('ak').value.trim());localStorage.setItem('rm',document.getElementById('am').value);document.getElementById('kb').style.display='none';toast('Key Saved!');}
function quickAI(txt){document.getElementById('aiP').value=txt;askAI();}

function renderFavs(){
  const box=document.getElementById('favBox'); const favs=JSON.parse(localStorage.getItem('r_favs')||'[]');
  box.innerHTML=favs.length?'':'<span style="font-size:12px;color:#64748b">Ask AI to draw a face, then tap Save Face!</span>';
  favs.forEach(f=>{
    const b=document.createElement('button'); b.className='chip'; b.innerText=f.n;
    b.onclick=()=>{const bd=new URLSearchParams({m:1,act:'CUSTOM_FACE',t:f.n,shapes:f.s});fetch('/ai',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:bd});toast(f.n);};
    box.appendChild(b);
  });
}
function saveFavFace(){
  if(!lastShapes){toast('Draw a custom face first!');return;}
  const favs=JSON.parse(localStorage.getItem('r_favs')||'[]');
  favs.push({n:lastFaceName.slice(0,14),s:lastShapes});
  localStorage.setItem('r_favs',JSON.stringify(favs.slice(-10)));
  renderFavs(); toast('Face Saved!');
}

async function syncStatus(){
  try{
    const r=await fetch('/status'); const s=await r.json();
    document.getElementById('lvPill').innerText='LV.'+s.lv;
    document.getElementById('vHp').innerText=s.hp+'%'; document.getElementById('bHp').style.width=s.hp+'%';
    document.getElementById('vEn').innerText=s.en+'%'; document.getElementById('bEn').style.width=s.en+'%';
    document.getElementById('vXp').innerText=s.xp+'%'; document.getElementById('bXp').style.width=s.xp+'%';
    document.getElementById('hStr').innerText=s.hs;
    for(let i=0;i<4;i++) document.getElementById('hb'+i).className=(s.hm&(1<<i))?'done':'';
    if(!document.getElementById('ghRepo').value) document.getElementById('ghRepo').value=s.gh||'';
    if(!document.getElementById('ntfyIn').value) document.getElementById('ntfyIn').value=s.ntfy||'';
  }catch(e){}
}
syncStatus();

async function scanWifi(){
  const st=document.getElementById('wSt'), box=document.getElementById('nets');
  st.innerText='Scanning...'; box.innerHTML='';
  const lines=(await(await fetch('/scan')).text()).split('\n').filter(x=>x.trim());
  st.innerText=lines.length?'Tap a network:':'No 2.4GHz networks found.';
  lines.forEach(n=>{const b=document.createElement('button');b.className='chip';b.innerText=n;b.onclick=()=>document.getElementById('ssid').value=n;box.appendChild(b);});
}
function saveW(){const s=document.getElementById('ssid').value.trim();if(!s)return;document.getElementById('wSt').innerText='Rebooting...';fetch('/savewifi?s='+encodeURIComponent(s)+'&p='+encodeURIComponent(document.getElementById('pass').value));}

function uploadOTA(){
  const f=document.getElementById('otaFile').files[0], st=document.getElementById('otaSt'), bar=document.getElementById('otaBar'), fill=document.getElementById('otaFill');
  if(!f){st.innerText='Select a .bin file first!';return;}
  bar.style.display='block'; fill.style.width='0%'; st.innerText='Uploading...';
  const fd=new FormData(); fd.append('update',f,f.name);
  const xhr=new XMLHttpRequest(); xhr.open('POST','/update',true);
  xhr.upload.onprogress=(e)=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);fill.style.width=p+'%';st.innerText='Flashing: '+p+'%';}};
  xhr.onload=()=>{if(xhr.status===200&&xhr.responseText==='OK'){fill.style.width='100%';st.innerText='Success! Rebooting...';setTimeout(()=>location.reload(),5000);}else st.innerText='Update Failed!';};
  xhr.send(fd);
}

function ser(a){if(!Array.isArray(a))return'';const cl=(v,l,h)=>Math.max(l,Math.min(h,Math.round(+v||0))),o=[];
for(const s of a.slice(0,24)){const t=(s.type||'').toLowerCase(),c=(s.c===0)?0:1;
if(t==='rbox')o.push(`R,${cl(s.x,0,127)},${cl(s.y,0,63)},${cl(s.w,2,128)},${cl(s.h,2,64)},${cl(s.r,0,20)},${c}`);
else if(t==='disc')o.push(`D,${cl(s.x,0,127)},${cl(s.y,0,63)},${cl(s.r,1,32)},${c}`);
else if(t==='circle')o.push(`C,${cl(s.x,0,127)},${cl(s.y,0,63)},${cl(s.r,1,32)},${c}`);
else if(t==='line')o.push(`L,${cl(s.x1,0,127)},${cl(s.y1,0,63)},${cl(s.x2,0,127)},${cl(s.y2,0,63)},${c}`);
else if(t==='arc')o.push(`A,${cl(s.x,0,127)},${cl(s.y,0,63)},${cl(s.r,1,30)},${s.dir?1:0},${c}`);}return o.join(';');}

async function askAI(){
  const k=localStorage.getItem('rk')||'', m=localStorage.getItem('rm')||'gemini-3.5-flash-lite', p=document.getElementById('aiP').value.trim(), lg=document.getElementById('aiLog');
  if(!k){document.getElementById('kb').style.display='block';lg.innerText='Paste your Gemini API key first.';return;}
  if(!p)return; document.getElementById('aiP').value=''; lg.innerText='Riddhi is thinking...'; fetch('/aithink');
  const pMap={cute:'cute witty desk buddy',coach:'strict motivating study coach',gym:'high-energy gym bro motivator',hacker:'sarcastic cyberpunk hacker'};
  const sys='You are Riddhi, a '+pMap[document.getElementById('aiPers').value]+' ESP32 robot (128x64 OLED). Return ONLY JSON: {"mood":"NORMAL|HAPPY|LOVE|ANGRY|DIZZY|SURPRISED|SLEEP","action":"NONE|CUSTOM_FACE|POMO_START|WEATHER|CLOCK|DINO|HELI|RACER|REACT|FISH|LOCK|BREATH|SAVER|PC|PET","arg":0,"reply":"ASCII <50 chars","shapes":[]}. For custom faces use action="CUSTOM_FACE" with shapes (rbox,disc,circle,line,arc).';
  aiHist.push({role:'user',parts:[{text:p}]}); if(aiHist.length>6) aiHist=aiHist.slice(-6);
  try{
    const r=await fetch('https://generativelanguage.googleapis.com/v1beta/models/'+m+':generateContent?key='+encodeURIComponent(k),{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({system_instruction:{parts:[{text:sys}]},generationConfig:{responseMimeType:'application/json'},contents:aiHist})});
    const d=await r.json(); const raw=d.candidates[0].content.parts[0].text.replace(/^```json|```$/gi,'').trim();
    const j=JSON.parse(raw); aiHist.push({role:'model',parts:[{text:j.reply||'OK'}]});
    const mm={NORMAL:0,HAPPY:1,LOVE:2,ANGRY:3,DIZZY:4,SURPRISED:5,SLEEP:6}; lg.innerText=j.reply||'Done!';
    const b=new URLSearchParams(); b.set('m',mm[(j.mood||'HAPPY').toUpperCase()]??1); b.set('act',(j.action||'NONE').toUpperCase()); b.set('arg',j.arg||0); b.set('t',(j.reply||'').replace(/[^\x20-\x7E]/g,'').slice(0,60));
    const sh=ser(j.shapes); if(sh){b.set('shapes',sh);lastShapes=sh;lastFaceName=p;}
    await fetch('/ai',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b.toString()});
    if(ttsOn&&'speechSynthesis'in window){window.speechSynthesis.cancel();window.speechSynthesis.speak(new SpeechSynthesisUtterance(j.reply||''));}
  }catch(e){lg.innerText='Err: '+e.message;}
}
</script></body></html>
)rawliteral";

void setupWebServer() {
  server.on("/", []() { server.send_P(200, "text/html", HTML_PAGE); });

  server.on("/status", []() {
    String j = "{\"lv\":" + String(petLevel) + ",\"xp\":" + String(petXP) +
               ",\"hp\":" + String(petHappy) + ",\"en\":" + String(petEnergy) +
               ",\"hm\":" + String(habitMask) + ",\"hs\":" + String(habitStreak) +
               ",\"gh\":\"" + ghRepo + "\",\"ntfy\":\"" + ntfyTopic + "\"}";
    server.send(200, "application/json", j);
  });

  server.on("/cfg", []() {
    if (server.hasArg("gh")) ghRepo = server.arg("gh");
    if (server.hasArg("ntfy")) { ntfyTopic = server.arg("ntfy"); lastNtfyId = ""; }
    saveState(); server.send(200, "text/plain", "OK");
  });

  server.on("/ghota", []() {
    server.send(200, "text/plain", "OK");
    triggerGhOta = true;
  });

  server.on("/habit", []() {
    int i = server.arg("i").toInt();
    if (i >= 0 && i < 4) {
      habitMask ^= (1 << i);
      if (habitMask & (1 << i)) addPetXP(10, 8, 5);
      if (habitMask == 15) { habitStreak++; showToast("ALL HABITS! +Streak", 2200); }
      saveState();
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/cnt", []() {
    if (server.hasArg("l")) counterLabel = server.arg("l");
    if (server.hasArg("v")) counterVal = server.arg("v").toInt();
    if (server.hasArg("inc")) counterVal += server.arg("inc").toInt();
    saveState(); currentScreen = SCR_COUNTER;
    if (counterVal > 0 && counterVal % 10 == 0) fireworksUntil = millis() + 2500;
    server.send(200, "text/plain", "OK");
  });

  server.on("/night", []() {
    manualNight = !manualNight; nightMode = manualNight;
    u8g2.setContrast(nightMode ? 5 : 255);
    if (nightMode) currentScreen = SCR_CLOCK;
    server.send(200, "text/plain", "OK");
  });

  // Manual Web OTA
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(400); if (!Update.hasError()) ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    static unsigned long lastOtaDraw = 0;
    if (upload.status == UPLOAD_FILE_START) { drawBootProgress(5, "Starting OTA..."); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      Update.write(upload.buf, upload.currentSize);
      if (millis() - lastOtaDraw > 180) { lastOtaDraw = millis(); char lbl[24]; sprintf(lbl, "Flashing %d KB...", upload.totalSize / 1024); drawBootProgress(constrain((upload.totalSize / 1024) * 100 / 1150, 8, 96), lbl); }
    } else if (upload.status == UPLOAD_FILE_END) { drawBootProgress(Update.end(true) ? 100 : 0, Update.hasError() ? "OTA Failed!" : "Rebooting..."); }
  });

  server.on("/g", []() { triggerGesture(server.arg("w").toInt(), false); server.send(200, "text/plain", "OK"); });
  server.on("/note", []() { if (server.hasArg("t")) { customNote = server.arg("t"); currentScreen = SCR_NOTE; lastActivity = millis(); } server.send(200, "text/plain", "OK"); });
  server.on("/mood", []() { currentScreen = SCR_HOME; currentMood = (Mood)server.arg("m").toInt(); tempMoodUntil = 0; lastActivity = millis(); server.send(200, "text/plain", "OK"); });
  server.on("/scr", []() { currentScreen = (ScreenMode)server.arg("s").toInt(); dinoState = G_READY; heliState = G_READY; racerState = G_READY; reactStage = 0; fishStage = 0; lockOver = false; lastActivity = millis(); server.send(200, "text/plain", "OK"); });
  server.on("/saver", []() { saverMode = server.arg("m").toInt() % 3; currentScreen = SCR_SAVER; lastActivity = millis(); server.send(200, "text/plain", "OK"); });
  server.on("/flip", []() { flippedDisplay = !flippedDisplay; u8g2.setFlipMode(flippedDisplay ? 1 : 0); server.send(200, "text/plain", "OK"); });
  server.on("/nudge", []() { nudgeIsWater = !nudgeIsWater; currentScreen = SCR_NUDGE; server.send(200, "text/plain", "OK"); });
  server.on("/notify", []() {
    notifyTitle = server.hasArg("title") ? server.arg("title") : "iPhone";
    notifyMsg = server.hasArg("msg") ? server.arg("msg") : "New Notification";
    notifyUntil = millis() + 6000; currentScreen = SCR_NOTIFY; lastActivity = millis();
    server.send(200, "text/plain", "OK");
  });
  server.on("/pc", []() {
    if (server.hasArg("cpu")) pcCpu = server.arg("cpu").toInt();
    if (server.hasArg("ram")) pcRam = server.arg("ram").toInt();
    if (server.hasArg("gpu")) pcGpu = server.arg("gpu").toInt();
    if (server.hasArg("track")) pcTrack = server.arg("track");
    lastPcUpdate = millis(); server.send(200, "text/plain", "OK");
  });
  server.on("/aithink", []() { currentScreen = SCR_HOME; currentMood = MOOD_THINKING; tempMoodUntil = millis() + 15000; showToast("Thinking...", 15000); server.send(200, "text/plain", "OK"); });
  server.on("/ai", HTTP_ANY, []() {
    int m = constrain(server.arg("m").toInt(), 0, 6);
    String act = server.arg("act"); act.toUpperCase();
    int argVal = server.arg("arg").toInt();
    String text = server.arg("t"), shapes = server.arg("shapes");
    aiReplyMood = (Mood)m; aiReplyText = text; currentScreen = SCR_HOME; currentMood = aiReplyMood; tempMoodUntil = 0; lastActivity = millis();
    pendingScreen = SCR_AI; pendingScreenAt = millis() + 1400;
    if (act == "CUSTOM_FACE" || shapes.length() > 0) { parseCustomShapes(shapes); if (customShapeCount > 0) { currentMood = MOOD_CUSTOM; pendingScreenAt = 0; showToast(text.substring(0, 20), 2400); } }
    else if (act == "POMO_START") { int mn = constrain(argVal > 0 ? argVal : 25, 1, 99); pomoBreak = false; pomoMaxMs = mn * 60000UL; pomoRemainingMs = pomoMaxMs; pomoRunning = true; pomoLastTick = millis(); pendingScreen = SCR_POMODORO; }
    else if (act == "WEATHER") { weatherNeedsFetch = true; pendingScreen = SCR_WEATHER; }
    else if (act == "CLOCK") pendingScreen = SCR_CLOCK;
    else if (act == "DINO") pendingScreen = SCR_DINO;
    else if (act == "HELI") pendingScreen = SCR_HELI;
    else if (act == "RACER") pendingScreen = SCR_RACER;
    else if (act == "REACT") pendingScreen = SCR_REACT;
    else if (act == "FISH") pendingScreen = SCR_FISH;
    else if (act == "LOCK") pendingScreen = SCR_LOCK;
    else if (act == "BREATH") pendingScreen = SCR_BREATH;
    else if (act == "SAVER") pendingScreen = SCR_SAVER;
    else if (act == "PC") pendingScreen = SCR_PC;
    else if (act == "PET") pendingScreen = SCR_PET;
    server.send(200, "text/plain", "OK");
  });
  server.on("/scan", []() {
    int n = WiFi.scanNetworks(); String list = "";
    for (int i = 0; i < n && i < 12; i++) { String ss = WiFi.SSID(i); if (ss.length() && list.indexOf(ss + "\n") == -1) list += ss + "\n"; }
    WiFi.scanDelete(); server.send(200, "text/plain", list);
  });
  server.on("/savewifi", []() {
    String s = server.arg("s"), p = server.arg("p"); server.send(200, "text/plain", "OK");
    WiFi.mode(WIFI_STA); WiFi.persistent(true); WiFi.begin(s.c_str(), p.c_str()); delay(800); ESP.restart();
  });
  server.on("/resetwifi", []() { server.send(200, "text/plain", "OK"); WiFi.disconnect(true, true); delay(500); ESP.restart(); });
  server.begin();
}

// ================= SETUP & LOOP =================
void setup() {
  Serial.begin(115200); pinMode(IR_PIN, INPUT); Wire.begin(21, 22); u8g2.begin();
  loadState(); initSavers(); playBootAnimation();

  WiFi.persistent(true); WiFi.mode(WIFI_STA); WiFi.begin();
  for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++) { drawBootProgress((i * 100) / 24, "Connecting Wi-Fi..."); delay(250); }

  if (WiFi.status() == WL_CONNECTED) {
    isAPMode = false; activeIP = WiFi.localIP().toString();
    MDNS.begin("riddhi");
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    fetchWeather();
  } else {
    isAPMode = true; WiFi.disconnect(false); WiFi.setAutoReconnect(false); delay(150);
    WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID, AP_PASS, 1); activeIP = WiFi.softAPIP().toString();
  }
  setupWebServer();
  u8g2.clearBuffer(); drawWifiInfo(); u8g2.sendBuffer(); delay(2500);
  lastActivity = millis(); lastNudgeAt = millis(); lastPetDecay = millis();
  showToast("Riddhi v5 Ready!", 1800);
}

void loop() {
  server.handleClient();
  updateIRSensor();
  static unsigned long lastFrame = 0, lastNightChk = 0;
  unsigned long now = millis();

  if (triggerGhOta) { triggerGhOta = false; runGitHubOTA(); }

  if (pendingScreenAt > 0 && now >= pendingScreenAt) {
    pendingScreenAt = 0; currentScreen = pendingScreen;
    if (weatherNeedsFetch) { weatherNeedsFetch = false; fetchWeather(); }
  }

  // Auto Nightstand Dimming (11:00 PM – 6:30 AM IST)
  if (!manualNight && now - lastNightChk > 60000UL) {
    lastNightChk = now; struct tm ti;
    if (getLocalTime(&ti, 5)) {
      bool shouldDim = (ti.tm_hour >= 23 || ti.tm_hour < 6 || (ti.tm_hour == 6 && ti.tm_min < 30));
      if (shouldDim != nightMode) { nightMode = shouldDim; u8g2.setContrast(nightMode ? 5 : 255); }
    }
  }

  if (now - lastPetDecay > 600000UL) { lastPetDecay = now; petHappy = max(0, petHappy - 2); petEnergy = max(0, petEnergy - 1); }
  if (currentScreen == SCR_HOME && (now - lastNudgeAt > NUDGE_INTERVAL_MS)) { lastNudgeAt = now; nudgeIsWater = !nudgeIsWater; currentScreen = SCR_NUDGE; }
  if (currentScreen == SCR_HOME && ntfyTopic.length() > 1 && (now - lastNtfyPoll > NTFY_INTERVAL_MS)) { lastNtfyPoll = now; pollLoveBox(); }
  if (WiFi.status() == WL_CONNECTED && !isGameScreen(currentScreen) && (now - lastWeatherFetch > WEATHER_INTERVAL_MS)) fetchWeather();

  if (now - lastFrame >= 33) {
    lastFrame = now;
    u8g2.clearBuffer();
    switch (currentScreen) {
      case SCR_HOME:      drawRiddhiFace(); break;
      case SCR_MENU:      drawMenu(); break;
      case SCR_CLOCK:     drawClock(); break;
      case SCR_WEATHER:   drawWeather(); break;
      case SCR_POMODORO:  drawPomodoro(); break;
      case SCR_PET:       drawPetStats(); break;
      case SCR_PC:        drawPcMonitor(); break;
      case SCR_SAVER:     drawScreensaver(); break;
      case SCR_DINO:      drawDino(); break;
      case SCR_HELI:      drawHeli(); break;
      case SCR_RACER:     drawRacer(); break;
      case SCR_REACT:     drawReact(); break;
      case SCR_FISH:      drawFishing(); break;
      case SCR_LOCK:      drawLock(); break;
      case SCR_BREATH:    drawBreathing(); break;
      case SCR_HABITS:    drawHabits(); break;
      case SCR_COUNTER:   drawCounter(); break;
      case SCR_LOVEBOX:   drawLoveBox(); break;
      case SCR_NOTE:      drawHeader("NOTE", "Phone"); u8g2.setFont(u8g2_font_6x10_tf); drawWrappedText(customNote, 26, 12, 3, 20); break;
      case SCR_AI:        drawHeader("RIDDHI AI", "1x:Face"); u8g2.setFont(u8g2_font_6x10_tf); drawWrappedText(aiReplyText, 26, 11, 3, 20); break;
      case SCR_WIFI_INFO: drawWifiInfo(); break;
      case SCR_NUDGE:     drawNudge(); break;
      case SCR_NOTIFY:    drawNotify(); break;
    }
    if (irWaveCount > 0 && !isGameScreen(currentScreen)) {
      for (int i = 0; i < irWaveCount && i < 4; i++) u8g2.drawDisc(122 - (i * 7), 5, 2);
    }
    u8g2.sendBuffer();
  }
}
