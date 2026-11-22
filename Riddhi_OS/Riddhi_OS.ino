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
#define NTFY_INTERVAL_MS 35000UL        // 35 sec Love-Box poll
#define NVS_SAVE_INTERVAL_MS 30000UL    // Lazy NVS flush every 30s
// ===============================================

WebServer server(80);
Preferences prefs;
SemaphoreHandle_t netMutex = NULL;

enum ScreenMode {
  SCR_HOME = 0, SCR_MENU, SCR_COUPLE, SCR_LOVEBOX, SCR_CLOCK,
  SCR_WEATHER, SCR_POMODORO, SCR_BREATH, SCR_SAVER, SCR_DINO,
  SCR_HELI, SCR_RACER, SCR_REACT, SCR_NOTE, SCR_AI, SCR_WIFI_INFO
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
  "Riddhi Face", "Days Together", "Love-Box Mail", "Live Clock",
  "Live Weather", "Pomodoro", "4-7-8 Zen", "Screensavers",
  "Game: Dino", "Game: Heli", "Game: Racer", "Game: React",
  "Sweet Note", "AI Reply", "Wi-Fi / IP"
};
const int MENU_COUNT = 15;
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

// Persistent Couple Data
String coupleNames = "You & Me";
int daysTogether = 1;
int lastDayChecked = -1;
bool stateDirty = false;
unsigned long lastNvsSave = 0;

// GitHub OTA & Love-Box (Shared with Core 0)
String ghRepo = "";
volatile bool triggerGhOta = false;
volatile bool netTaskPaused = false;
volatile bool reqWeatherNow = false;
volatile bool newLoveMailFlag = false;

String ntfyTopic = "";
String lastNtfyId = "";
String loveMsg = "No love mail yet! Set a topic in Mail.";
bool loveUnread = false;

// Screensavers
int saverMode = 0;
int matrixY[16];
float cubeAngle = 0.0f;
float starX[20], starY[20], starZ[20];

// AI, Weather, Pomodoro, Note
String aiReplyText = "Ask me to draw a cute face or write a love note!";
Mood aiReplyMood = MOOD_LOVE;
ScreenMode pendingScreen = SCR_HOME;
unsigned long pendingScreenAt = 0;
String toastText = "";
unsigned long toastUntil = 0;

volatile bool weatherLoaded = false;
float weatherTemp = 0, weatherWind = 0;
int weatherHumidity = 0, weatherCode = 0;

bool pomoRunning = false, pomoBreak = false;
unsigned long pomoMaxMs = 25UL * 60000UL, pomoRemainingMs = 25UL * 60000UL, pomoLastTick = 0;
String customNote = "Thinking of you <3";

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

// IR Engine
bool irStableState = false, irLastRaw = false, irHoldTriggered = false;
unsigned long irDebounceTime = 0, irPressStart = 0, irLastRelease = 0;
int irWaveCount = 0;
String activeIP = "192.168.4.1";
bool isAPMode = false;

// ================= PERSISTENT STORAGE =================
void saveStateNow() {
  prefs.begin("riddhi_lv", false);
  prefs.putString("cNames", coupleNames);
  prefs.putInt("dTog", daysTogether);
  prefs.putInt("lDay", lastDayChecked);
  prefs.putInt("dHi", dinoHi);
  prefs.putInt("hHi", heliHi);
  prefs.putInt("rHi", racerHi);
  prefs.putULong("reBst", reactBestMs);
  prefs.putString("ghRepo", ghRepo);
  prefs.putString("ntfy", ntfyTopic);
  prefs.end();
  stateDirty = false;
  lastNvsSave = millis();
}

void markDirty() { stateDirty = true; }

void loadState() {
  prefs.begin("riddhi_lv", true);
  coupleNames    = prefs.getString("cNames", "You & Me");
  daysTogether   = prefs.getInt("dTog", 1);
  lastDayChecked = prefs.getInt("lDay", -1);
  dinoHi         = prefs.getInt("dHi", 0);
  heliHi         = prefs.getInt("hHi", 0);
  racerHi        = prefs.getInt("rHi", 0);
  reactBestMs    = prefs.getULong("reBst", 9999);
  ghRepo         = prefs.getString("ghRepo", "");
  ntfyTopic      = prefs.getString("ntfy", "");
  prefs.end();
}

// ================= HELPERS =================
String jsonEscape(const String& s) {
  String out = ""; out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') out += '\\';
    if (c >= 0x20) out += c;
  }
  return out;
}

void showToast(const String& msg, unsigned long ms = 1800) { toastText = msg; toastUntil = millis() + ms; }

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
  return (s == SCR_DINO || s == SCR_HELI || s == SCR_RACER || s == SCR_REACT);
}

void drawBootProgress(int pct, const char* label) {
  u8g2.clearBuffer();
  drawMiniHeart(64, 6);
  u8g2.setFont(u8g2_font_logisoso18_tf);
  u8g2.drawStr((128 - u8g2.getStrWidth("RIDDHI")) / 2, 31, "RIDDHI");
  drawSafeRFrame(18, 39, 92, 8, 4);
  if (pct > 3) drawSafeRBox(20, 41, (88 * constrain(pct, 0, 100)) / 100, 4, 2);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr((128 - u8g2.getStrWidth(label)) / 2, 59, label);
  u8g2.sendBuffer();
}

// ================= CORE 0 BACKGROUND NETWORK TASK =================
float extractCurrentValue(const String& json, const char* key, float fb) {
  int ci = json.indexOf("\"current\":{"); if (ci < 0) return fb;
  int ki = json.indexOf(key, ci); if (ki < 0) return fb;
  int vs = ki + strlen(key), ve = vs;
  while (ve < (int)json.length() && (isDigit(json[ve]) || json[ve] == '.' || json[ve] == '-')) ve++;
  return json.substring(vs, ve).toFloat();
}

String extractJsonStr(const String& json, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int idx = json.indexOf(pat); if (idx < 0) return "";
  int st = idx + pat.length(), en = json.indexOf("\"", st);
  return (en > st) ? json.substring(st, en) : "";
}

void bgFetchWeather() {
  HTTPClient http; http.setTimeout(4000);
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + WEATHER_LAT +
               "&longitude=" + WEATHER_LON +
               "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m";
  if (http.begin(url) && http.GET() == HTTP_CODE_OK) {
    String p = http.getString();
    float t = extractCurrentValue(p, "\"temperature_2m\":", weatherTemp);
    int h = (int)extractCurrentValue(p, "\"relative_humidity_2m\":", weatherHumidity);
    int c = (int)extractCurrentValue(p, "\"weather_code\":", weatherCode);
    float w = extractCurrentValue(p, "\"wind_speed_10m\":", weatherWind);
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      weatherTemp = t; weatherHumidity = h; weatherCode = c; weatherWind = w;
      weatherLoaded = true; xSemaphoreGive(netMutex);
    }
  }
  http.end();
}

void bgPollLoveBox() {
  String topicCopy = "";
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) { topicCopy = ntfyTopic; xSemaphoreGive(netMutex); }
  if (topicCopy.length() < 2) return;

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(4000);
  String url = "https://ntfy.sh/" + topicCopy + "/json?poll=1&since=latest";
  if (http.begin(client, url) && http.GET() == HTTP_CODE_OK) {
    String payload = http.getString();
    String id = extractJsonStr(payload, "id");
    String msg = extractJsonStr(payload, "message");
    if (id.length() > 0 && id != lastNtfyId && msg.length() > 0) {
      bool firstBoot = (lastNtfyId.length() == 0);
      lastNtfyId = id;
      if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        loveMsg = msg;
        if (!firstBoot) newLoveMailFlag = true;
        xSemaphoreGive(netMutex);
      }
    }
  }
  http.end();
}

void networkTaskCore0(void* pvParameters) {
  unsigned long lastW = 0, lastN = 0;
  for (;;) {
    if (!netTaskPaused && WiFi.status() == WL_CONNECTED) {
      unsigned long now = millis();
      if (reqWeatherNow || lastW == 0 || (now - lastW > WEATHER_INTERVAL_MS)) {
        reqWeatherNow = false; bgFetchWeather(); lastW = millis();
      }
      if (now - lastN > NTFY_INTERVAL_MS) {
        bgPollLoveBox(); lastN = millis();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ================= CLEAN 1-TAP GITHUB SELF-UPDATE =================
void runGitHubOTA() {
  if (WiFi.status() != WL_CONNECTED || ghRepo.length() < 3) { showToast("Set GitHub Repo!", 2000); return; }
  if (stateDirty) saveStateNow();
  netTaskPaused = true; server.stop(); MDNS.end(); delay(250);

  drawBootProgress(10, "Connecting GitHub...");
  WiFiClientSecure client; client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int cur, int total) {
    static unsigned long lastD = 0;
    if (millis() - lastD > 180 || cur == total) {
      lastD = millis(); int pct = (total > 0) ? (cur * 100) / total : 50;
      char b[24]; sprintf(b, "GitHub OTA %d%%", pct); drawBootProgress(pct, b);
    }
  });

  String url = "https://github.com/" + ghRepo + "/releases/download/latest/Riddhi_OS.ino.bin";
  if (httpUpdate.update(client, url) == HTTP_UPDATE_FAILED) {
    drawBootProgress(0, "OTA Failed! Rebooting"); delay(2000); ESP.restart();
  }
}

// ================= BACKGROUND POMODORO & MIDNIGHT =================
void updatePomodoro() {
  if (!pomoRunning) return;
  unsigned long now = millis(), el = now - pomoLastTick; pomoLastTick = now;
  if (pomoRemainingMs > el) pomoRemainingMs -= el;
  else {
    pomoRemainingMs = 0; pomoRunning = false; currentScreen = SCR_HOME; currentMood = MOOD_LOVE;
    tempMoodUntil = now + 5000; showToast("Session Done! <3", 3000);
  }
}

void checkMidnightAndNightstand() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 30000UL) return;
  lastCheck = millis();
  struct tm ti; if (!getLocalTime(&ti, 5)) return;

  if (lastDayChecked == -1) { lastDayChecked = ti.tm_yday; markDirty(); }
  else if (ti.tm_yday != lastDayChecked) {
    daysTogether++; lastDayChecked = ti.tm_yday; saveStateNow();
  }

  if (!manualNight) {
    bool shouldDim = (ti.tm_hour >= 23 || ti.tm_hour < 6 || (ti.tm_hour == 6 && ti.tm_min < 30));
    if (shouldDim != nightMode) { nightMode = shouldDim; u8g2.setContrast(nightMode ? 5 : 255); }
  }
}

// ================= CUSTOM AI FACE =================
void parseCustomShapes(const String& data) {
  customShapeCount = 0; int start = 0, len = data.length();
  while (start < len && customShapeCount < 24) {
    int semi = data.indexOf(';', start); if (semi < 0) semi = len;
    String tok = data.substring(start, semi); tok.trim();
    if (tok.length() >= 3) {
      CustomShape& s = customShapes[customShapeCount]; s.type = tok[0];
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

// ================= ROMANTIC BOOT ANIMATION =================
void playBootAnimation() {
  for (int f = 0; f < 10; f++) {
    u8g2.clearBuffer();
    int p = (f % 2 == 0) ? 1 : 0, cx[2] = {42, 86};
    for (int i = 0; i < 2; i++) {
      u8g2.drawDisc(cx[i]-5, 27, 6+p); u8g2.drawDisc(cx[i]+5, 27, 6+p);
      u8g2.drawTriangle(cx[i]-11-p, 29, cx[i]+11+p, 29, cx[i], 42+p);
    }
    drawMiniHeart(64, 16 - f);
    drawSparkle(16, 16, (f % 2 == 0) ? 3 : 2); drawSparkle(112, 18, (f % 2 == 0) ? 2 : 3);
    u8g2.drawCircle(64, 46, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    u8g2.sendBuffer(); delay(85);
  }
}

// ================= RIDDHI FACE ENGINE =================
void drawRiddhiFace() {
  unsigned long now = millis(); float t = now * 0.003f;
  if (tempMoodUntil > 0 && now > tempMoodUntil) { tempMoodUntil = 0; currentMood = MOOD_NORMAL; }
  if (currentMood == MOOD_NORMAL && (now - lastActivity > SLEEP_TIMEOUT_MS)) currentMood = MOOD_SLEEP;

  eyeCurX += (eyeTargetX - eyeCurX) * 0.20f; eyeCurY += (eyeTargetY - eyeCurY) * 0.20f;
  eyeCurLH += (eyeTargetLH - eyeCurLH) * 0.42f; eyeCurRH += (eyeTargetRH - eyeCurRH) * 0.42f;
  int breath = (int)round(sin(t) * 1.4f), ox = (int)round(eyeCurX), oy = (int)round(eyeCurY) + breath;

  switch (currentMood) {
    case MOOD_NORMAL: {
      if (blinkStage == 0 && now > nextBlinkAt) {
        blinkStage = 1; bool wink = (random(0, 5) == 0);
        eyeTargetLH = 3.0f; eyeTargetRH = wink ? 26.0f : 3.0f; blinkPhaseEnd = now + (wink ? 260 : 95);
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
      drawMiniHeart(16, 14); drawMiniHeart(112, 14);
      break;
    }
    case MOOD_LOVE: {
      int p = ((now / 280) % 2 == 0) ? 1 : 0, cx[2] = {42, 86};
      for (int i = 0; i < 2; i++) { u8g2.drawDisc(cx[i]-5, 27, 6+p); u8g2.drawDisc(cx[i]+5, 27, 6+p); u8g2.drawTriangle(cx[i]-11-p, 29, cx[i]+11+p, 29, cx[i], 42+p); }
      drawMiniHeart(64, 15 - ((now / 100) % 10));
      drawMiniHeart(18, 22 - ((now / 130) % 8));
      drawMiniHeart(110, 22 - ((now / 130) % 8));
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
  if (loveUnread && (now / 350) % 2 == 0) drawMiniHeart(14, 2);

  if (now < toastUntil && toastText.length() > 0) {
    u8g2.setFont(u8g2_font_5x7_tf); int tw = min(124, u8g2.getStrWidth(toastText.c_str()) + 12); int tx = (128 - tw) / 2;
    u8g2.setDrawColor(0); u8g2.drawRBox(tx - 1, 51, tw + 2, 12, 5); u8g2.setDrawColor(1);
    u8g2.drawRBox(tx, 52, tw, 10, 4); u8g2.setDrawColor(0); u8g2.drawStr(tx + 6, 60, toastText.c_str()); u8g2.setDrawColor(1);
  }
}

// ================= COUPLE & LOVE-BOX SCREENS =================
void drawCoupleScreen() {
  drawHeader(coupleNames.c_str(), "LOVE");
  int pulse = (millis() / 450) % 2;
  drawMiniHeart(16, 28 - pulse); drawMiniHeart(112, 28 - pulse);

  char b[16]; sprintf(b, "%d", daysTogether);
  u8g2.setFont(u8g2_font_logisoso22_tn);
  u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 42, b);

  u8g2.setFont(u8g2_font_5x7_tf);
  const char* sub = "DAYS TOGETHER";
  u8g2.drawStr((128 - u8g2.getStrWidth(sub)) / 2, 56, sub);
}

void drawLoveBox() {
  if (loveUnread) {
    drawHeader("LOVE MAIL!", "1x:Open");
    int bob = (int)(sin(millis() * 0.008f) * 3.0f);
    drawSafeRFrame(36, 22 + bob, 56, 32, 4);
    u8g2.drawLine(36, 24 + bob, 64, 40 + bob);
    u8g2.drawLine(92, 24 + bob, 64, 40 + bob);
    drawMiniHeart(64, 38 + bob);
  } else {
    drawHeader("LOVE-BOX", "4x:Home");
    String msgCopy = "";
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(10)) == pdTRUE) { msgCopy = loveMsg; xSemaphoreGive(netMutex); }
    u8g2.setFont(u8g2_font_6x10_tf);
    drawWrappedText(msgCopy, 26, 11, 3, 20);
  }
}

// ================= ARCADE & UTILITY SCREENS =================
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
    else if (reactStage == 2) { reactResultMs = now - reactGoAt; if (reactResultMs < reactBestMs) { reactBestMs = reactResultMs; markDirty(); } reactStage = 3; }
  }
}

void drawDino() {
  if (dinoState == G_PLAY) {
    dinoY += dinoVel; dinoVel += 0.62f; if (dinoY >= 56) { dinoY = 56; dinoVel = 0; }
    obsX -= gameSpeed; groundOffset = (groundOffset + (int)gameSpeed) % 16;
    if (obsX < -10) { obsX = 128 + random(14, 46); obsH = random(11, 17); dinoScore++; if (gameSpeed < 6.4f) gameSpeed += 0.12f; }
    if (27 > obsX + 1 && 19 < obsX + 6 && dinoY - 1 > 56 - obsH + 1 && dinoY - 12 < 56) {
      dinoState = G_OVER; gameOverAt = millis(); if (dinoScore > dinoHi) { dinoHi = dinoScore; markDirty(); }
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
      heliState = G_OVER; gameOverAt = millis(); if (heliScore > heliHi) { heliHi = heliScore; markDirty(); }
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
    if (carLane == playerLane && carX < 28 && carX + 12 > 14) { racerState = G_OVER; gameOverAt = millis(); if (racerScore > racerHi) { racerHi = racerScore; markDirty(); } }
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

void drawBreathing() {
  drawHeader("4-7-8 ZEN", "4x:Home");
  unsigned long sec = (millis() / 1000) % 19;
  int r = 6; const char* lbl = "INHALE";
  if (sec < 4) { r = 6 + (int)((millis() % 19000) * 14 / 4000); lbl = "INHALE (4s)"; }
  else if (sec < 11) { r = 20; lbl = "HOLD (7s)"; }
  else { r = 20 - (int)(((millis() % 19000) - 11000) * 14 / 8000); lbl = "EXHALE (8s)"; }
  u8g2.drawCircle(34, 38, constrain(r, 4, 20)); u8g2.drawDisc(34, 38, constrain(r - 4, 1, 16));
  u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(62, 41, lbl);
}

void initSavers() {
  for (int i = 0; i < 16; i++) matrixY[i] = random(-60, 0);
  for (int i = 0; i < 20; i++) { starX[i] = random(-64, 64); starY[i] = random(-32, 32); starZ[i] = random(1, 64); }
}

void drawScreensaver() {
  if (saverMode == 0) {
    u8g2.setFont(u8g2_font_5x7_tf);
    for (int i = 0; i < 16; i++) { matrixY[i] += 3; if (matrixY[i] > 76) matrixY[i] = random(-20, 0); drawMiniHeart(i * 8 + 4, matrixY[i]); }
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

void drawWeather() {
  drawHeader("WEATHER", "1x:Sync");
  if (WiFi.status() != WL_CONNECTED) { u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(8, 38, "Connect Wi-Fi first"); return; }
  if (!weatherLoaded) { u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(14, 38, "Syncing weather..."); return; }
  float t = 0, w = 0; int h = 0;
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(10)) == pdTRUE) { t = weatherTemp; h = weatherHumidity; w = weatherWind; xSemaphoreGive(netMutex); }
  char tb[12]; sprintf(tb, "%.1fC", t);
  u8g2.setFont(u8g2_font_logisoso18_tf); u8g2.drawStr(24, 38, tb);
  char sb[28]; sprintf(sb, "RH:%d%%   Wind:%.0fkm/h", h, w);
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
  char b[10], t[16]; sprintf(b, "%02d:%02d", (int)((pomoRemainingMs / 1000) / 60), (int)((pomoRemainingMs / 1000) % 60));
  sprintf(t, "%s %dM", pomoBreak ? "BREAK" : "FOCUS", (int)(pomoMaxMs / 60000UL));
  drawHeader(t, pomoRunning ? "RUN" : "PAUSE");
  u8g2.setFont(u8g2_font_logisoso22_tn); u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 43, b);
  int bw = pomoMaxMs > 0 ? (int)((116UL * pomoRemainingMs) / pomoMaxMs) : 0;
  drawSafeRFrame(4, 51, 120, 9, 4); if (bw > 2) drawSafeRBox(6, 53, min(116, bw), 5, 2);
}

void drawMenu() {
  drawHeader("RIDDHI LOVE", "2x:Next");
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
  if (waves >= 4) { currentScreen = SCR_HOME; currentMood = MOOD_NORMAL; showToast("Home", 800); return; }

  if (isHold) {
    if (isGameScreen(currentScreen)) {
      if (currentScreen == SCR_HELI && heliState == G_PLAY) return;
      currentScreen = SCR_HOME; currentMood = MOOD_NORMAL;
      dinoState = G_READY; heliState = G_READY; racerState = G_READY; reactStage = 0;
      showToast("GG!", 1000);
    } else if (currentScreen == SCR_HOME) {
      currentMood = MOOD_LOVE; tempMoodUntil = millis() + 2800; showToast("Love You! <3", 1800);
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
        SCR_HOME, SCR_COUPLE, SCR_LOVEBOX, SCR_CLOCK, SCR_WEATHER, SCR_POMODORO,
        SCR_BREATH, SCR_SAVER, SCR_DINO, SCR_HELI, SCR_RACER, SCR_REACT, SCR_NOTE, SCR_AI, SCR_WIFI_INFO
      };
      currentScreen = mapM[menuIndex];
      dinoState = G_READY; heliState = G_READY; racerState = G_READY; reactStage = 0;
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
    else if (currentScreen == SCR_LOVEBOX && loveUnread) { loveUnread = false; }
    else if (currentScreen == SCR_SAVER) saverMode = (saverMode + 1) % 3;
    else if (currentScreen == SCR_WEATHER) { reqWeatherNow = true; showToast("Syncing...", 1000); }
    else if (currentScreen == SCR_POMODORO) { pomoRunning = !pomoRunning; pomoLastTick = millis(); }
    else if (currentScreen == SCR_MENU) menuIndex = (menuIndex + 1) % MENU_COUNT;
    else if (currentScreen == SCR_HOME) {
      currentMood = MOOD_LOVE; tempMoodUntil = millis() + 2200;
      const char* g[] = {"Love You! <3", "Miss You! <3", "Cutie! <3"};
      showToast(g[random(0, 3)], 1500);
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

// ================= LOVERS EDITION 5-PAGE WEB UI =================
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Riddhi Love">
<title>Riddhi Lovers Edition</title><style>
:root{--bg:#120814;--card:rgba(42,16,42,.82);--btn:#381639;--rose:#fb7185;--pink:#f472b6;--gold:#fcd34d;--lav:#c084fc;}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:radial-gradient(circle at top,#4c1130 0%,var(--bg) 60%);color:#fff1f2;margin:0;padding:16px 14px 86px;min-height:100vh;}
.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px;padding:0 4px;}
.brand{display:flex;align-items:center;gap:8px;}.heart{color:var(--rose);font-size:18px;animation:beat 1.4s infinite;}
@keyframes beat{0%,100%{transform:scale(1)}50%{transform:scale(1.22)}}
h2{margin:0;font-size:20px;background:linear-gradient(90deg,#fff1f2,#fda4af,#fcd34d);-webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.pill{font-size:11px;font-weight:700;padding:4px 11px;border-radius:20px;background:rgba(251,113,133,.18);color:#fda4af;border:1px solid rgba(251,113,133,.4);}
.page{display:none;animation:fade .2s ease;}.page.active{display:block;}
@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.card{background:var(--card);border:1px solid rgba(253,164,175,.16);border-radius:22px;padding:15px;margin-bottom:12px;box-shadow:0 10px 28px rgba(0,0,0,.35);}
.hero-card{background:linear-gradient(135deg,rgba(159,18,57,.55),rgba(76,17,48,.85));border-color:rgba(251,113,133,.45);text-align:center;padding:18px 14px;}
.days-num{font-size:38px;font-weight:800;line-height:1.1;background:linear-gradient(90deg,#fff,#fda4af,#fcd34d);-webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.days-sub{font-size:11px;letter-spacing:1.6px;text-transform:uppercase;color:#fecdd3;margin-top:4px;}
.t{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:1.2px;color:#fda4af;margin-bottom:10px;text-align:left;}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:8px;}.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;}
.row{display:flex;gap:7px;margin-bottom:8px;align-items:center;}
button{background:var(--btn);color:#fff1f2;border:1px solid rgba(253,164,175,.15);border-radius:14px;padding:12px 8px;font-size:13px;font-weight:600;cursor:pointer;}
button:active{transform:scale(.95);filter:brightness(1.25);}
.love-btn{background:linear-gradient(135deg,#e11d48,#f472b6);color:#fff;border:none;box-shadow:0 4px 14px rgba(225,29,72,.35);}
.gold-btn{background:linear-gradient(135deg,#d97706,#fcd34d);color:#271102;border:none;}
.lav-btn{background:linear-gradient(135deg,#7c3aed,#ec4899);color:#fff;border:none;}
.red{background:rgba(244,63,94,.18);border-color:var(--rose);color:#fda4af;}
input,select{width:100%;padding:11px;border-radius:12px;border:1px solid rgba(253,164,175,.2);background:rgba(18,8,20,.75);color:#fff;margin-bottom:8px;font-size:13px;}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px;}
.chip{background:rgba(244,114,182,.16);border:1px solid rgba(244,114,182,.4);color:#fbcfe8;padding:6px 11px;border-radius:20px;font-size:11px;font-weight:600;}
#aiLog,#mailBox{text-align:left;background:rgba(18,8,20,.65);border-radius:14px;padding:12px;min-height:48px;font-size:13px;color:#ffe4e6;margin-bottom:10px;line-height:1.45;border:1px solid rgba(253,164,175,.12);}
.sub{font-size:11px;color:#fda4af;margin-top:6px;text-align:left;opacity:.85;}
.sbar{width:100%;height:7px;background:#1f0a21;border-radius:6px;overflow:hidden;margin-top:8px;}
.sfill{height:100%;background:linear-gradient(90deg,#fb7185,#fcd34d);transition:width .25s;}
.nav{position:fixed;bottom:0;left:0;right:0;background:rgba(18,8,20,.95);backdrop-filter:blur(14px);border-top:1px solid rgba(253,164,175,.14);display:flex;justify-content:space-around;padding:8px 4px 12px;z-index:99;}
.nb{background:none;border:none;color:#9d6b84;display:flex;flex-direction:column;align-items:center;gap:3px;font-size:10px;padding:4px 11px;border-radius:12px;}
.nb.active{color:#fb7185;background:rgba(251,113,133,.15);}
.nb span{font-size:16px;}
#toast{position:fixed;top:14px;left:50%;transform:translateX(-50%);background:linear-gradient(90deg,#fb7185,#f472b6);color:#fff;padding:7px 16px;border-radius:20px;font-size:12px;font-weight:700;display:none;z-index:100;box-shadow:0 4px 14px rgba(0,0,0,.4);}
</style></head><body>

<div id="toast">Sent!</div>
<div class="top"><div class="brand"><span class="heart">&#10084;</span><h2 id="cTitle">You &amp; Me</h2></div><span class="pill">Riddhi Love</span></div>

<!-- PAGE 1: US (HOME) -->
<div id="p-us" class="page active">
  <div class="card hero-card" onclick="cmd('/scr?s=2','Showing Counter!')">
    <div id="dNum" class="days-num">1</div>
    <div class="days-sub">Days Together &bull; Tap to Show on OLED</div>
  </div>
  <div class="card">
    <div class="t">Send Instant Love Ping</div>
    <div class="g2">
      <button class="love-btn" onclick="cmd('/love?k=0','Miss You Sent!')">Miss You &#10084;</button>
      <button class="love-btn" onclick="cmd('/love?k=1','Kiss Sent!')">Send Kiss &#128536;</button>
      <button class="gold-btn" onclick="cmd('/love?k=2','Big Hug!')">Big Hug &#129303;</button>
      <button onclick="cmd('/love?k=3','Sweet Dreams')">Goodnight &#127769;</button>
    </div>
  </div>
  <div class="card">
    <div class="t">Pin a Sweet Note to OLED</div>
    <div class="row" style="margin:0">
      <input id="noteTxt" style="margin:0;flex:1" maxlength="60" placeholder="Write something sweet...">
      <button class="love-btn" style="width:74px" onclick="sendNote()">Send</button>
    </div>
  </div>
  <div class="card">
    <div class="t">Riddhi Moods</div>
    <div class="g3">
      <button onclick="cmd('/mood?m=2','In Love')">Love</button>
      <button onclick="cmd('/mood?m=1','Happy')">Happy</button>
      <button onclick="cmd('/mood?m=0','Chill')">Chill</button>
      <button onclick="cmd('/mood?m=5','Surprised')">Woah</button>
      <button onclick="cmd('/mood?m=3','Pouty')">Pout</button>
      <button onclick="cmd('/mood?m=6','Sleepy')">Sleep</button>
    </div>
  </div>
</div>

<!-- PAGE 2: LOVE-BOX MAIL -->
<div id="p-mail" class="page">
  <div class="card">
    <div class="t">Latest Secret Love-Box Mail</div>
    <div id="mailBox">Loading...</div>
    <button class="love-btn" style="width:100%;margin-bottom:8px" onclick="cmd('/scr?s=3','Opened Mail!')">Show Mail on OLED</button>
  </div>
  <div class="card">
    <div class="t">Send Remote Love Note (via ntfy.sh)</div>
    <input id="remoteMsg" maxlength="60" placeholder="Message to send to your secret topic...">
    <button class="gold-btn" style="width:100%" onclick="sendRemote()">Send Love Envelope &#9993;</button>
    <div class="sub">Works from anywhere in the world when both of you share the same secret topic below.</div>
  </div>
  <div class="card">
    <div class="t">Secret Couple Topic</div>
    <div class="row" style="margin:0">
      <input id="ntfyIn" style="margin:0;flex:1" placeholder="e.g. our-lovebox-2026">
      <button class="love-btn" style="width:70px" onclick="saveNtfy()">Save</button>
    </div>
  </div>
</div>

<!-- PAGE 3: AI -->
<div id="p-ai" class="page">
  <div class="card">
    <div class="t">Cupid AI &amp; Custom Face Designer</div>
    <div id="aiLog">Ask me to invent a cute face, write a sweet note, or suggest a date idea!</div>
    <div class="chips">
      <button class="chip" onclick="quickAI('Draw a blushing heart-eyes face')">Heart Eyes</button>
      <button class="chip" onclick="quickAI('Draw a cute cat face with whiskers')">Cat Face</button>
      <button class="chip" onclick="quickAI('Write a tiny romantic compliment')">Compliment</button>
      <button class="chip" onclick="quickAI('Give us a cute movie night idea')">Date Idea</button>
    </div>
    <div class="row">
      <input id="aiP" style="margin:0;flex:1" placeholder="Ask Riddhi..." onkeydown="if(event.key==='Enter')askAI()">
      <button class="lav-btn" style="width:66px" onclick="askAI()">Ask</button>
    </div>
    <div class="g2">
      <button id="ttsBtn" style="font-size:12px;padding:9px" onclick="toggleTTS()">Voice: OFF</button>
      <button style="font-size:12px;padding:9px" onclick="toggleKB()">API Key &amp; Model</button>
    </div>
    <div id="kb" style="display:none;margin-top:10px;border-top:1px solid rgba(255,255,255,.1);padding-top:10px">
      <input id="ak" type="password" placeholder="Paste Google AI Studio API Key">
      <select id="am"><option value="gemini-3.5-flash-lite">gemini-3.5-flash-lite (Fastest)</option><option value="gemini-3.8-flash">gemini-3.8-flash (Smarter)</option></select>
      <button class="love-btn" style="width:100%" onclick="saveK()">Save Key</button>
    </div>
  </div>
</div>

<!-- PAGE 4: APPS & GAMES -->
<div id="p-apps" class="page">
  <div class="card">
    <div class="t">Desk &amp; Mood Apps</div>
    <div class="g3">
      <button onclick="cmd('/scr?s=0','Riddhi')">Face</button>
      <button onclick="cmd('/scr?s=4','Clock')">Clock</button>
      <button onclick="cmd('/scr?s=5','Weather')">Weather</button>
      <button onclick="cmd('/scr?s=6','Pomo')">Pomodoro</button>
      <button onclick="cmd('/scr?s=7','Zen')">4-7-8 Zen</button>
      <button onclick="cmd('/night','Night Dim')">Night Dim</button>
    </div>
  </div>
  <div class="card">
    <div class="t">Ambient Screensavers</div>
    <div class="g3">
      <button class="love-btn" onclick="cmd('/saver?m=0','Heart Rain')">Heart Rain</button>
      <button onclick="cmd('/saver?m=1','3D Cube')">3D Cube</button>
      <button onclick="cmd('/saver?m=2','Starfield')">Starfield</button>
    </div>
  </div>
  <div class="card">
    <div class="t">IR Mini-Games</div>
    <div class="g2">
      <button onclick="cmd('/scr?s=9','Dino!')">Dino Jump</button>
      <button onclick="cmd('/scr?s=10','Heli!')">Heli Cave</button>
      <button onclick="cmd('/scr?s=11','Racer!')">Micro Racer</button>
      <button onclick="cmd('/scr?s=12','React!')">Reaction Test</button>
    </div>
  </div>
</div>

<!-- PAGE 5: SYSTEM -->
<div id="p-sys" class="page">
  <div class="card">
    <div class="t">Couple Profile</div>
    <input id="cNamesIn" maxlength="16" placeholder="Couple Names (e.g. Venu &amp; Love)">
    <input id="annivDate" type="date">
    <button class="love-btn" style="width:100%" onclick="saveCouple()">Save Names &amp; Anniversary</button>
  </div>
  <div class="card">
    <div class="t">1-Tap GitHub Self-Update</div>
    <input id="ghRepo" placeholder="Public repo (e.g. yourname/Riddhi)">
    <div class="g2">
      <button onclick="saveGh()">Save Repo</button>
      <button class="lav-btn" onclick="pullGh()">Pull Latest Update</button>
    </div>
  </div>
  <div class="card">
    <div class="t">Manual Web OTA (.bin Upload)</div>
    <input type="file" id="otaFile" accept=".bin">
    <button class="gold-btn" style="width:100%" onclick="uploadOTA()">Flash .bin Firmware</button>
    <div id="otaBar" class="sbar" style="display:none"><div id="otaFill" class="sfill" style="width:0%"></div></div>
    <div id="otaSt" class="sub"></div>
  </div>
  <div class="card">
    <div class="t">Wi-Fi &amp; Display</div>
    <button style="width:100%;margin-bottom:8px" onclick="scanWifi()">Scan Nearby 2.4GHz Wi-Fi</button>
    <div id="nets" class="chips"></div>
    <input id="ssid" placeholder="Wi-Fi Name (SSID)"><input id="pass" type="password" placeholder="Wi-Fi Password">
    <div class="g3">
      <button class="love-btn" onclick="saveW()">Save Wi-Fi</button>
      <button onclick="cmd('/flip','Flipped!')">Flip OLED</button>
      <button class="red" onclick="cmd('/resetwifi','Resetting')">Reset</button>
    </div>
    <div id="wSt" class="sub"></div>
  </div>
</div>

<nav class="nav">
  <button class="nb active" onclick="go('us',this)"><span>&#10084;</span>Us</button>
  <button class="nb" onclick="go('mail',this)"><span>&#9993;</span>Mail</button>
  <button class="nb" onclick="go('ai',this)"><span>&#10024;</span>AI</button>
  <button class="nb" onclick="go('apps',this)"><span>&#9783;</span>Apps</button>
  <button class="nb" onclick="go('sys',this)"><span>&#9881;</span>System</button>
</nav>

<script>
let ttsOn=false, aiHist=[];
const sk=localStorage.getItem('rk')||''; document.getElementById('ak').value=sk;
if(!sk) document.getElementById('kb').style.display='block';

function go(id,el){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.nb').forEach(b=>b.classList.remove('active'));
  document.getElementById('p-'+id).classList.add('active'); el.classList.add('active');
  if(id==='us'||id==='mail'||id==='sys') syncStatus();
}
function toast(m){const t=document.getElementById('toast');t.innerText=m;t.style.display='block';setTimeout(()=>t.style.display='none',1300);}
function cmd(u,msg){fetch(u);if(msg)toast(msg);}
function sendNote(){const v=document.getElementById('noteTxt').value.trim();if(!v)return;cmd('/note?t='+encodeURIComponent(v),'Note Pinned!');document.getElementById('noteTxt').value='';}
function saveNtfy(){cmd('/cfg?ntfy='+encodeURIComponent(document.getElementById('ntfyIn').value.trim()),'Topic Saved!');}
function saveGh(){cmd('/cfg?gh='+encodeURIComponent(document.getElementById('ghRepo').value.trim()),'Repo Saved!');}
function pullGh(){if(confirm('Pull latest Riddhi_OS.ino.bin from GitHub and reboot?')) cmd('/ghota','Updating...');}
function toggleKB(){const b=document.getElementById('kb');b.style.display=b.style.display==='none'?'block':'none';}
function toggleTTS(){ttsOn=!ttsOn;document.getElementById('ttsBtn').innerText='Voice: '+(ttsOn?'ON':'OFF');}
function saveK(){localStorage.setItem('rk',document.getElementById('ak').value.trim());localStorage.setItem('rm',document.getElementById('am').value);document.getElementById('kb').style.display='none';toast('Key Saved!');}
function quickAI(txt){document.getElementById('aiP').value=txt;askAI();}

function saveCouple(){
  const nm=document.getElementById('cNamesIn').value.trim()||'You & Me';
  const dt=document.getElementById('annivDate').value;
  let days=parseInt(document.getElementById('dNum').innerText,10)||1;
  if(dt){ days=Math.max(1, Math.floor((Date.now()-new Date(dt).getTime())/86400000)+1); }
  cmd('/cfg?cn='+encodeURIComponent(nm)+'&days='+days,'Saved!');
  setTimeout(syncStatus,300);
}

async function sendRemote(){
  const tp=document.getElementById('ntfyIn').value.trim();
  const msg=document.getElementById('remoteMsg').value.trim();
  if(!tp||!msg){toast('Enter topic & message!');return;}
  try{
    await fetch('https://ntfy.sh/'+encodeURIComponent(tp),{method:'POST',body:msg});
    document.getElementById('remoteMsg').value=''; toast('Love Envelope Sent!');
  }catch(e){toast('Send failed');}
}

async function syncStatus(){
  try{
    const r=await fetch('/status'); if(!r.ok) return;
    const s=await r.json();
    document.getElementById('cTitle').innerText=s.cn;
    document.getElementById('dNum').innerText=s.days;
    document.getElementById('mailBox').innerText=s.mail;
    if(!document.getElementById('cNamesIn').value) document.getElementById('cNamesIn').value=s.cn;
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
  const sys='You are Riddhi, a sweet, romantic ESP32 desk robot for a couple (128x64 OLED). Return ONLY JSON: {"mood":"NORMAL|HAPPY|LOVE|ANGRY|DIZZY|SURPRISED|SLEEP","action":"NONE|CUSTOM_FACE|COUPLE|WEATHER|CLOCK|POMO_START","arg":0,"reply":"sweet ASCII <52 chars","shapes":[]}. For custom faces use action="CUSTOM_FACE" with shapes (rbox,disc,circle,line,arc).';
  const reqContents=[...aiHist, {role:'user',parts:[{text:p}]}];
  try{
    const r=await fetch('https://generativelanguage.googleapis.com/v1beta/models/'+m+':generateContent?key='+encodeURIComponent(k),{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({system_instruction:{parts:[{text:sys}]},generationConfig:{responseMimeType:'application/json'},contents:reqContents})});
    const d=await r.json();
    if(!r.ok) throw new Error((d.error&&d.error.message)||('HTTP '+r.status));
    if(!d.candidates||!d.candidates[0]||!d.candidates[0].content) throw new Error('Empty AI response');
    const raw=d.candidates[0].content.parts[0].text.replace(/^```json|```$/gi,'').trim();
    const j=JSON.parse(raw);
    aiHist.push({role:'user',parts:[{text:p}]}, {role:'model',parts:[{text:j.reply||'OK'}]});
    if(aiHist.length>6) aiHist=aiHist.slice(-6);
    const mm={NORMAL:0,HAPPY:1,LOVE:2,ANGRY:3,DIZZY:4,SURPRISED:5,SLEEP:6}; lg.innerText=j.reply||'Done!';
    const b=new URLSearchParams(); b.set('m',mm[(j.mood||'LOVE').toUpperCase()]??2); b.set('act',(j.action||'NONE').toUpperCase()); b.set('arg',j.arg||0); b.set('t',(j.reply||'').replace(/[^\x20-\x7E]/g,'').slice(0,60));
    const sh=ser(j.shapes); if(sh) b.set('shapes',sh);
    await fetch('/ai',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b.toString()});
    if(ttsOn&&'speechSynthesis'in window){window.speechSynthesis.cancel();window.speechSynthesis.speak(new SpeechSynthesisUtterance(j.reply||''));}
  }catch(e){lg.innerText='AI Error: '+e.message; fetch('/mood?m=4');}
}
</script></body></html>
)rawliteral";

void setupWebServer() {
  server.on("/", []() { server.send_P(200, "text/html", HTML_PAGE); });

  server.on("/status", []() {
    String mailCopy = "";
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(50)) == pdTRUE) { mailCopy = loveMsg; xSemaphoreGive(netMutex); }
    String j = "{\"cn\":\"" + jsonEscape(coupleNames) + "\",\"days\":" + String(daysTogether) +
               ",\"mail\":\"" + jsonEscape(mailCopy) + "\",\"gh\":\"" + jsonEscape(ghRepo) +
               "\",\"ntfy\":\"" + jsonEscape(ntfyTopic) + "\"}";
    server.send(200, "application/json", j);
  });

  server.on("/cfg", []() {
    if (server.hasArg("cn")) coupleNames = server.arg("cn").substring(0, 16);
    if (server.hasArg("days")) daysTogether = max(1, server.arg("days").toInt());
    if (server.hasArg("gh")) ghRepo = server.arg("gh");
    if (server.hasArg("ntfy")) {
      if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) { ntfyTopic = server.arg("ntfy"); lastNtfyId = ""; xSemaphoreGive(netMutex); }
    }
    saveStateNow(); server.send(200, "text/plain", "OK");
  });

  server.on("/love", []() {
    int k = server.arg("k").toInt();
    currentScreen = SCR_HOME; lastActivity = millis(); tempMoodUntil = millis() + 4000;
    if (k == 0) { currentMood = MOOD_LOVE; showToast("Miss You! <3", 3800); }
    else if (k == 1) { currentMood = MOOD_LOVE; showToast("Mwah! <3", 3800); }
    else if (k == 2) { currentMood = MOOD_HAPPY; showToast("Big Hug! <3", 3800); }
    else { currentMood = MOOD_SLEEP; tempMoodUntil = 0; showToast("Sweet Dreams", 3500); }
    server.send(200, "text/plain", "OK");
  });

  server.on("/ghota", []() { server.send(200, "text/plain", "OK"); triggerGhOta = true; });
  server.on("/night", []() {
    manualNight = !manualNight; nightMode = manualNight; u8g2.setContrast(nightMode ? 5 : 255);
    if (nightMode) currentScreen = SCR_CLOCK; server.send(200, "text/plain", "OK");
  });

  // Manual Web OTA
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(400); if (!Update.hasError()) ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload(); static unsigned long lastOtaDraw = 0;
    if (upload.status == UPLOAD_FILE_START) { netTaskPaused = true; if (stateDirty) saveStateNow(); drawBootProgress(5, "Starting OTA..."); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      Update.write(upload.buf, upload.currentSize);
      if (millis() - lastOtaDraw > 180) { lastOtaDraw = millis(); char lbl[24]; sprintf(lbl, "Flashing %d KB...", upload.totalSize / 1024); drawBootProgress(constrain((upload.totalSize / 1024) * 100 / 1150, 8, 96), lbl); }
    } else if (upload.status == UPLOAD_FILE_END) { drawBootProgress(Update.end(true) ? 100 : 0, Update.hasError() ? "OTA Failed!" : "Rebooting..."); if (Update.hasError()) netTaskPaused = false; }
  });

  server.on("/note", []() { if (server.hasArg("t")) { customNote = server.arg("t"); currentScreen = SCR_NOTE; lastActivity = millis(); } server.send(200, "text/plain", "OK"); });
  server.on("/mood", []() { currentScreen = SCR_HOME; currentMood = (Mood)server.arg("m").toInt(); tempMoodUntil = 0; lastActivity = millis(); server.send(200, "text/plain", "OK"); });
  server.on("/scr", []() { currentScreen = (ScreenMode)server.arg("s").toInt(); dinoState = G_READY; heliState = G_READY; racerState = G_READY; reactStage = 0; lastActivity = millis(); server.send(200, "text/plain", "OK"); });
  server.on("/saver", []() { saverMode = server.arg("m").toInt() % 3; currentScreen = SCR_SAVER; lastActivity = millis(); server.send(200, "text/plain", "OK"); });
  server.on("/flip", []() { flippedDisplay = !flippedDisplay; u8g2.setFlipMode(flippedDisplay ? 1 : 0); server.send(200, "text/plain", "OK"); });
  server.on("/aithink", []() { currentScreen = SCR_HOME; currentMood = MOOD_THINKING; tempMoodUntil = millis() + 15000; showToast("Thinking...", 15000); server.send(200, "text/plain", "OK"); });

  server.on("/ai", HTTP_ANY, []() {
    int m = constrain(server.arg("m").toInt(), 0, 6);
    String act = server.arg("act"); act.toUpperCase();
    int argVal = server.arg("arg").toInt();
    String text = server.arg("t"), shapes = server.arg("shapes");
    aiReplyMood = (Mood)m; aiReplyText = text; currentScreen = SCR_HOME; currentMood = aiReplyMood; tempMoodUntil = 0; lastActivity = millis();
    pendingScreen = SCR_AI; pendingScreenAt = millis() + 1400;
    if (act == "CUSTOM_FACE" || shapes.length() > 0) { parseCustomShapes(shapes); if (customShapeCount > 0) { currentMood = MOOD_CUSTOM; pendingScreenAt = 0; showToast(text.substring(0, 20), 2400); } }
    else if (act == "COUPLE") pendingScreen = SCR_COUPLE;
    else if (act == "POMO_START") { int mn = constrain(argVal > 0 ? argVal : 25, 1, 99); pomoBreak = false; pomoMaxMs = mn * 60000UL; pomoRemainingMs = pomoMaxMs; pomoRunning = true; pomoLastTick = millis(); pendingScreen = SCR_POMODORO; }
    else if (act == "WEATHER") { reqWeatherNow = true; pendingScreen = SCR_WEATHER; }
    else if (act == "CLOCK") pendingScreen = SCR_CLOCK;
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
  netMutex = xSemaphoreCreateMutex();
  loadState(); initSavers(); playBootAnimation();

  WiFi.persistent(true); WiFi.mode(WIFI_STA); WiFi.begin();
  for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++) { drawBootProgress((i * 100) / 24, "Connecting Wi-Fi..."); delay(250); }

  if (WiFi.status() == WL_CONNECTED) {
    isAPMode = false; activeIP = WiFi.localIP().toString();
    MDNS.begin("riddhi");
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  } else {
    isAPMode = true; WiFi.disconnect(false); WiFi.setAutoReconnect(false); delay(150);
    WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID, AP_PASS, 1); activeIP = WiFi.softAPIP().toString();
  }

  setupWebServer();
  xTaskCreatePinnedToCore(networkTaskCore0, "NetCore0", 6144, NULL, 1, NULL, 0);

  u8g2.clearBuffer(); drawWifiInfo(); u8g2.sendBuffer(); delay(2500);
  lastActivity = millis(); lastNvsSave = millis();
  showToast(coupleNames + " <3", 2000);
}

void loop() {
  server.handleClient();
  updateIRSensor();
  updatePomodoro();
  checkMidnightAndNightstand();

  static unsigned long lastFrame = 0;
  unsigned long now = millis();

  if (triggerGhOta) { triggerGhOta = false; runGitHubOTA(); }

  if (newLoveMailFlag) {
    newLoveMailFlag = false; loveUnread = true; currentScreen = SCR_LOVEBOX; lastActivity = now;
  }

  if (pendingScreenAt > 0 && now >= pendingScreenAt) {
    pendingScreenAt = 0; currentScreen = pendingScreen;
  }

  if (stateDirty && (now - lastNvsSave > NVS_SAVE_INTERVAL_MS)) saveStateNow();

  if (now - lastFrame >= 33) {
    lastFrame = now;
    u8g2.clearBuffer();
    switch (currentScreen) {
      case SCR_HOME:      drawRiddhiFace(); break;
      case SCR_MENU:      drawMenu(); break;
      case SCR_COUPLE:    drawCoupleScreen(); break;
      case SCR_LOVEBOX:   drawLoveBox(); break;
      case SCR_CLOCK:     drawClock(); break;
      case SCR_WEATHER:   drawWeather(); break;
      case SCR_POMODORO:  drawPomodoro(); break;
      case SCR_BREATH:    drawBreathing(); break;
      case SCR_SAVER:     drawScreensaver(); break;
      case SCR_DINO:      drawDino(); break;
      case SCR_HELI:      drawHeli(); break;
      case SCR_RACER:     drawRacer(); break;
      case SCR_REACT:     drawReact(); break;
      case SCR_NOTE:      drawHeader("SWEET NOTE", "<3"); u8g2.setFont(u8g2_font_6x10_tf); drawWrappedText(customNote, 26, 12, 3, 20); break;
      case SCR_AI:        drawHeader("CUPID AI", "1x:Face"); u8g2.setFont(u8g2_font_6x10_tf); drawWrappedText(aiReplyText, 26, 11, 3, 20); break;
      case SCR_WIFI_INFO: drawWifiInfo(); break;
    }
    if (irWaveCount > 0 && !isGameScreen(currentScreen)) {
      for (int i = 0; i < irWaveCount && i < 4; i++) u8g2.drawDisc(122 - (i * 7), 5, 2);
    }
    u8g2.sendBuffer();
  }
}
