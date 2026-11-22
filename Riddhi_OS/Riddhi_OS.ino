// =====================================================================
//  RIDDHI OS v7.0  -  ESP32 desk companion
//  Polished build: cleaner code, OLED auto-off, night dimming,
//  screen transitions, richer Tasks/Weather/Clock screens,
//  smarter Pomodoro, safer Wi-Fi reset, status panel in Web UI.
// =====================================================================
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

// ================= HARDWARE CONFIG (ESP32 DEVKIT V1) =================
// Uncomment the ONE driver that matches your OLED:
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);   // 1.3" OLED
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE); // 0.96" OLED

#define FW_VER "7.0"
#define IR_PIN 25
#define SDA_PIN 21
#define SCL_PIN 22

const char* AP_SSID = "Riddhi-OS";
const char* AP_PASS = "12345678";          // change me if you use AP mode a lot

// Madanapalle, AP coordinates for Open-Meteo
const char* WEATHER_LAT = "13.5503";
const char* WEATHER_LON = "78.5029";
#define GMT_OFFSET_SEC      19800            // IST (UTC +5:30)
#define DROWSY_TIMEOUT_MS   35000UL          // droopy eyes at 35s idle
#define SLEEP_TIMEOUT_MS    50000UL          // sleep / screensaver at 50s idle
#define DISPLAY_OFF_MS      600000UL         // OLED fully off after 10 min idle (saves the panel)
#define MENU_IDLE_MS        20000UL          // menu returns to face
#define SCREEN_IDLE_MS      45000UL          // other transient screens return to face
#define WEATHER_INTERVAL_MS 900000UL         // 15 mins
#define CARE_DECAY_MS       1800000UL        // -5% Care every 30 mins
// =====================================================================

WebServer server(80);
Preferences prefs;
SemaphoreHandle_t netMutex = NULL;

enum ScreenMode {
  SCR_FACE = 0, SCR_MENU, SCR_TASKS, SCR_CLOCK, SCR_WEATHER,
  SCR_POMO, SCR_COUNTER, SCR_DICE, SCR_BREATH, SCR_BANNER,
  SCR_PC, SCR_DOODLE, SCR_NUDGE,
  SCR_SPACE, SCR_FLAPPY, SCR_PONG, SCR_CATCH,
  SCR_REACT, SCR_DINO, SCR_RHYTHM,
  SCR_AI, SCR_WIFI
};

enum Mood {
  MOOD_CHILL = 0, MOOD_HYPED, MOOD_ZEN, MOOD_GRUMPY,
  MOOD_SHOCKED, MOOD_SUSPICIOUS, MOOD_SMUG, MOOD_DIZZY,
  MOOD_LOVE, MOOD_SICK, MOOD_DROWSY, MOOD_SLEEP,
  MOOD_THINK, MOOD_CUSTOM
};

ScreenMode currentScreen = SCR_FACE;
Mood currentMood = MOOD_CHILL;
bool flippedDisplay = false;
bool dimMode = false;
bool weatherFxOn = true;
bool autoDim = false;                 // dim OLED 23:00-06:00
uint8_t brightness = 255;
int appliedContrast = -1;
bool displayOff = false;

// Quick IR Menu (20 items)
const char* menuItems[] = {
  "Riddhi Face", "Sticky Tasks", "Digital Clock", "Live Weather",
  "Focus Timer", "Tally Counter", "Dice & Oracle", "4-7-8 Breath",
  "Desk Banner", "PC & Media", "Pixel Doodle",
  "Game: Space Def", "Game: Flappy Orb", "Game: Solo Pong",
  "Game: Orb Catch", "Game: Reflex ms", "Game: Cyber Dino",
  "Game: Beat Tap", "AI Console", "Wi-Fi & Link"
};
const int MENU_COUNT = 20;
int menuIndex = 0;

// Custom AI Face Primitives
struct CustomShape { char type; int p[7]; };
CustomShape customShapes[24];
int customShapeCount = 0;

// Eye Animation & Progressive Sleep
float eyeX = 0, eyeTX = 0, eyeY = 0, eyeTY = 0;
float eyeLH = 24, eyeTLH = 24, eyeRH = 24, eyeTRH = 24;
unsigned long lastAct = 0, nextBlink = 2200, blinkEnd = 0, nextLook = 2800, tempMoodEnd = 0;
int blinkPhase = 0;
bool wasSleeping = false;

// Ambient Screensavers (0=Zzz, 1=Matrix, 2=DVD, 3=Analog Clock)
int saverMode = 0;
int matY[16];
float dvdX = 20, dvdY = 20, dvdVX = 1.4f, dvdVY = 1.1f;

// Posture & Hydration Nudge Loop
bool nudgeOn = false;
int nudgeMin = 45;
unsigned long lastNudgeMs = 0;
bool nudgeIsWater = true;

// Finger-Draw 64x32 Pixel Canvas (256 bytes, scaled 2x2 to 128x64)
uint8_t doodleBuf[256];
bool hasDoodle = false;

// Live PC Telemetry & Wi-Fi Media Remote
int pcCpu = 0, pcRam = 0, pcBat = 100;
String pcSong = "Run riddhi_pc_bridge.py";
String pcCmdQueue = "";

// Webhook & ntfy.sh Alerts
String ntfyTopic = "";
String lastNtfyId = "";
volatile bool alertPending = false;
String alertTitle = "", alertMsg = "", alertMoodStr = "HYPED";

// Care / Boredom & Sticky Tasks
int careLevel = 90;
unsigned long lastCareDecay = 0;
String tasks[5] = { "Drink water", "Study 45 mins", "Code Riddhi", "Evening walk", "Skincare" };
int taskDoneMask = 0;
int taskIdx = 0;

// Holiday & Time
int autoHoliday = 0;
int manualHoliday = -1;
int currentHour = 12, currentMin = 0, currentSec = 0;

// Persistent Data & High Scores
int tallyCount = 0;
String tallyLabel = "COUNT";
int hiSpace = 0, hiFlappy = 0, hiPong = 0, hiCatch = 0;
int hiDino = 0, hiRhythm = 0, bestReactMs = 999;
String ghRepo = "";
bool stateDirty = false;
unsigned long lastSaveMs = 0;

// Autonomous AI Brain State (Core 0)
String geminiKey = "";
String geminiModel = "gemini-3.8-flash";
bool autoBrainOn = false;
int brainIntervalMin = 20;
volatile bool triggerBrainNow = false;
volatile bool brainHasUpdate = false;
String brainMoodStr = "", brainToastStr = "", brainBannerT = "", brainBannerS = "", brainScrStr = "";
String lastBrainLog = "Autonomous brain idle.";

// Desk Banner, AI, Pomodoro, Dice
String bannerTop = "FOCUS MODE";
String bannerSub = "Do Not Disturb";
String aiReply = "Ready! Speak or type any multi-step command.";
ScreenMode pendingScreen = SCR_FACE;
unsigned long pendingAt = 0;
String toastMsg = "";
unsigned long toastEnd = 0;
bool pomoRun = false, pomoBreak = false;
const uint8_t POMO_PRESETS[4] = { 15, 25, 45, 50 };
int pomoPreset = 1, pomoSessions = 0;
unsigned long pomoTotal = 25UL * 60000UL, pomoLeft = 25UL * 60000UL, pomoTick = 0;
int diceVal = 6;
bool oracleYes = true;
unsigned long diceRollUntil = 0;
unsigned long wifiResetArm = 0;

// Background Weather (Core 0)
volatile bool netPaused = false, triggerGhOta = false, fetchWeatherNow = false, weatherReady = false;
float wTemp = 26.0f, wWind = 0;
int wHum = 0, wCode = 0;

// 7 Mini-Games State
enum GState { G_INTRO = 0, G_RUN, G_DEAD };
GState gState = G_INTRO;
bool newBest = false;
unsigned long gDeadAt = 0;
int shipLane = 1, astLane = 0, laserX = -10, laserLane = 1, scoreSpace = 0;
float astX = 132, astSpeed = 1.9f;
float orbY = 32, orbVel = 0, pipeX = 132;
int pipeGap = 26, scoreFlappy = 0;
float ballX = 64, ballY = 32, ballVX = 1.6f, ballVY = 1.2f, padY = 26, padDir = 1.6f;
int scorePong = 0;
int catchSide = 0, dropSide = 0, scoreCatch = 0;
float dropY = 14, dropSpeed = 1.4f;
int reactPhase = 0;
unsigned long reactGoAt = 0;
int reactResultMs = 0;
float dinoY = 48, dinoVy = 0, cacX = 134, cacSpeed = 2.0f;
int scoreDino = 0;
float ringR = 28.0f, ringSpeed = 0.55f;
int scoreRhythm = 0;

// IR Sensor Engine
bool irStable = false, irPrevRaw = false, irHeld = false;
unsigned long irDebMs = 0, irDownAt = 0, irUpAt = 0;
int irWaves = 0;
String activeIP = "192.168.4.1";
bool isAP = false;

// Screen transition
ScreenMode lastDrawnScreen = SCR_FACE;
unsigned long transStart = 0;

// ================= STORAGE =================
void saveNow() {
  prefs.begin("riddhi63", false);
  prefs.putInt("care", careLevel);
  prefs.putBool("wFx", weatherFxOn);
  prefs.putInt("savM", saverMode);
  prefs.putBool("ndgOn", nudgeOn);
  prefs.putInt("ndgM", nudgeMin);
  prefs.putString("ntfy", ntfyTopic);
  prefs.putInt("tMask", taskDoneMask);
  for (int i = 0; i < 5; i++) prefs.putString(("tk" + String(i)).c_str(), tasks[i]);
  prefs.putInt("tCnt", tallyCount);
  prefs.putString("tLbl", tallyLabel);
  prefs.putInt("hiS", hiSpace);
  prefs.putInt("hiF", hiFlappy);
  prefs.putInt("hiP", hiPong);
  prefs.putInt("hiC", hiCatch);
  prefs.putInt("hiD", hiDino);
  prefs.putInt("hiR", hiRhythm);
  prefs.putInt("hiMs", bestReactMs);
  prefs.putString("gh", ghRepo);
  prefs.putString("gKey", geminiKey);
  prefs.putString("gMod", geminiModel);
  prefs.putBool("aBrn", autoBrainOn);
  prefs.putInt("bInt", brainIntervalMin);
  prefs.putInt("bri", brightness);
  prefs.putBool("aDim", autoDim);
  prefs.end();
  stateDirty = false;
  lastSaveMs = millis();
}

void loadAll() {
  prefs.begin("riddhi63", true);
  careLevel    = prefs.getInt("care", 90);
  weatherFxOn  = prefs.getBool("wFx", true);
  saverMode    = prefs.getInt("savM", 0);
  nudgeOn      = prefs.getBool("ndgOn", false);
  nudgeMin     = prefs.getInt("ndgM", 45);
  ntfyTopic    = prefs.getString("ntfy", "");
  taskDoneMask = prefs.getInt("tMask", 0);
  for (int i = 0; i < 5; i++) {
    String s = prefs.getString(("tk" + String(i)).c_str(), "");
    if (s.length() > 0) tasks[i] = s;
  }
  tallyCount       = prefs.getInt("tCnt", 0);
  tallyLabel       = prefs.getString("tLbl", "COUNT");
  hiSpace          = prefs.getInt("hiS", 0);
  hiFlappy         = prefs.getInt("hiF", 0);
  hiPong           = prefs.getInt("hiP", 0);
  hiCatch          = prefs.getInt("hiC", 0);
  hiDino           = prefs.getInt("hiD", 0);
  hiRhythm         = prefs.getInt("hiR", 0);
  bestReactMs      = prefs.getInt("hiMs", 999);
  ghRepo           = prefs.getString("gh", "");
  geminiKey        = prefs.getString("gKey", "");
  geminiModel      = prefs.getString("gMod", "gemini-3.8-flash");
  autoBrainOn      = prefs.getBool("aBrn", false);
  brainIntervalMin = prefs.getInt("bInt", 20);
  brightness       = (uint8_t)constrain(prefs.getInt("bri", 255), 5, 255);
  autoDim          = prefs.getBool("aDim", false);
  prefs.end();
}

// ================= HELPERS =================
String jsonEscape(const String& s) {
  String out = ""; out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c < 0x20) continue;
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

String extractJsonStr(const String& json, const char* key) {
  String pat = String("\"") + key + "\"";
  int idx = json.indexOf(pat);
  if (idx < 0) return "";
  int col = json.indexOf(':', idx + pat.length());
  if (col < 0) return "";
  int q1 = json.indexOf('"', col + 1);
  if (q1 < 0) return "";
  String res = "";
  for (int i = q1 + 1; i < (int)json.length(); i++) {
    char c = json[i];
    if (c == '\\' && i + 1 < (int)json.length()) {
      char nx = json[++i];
      if (nx == 'n') res += ' ';
      else res += nx;
    } else if (c == '"') break;
    else res += c;
  }
  return res;
}

Mood parseMoodName(String m) {
  m.toUpperCase();
  if (m == "HYPED" || m == "HAPPY") return MOOD_HYPED;
  if (m == "ZEN") return MOOD_ZEN;
  if (m == "GRUMPY" || m == "ANGRY") return MOOD_GRUMPY;
  if (m == "SHOCKED" || m == "SURPRISED") return MOOD_SHOCKED;
  if (m == "SUS" || m == "SUSPICIOUS") return MOOD_SUSPICIOUS;
  if (m == "SMUG") return MOOD_SMUG;
  if (m == "DIZZY") return MOOD_DIZZY;
  if (m == "LOVE") return MOOD_LOVE;
  if (m == "SICK") return MOOD_SICK;
  if (m == "DROWSY") return MOOD_DROWSY;
  if (m == "SLEEP") return MOOD_SLEEP;
  return MOOD_CHILL;
}

void toast(const String& s, unsigned long ms = 1800) { toastMsg = s; toastEnd = millis() + ms; }
void boostCare(int amt) { careLevel = constrain(careLevel + amt, 0, 100); stateDirty = true; }

// Any user/web/alert activity: reset idle timers and wake the panel
void wakeUp() {
  lastAct = millis();
  wasSleeping = false;
  if (displayOff) { displayOff = false; u8g2.setPowerSave(0); }
}

void queueScreen(ScreenMode s, unsigned long delayMs = 1500) { pendingScreen = s; pendingAt = millis() + delayMs; }

void applyContrast() {
  bool night = autoDim && (currentHour >= 23 || currentHour < 6);
  int want = (dimMode || night) ? 5 : brightness;
  if (want != appliedContrast) { appliedContrast = want; u8g2.setContrast(want); }
}

void rBox(int x, int y, int w, int h, int r) {
  if (w < 2 || h < 2) return;
  u8g2.drawRBox(x, y, w, h, max(0, min(r, min(w / 2, h / 2))));
}
void rFrame(int x, int y, int w, int h, int r) {
  if (w < 2 || h < 2) return;
  u8g2.drawRFrame(x, y, w, h, max(0, min(r, min(w / 2, h / 2))));
}
void drawMiniHeart(int x, int y) {
  u8g2.drawPixel(x - 1, y); u8g2.drawPixel(x + 1, y);
  u8g2.drawHLine(x - 2, y + 1, 5); u8g2.drawHLine(x - 1, y + 2, 3); u8g2.drawPixel(x, y + 3);
}
void drawSparkle(int cx, int cy, int s) {
  u8g2.drawHLine(cx - s, cy, s * 2 + 1); u8g2.drawVLine(cx, cy - s, s * 2 + 1);
}
void drawCentered(const char* s, int y) {
  u8g2.drawStr((128 - u8g2.getStrWidth(s)) / 2, y, s);
}
void drawTopBar(const char* left, const char* right = "") {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(3, 9, left);
  if (strlen(right) > 0) {
    int rw = u8g2.getStrWidth(right);
    rBox(124 - rw - 4, 1, rw + 6, 10, 3);
    u8g2.setDrawColor(0); u8g2.drawStr(124 - rw - 1, 9, right); u8g2.setDrawColor(1);
  }
  u8g2.drawHLine(0, 12, 128);
}
void drawWrapped(const String& txt, int y0, int lh, int maxL, int maxC) {
  int len = txt.length(), idx = 0, ln = 0;
  while (idx < len && ln < maxL) {
    while (idx < len && txt[idx] == ' ') idx++;
    if (idx >= len) break;
    int end = min(idx + maxC, len);
    if (end < len && txt[end] != ' ') { int sp = txt.lastIndexOf(' ', end); if (sp > idx) end = sp; }
    String line = txt.substring(idx, end); line.trim();
    u8g2.drawStr(3, y0 + ln * lh, line.c_str());
    idx = end; ln++;
  }
}
bool isGame(ScreenMode s) { return (s >= SCR_SPACE && s <= SCR_RHYTHM); }
bool screenIsSticky(ScreenMode s) {
  return s == SCR_FACE || s == SCR_CLOCK || s == SCR_POMO || s == SCR_BANNER || s == SCR_PC ||
         s == SCR_BREATH || s == SCR_NUDGE || s == SCR_DOODLE || isGame(s);
}

void drawProgress(int pct, const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso18_tf);
  u8g2.drawStr((128 - u8g2.getStrWidth("RIDDHI 7.0")) / 2, 29, "RIDDHI 7.0");
  rFrame(16, 38, 96, 8, 4);
  if (pct > 3) rBox(18, 40, (92 * constrain(pct, 0, 100)) / 100, 4, 2);
  u8g2.setFont(u8g2_font_5x7_tf);
  drawCentered(msg, 58);
  u8g2.sendBuffer();
}

// Toast overlay - drawn on EVERY screen (was face-only before)
void drawToast(unsigned long now) {
  if (now >= toastEnd || toastMsg.length() == 0) return;
  u8g2.setFont(u8g2_font_5x7_tf);
  int tw = min(124, u8g2.getStrWidth(toastMsg.c_str()) + 12), tx = (128 - tw) / 2;
  u8g2.setDrawColor(0); rBox(tx - 1, 51, tw + 2, 12, 5); u8g2.setDrawColor(1);
  rBox(tx, 52, tw, 10, 4); u8g2.setDrawColor(0); u8g2.drawStr(tx + 6, 60, toastMsg.c_str()); u8g2.setDrawColor(1);
}

// ================= BOOT ANIMATION =================
void playBootAnimation() {
  for (int i = 0; i <= 16; i++) {
    float p = (float)i / 16.0f;
    float ease = 1.0f - (1.0f - p) * (1.0f - p);
    int w = (int)(ease * 120.0f);
    u8g2.clearBuffer();
    u8g2.drawHLine(64 - w / 2, 32, w);
    u8g2.drawDisc(64 - w / 2, 32, 2); u8g2.drawDisc(64 + w / 2, 32, 2);
    u8g2.sendBuffer(); delay(14);
  }
  for (int f = 1; f <= 14; f++) {
    u8g2.clearBuffer();
    int d = f * 3;
    u8g2.drawPixel(64 - d, 32 - d / 2); u8g2.drawPixel(64 + d, 32 - d / 2);
    u8g2.drawPixel(64 - d, 32 + d / 2); u8g2.drawPixel(64 + d, 32 + d / 2);
    u8g2.setFont(u8g2_font_logisoso16_tf);
    drawCentered("RIDDHI", 36);
    u8g2.setFont(u8g2_font_5x7_tf); drawCentered("AI OS v7.0", 50);
    u8g2.sendBuffer(); delay(22);
  }
  for (int i = 1; i <= 12; i++) {
    float p = (float)i / 12.0f;
    int h = 2 + (int)((1.0f - (1.0f - p) * (1.0f - p)) * 22.0f);
    u8g2.clearBuffer();
    rBox(28, 32 - h / 2, 26, h, 5); rBox(74, 32 - h / 2, 26, h, 5);
    if (h > 12) {
      u8g2.setDrawColor(0);
      u8g2.drawBox(33, 32 - h / 2 + 5, 5, 5); u8g2.drawBox(79, 32 - h / 2 + 5, 5, 5);
      u8g2.setDrawColor(1);
    }
    u8g2.sendBuffer(); delay(14);
  }
}

// ================= CORE 0: WEATHER + AI BRAIN + NTFY.SH =================
float extractVal(const String& json, const char* key, float fb) {
  int ci = json.indexOf("\"current\":{"); if (ci < 0) return fb;
  int ki = json.indexOf(key, ci); if (ki < 0) return fb;
  int vs = ki + strlen(key), ve = vs;
  while (ve < (int)json.length() && (isDigit(json[ve]) || json[ve] == '.' || json[ve] == '-')) ve++;
  return json.substring(vs, ve).toFloat();
}

void bgWeather() {
  HTTPClient http; http.setTimeout(4000);
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + WEATHER_LAT +
               "&longitude=" + WEATHER_LON +
               "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m";
  if (http.begin(url) && http.GET() == HTTP_CODE_OK) {
    String p = http.getString();
    float t = extractVal(p, "\"temperature_2m\":", wTemp);
    int h = (int)extractVal(p, "\"relative_humidity_2m\":", wHum);
    int c = (int)extractVal(p, "\"weather_code\":", wCode);
    float w = extractVal(p, "\"wind_speed_10m\":", wWind);
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      wTemp = t; wHum = h; wCode = c; wWind = w; weatherReady = true; xSemaphoreGive(netMutex);
    }
  }
  http.end();
}

void bgNtfyPoll(const String& topicCopy) {
  if (topicCopy.length() < 3) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(5000);
  String url = "https://ntfy.sh/" + topicCopy + "/json?poll=1&since=45s";
  if (http.begin(client, url) && http.GET() == HTTP_CODE_OK) {
    String body = http.getString();
    int lastLn = body.lastIndexOf("{\"id\":");
    if (lastLn >= 0) {
      String line = body.substring(lastLn);
      String id = extractJsonStr(line, "id");
      String msg = extractJsonStr(line, "message");
      String ttl = extractJsonStr(line, "title");
      if (id.length() > 0 && id != lastNtfyId && msg.length() > 0) {
        lastNtfyId = id;
        if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          alertTitle = ttl.length() > 0 ? ttl : "NTFY ALERT";
          alertMsg = msg;
          alertMoodStr = "HYPED";
          alertPending = true;
          xSemaphoreGive(netMutex);
        }
      }
    }
  }
  http.end();
}

void bgAutoBrain() {
  String keyCopy = "", modCopy = "", pendingTasks = "";
  float tempCopy = 26.0f; int careCopy = 90, hrCopy = 12, mnCopy = 0, codeCopy = 0;
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    keyCopy = geminiKey; modCopy = geminiModel;
    tempCopy = wTemp; codeCopy = wCode; careCopy = careLevel;
    hrCopy = currentHour; mnCopy = currentMin;
    for (int i = 0; i < 5; i++) {
      if (!(taskDoneMask & (1 << i)) && tasks[i].length() > 0) {
        if (pendingTasks.length() > 0) pendingTasks += ", ";
        pendingTasks += tasks[i];
      }
    }
    xSemaphoreGive(netMutex);
  }
  if (keyCopy.length() < 10) return;

  String ctx = "Time:" + String(hrCopy) + ":" + (mnCopy < 10 ? "0" : "") + String(mnCopy) +
               " IST, Temp:" + String(tempCopy, 1) + "C, WCode:" + String(codeCopy) +
               ", Care:" + String(careCopy) + "%, Todo:[" + jsonEscape(pendingTasks) + "]";
  String prompt = "You are Riddhi, an autonomous ESP32 desk robot. Based on context (" + ctx +
                  "), pick a fitting mood and short witty nudge (<22 chars). Return ONLY raw JSON: "
                  "{\\\"mood\\\":\\\"CHILL|HYPED|ZEN|SUS|SMUG|LOVE\\\",\\\"toast\\\":\\\"short ASCII\\\",\\\"scr\\\":\\\"FACE|TASKS|BANNER\\\",\\\"bt\\\":\\\"\\\",\\\"bs\\\":\\\"\\\"}";
  String body = "{\"generationConfig\":{\"responseMimeType\":\"application/json\",\"maxOutputTokens\":256},"
                "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"" + prompt + "\"}]}]}";

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(9000);
  // Key goes in a header, not the URL, so it never shows up in logs/proxies
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + modCopy + ":generateContent";
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-goog-api-key", keyCopy);
    int code = http.POST(body);
    if (code == HTTP_CODE_OK) {
      String inner = extractJsonStr(http.getString(), "text");
      if (inner.length() > 5) {
        String m = extractJsonStr(inner, "mood"), ts = extractJsonStr(inner, "toast"),
               sc = extractJsonStr(inner, "scr"), bt = extractJsonStr(inner, "bt"), bs = extractJsonStr(inner, "bs");
        if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          brainMoodStr = m; brainToastStr = ts; brainScrStr = sc; brainBannerT = bt; brainBannerS = bs;
          lastBrainLog = "[" + String(hrCopy) + ":" + (mnCopy < 10 ? "0" : "") + String(mnCopy) + "] " + m + ": " + ts;
          brainHasUpdate = true;
          xSemaphoreGive(netMutex);
        }
      }
    } else if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      lastBrainLog = "Brain HTTP " + String(code); xSemaphoreGive(netMutex);
    }
  }
  http.end();
}

void core0Task(void* pv) {
  unsigned long lastW = 0, lastB = 0, lastN = 0;
  for (;;) {
    if (!netPaused && WiFi.status() == WL_CONNECTED) {
      // snapshot shared settings under the mutex
      String topic = "", key = ""; bool brainOn = false; int bInt = 20;
      if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        topic = ntfyTopic; key = geminiKey; brainOn = autoBrainOn; bInt = brainIntervalMin;
        xSemaphoreGive(netMutex);
      }
      unsigned long now = millis();
      if (fetchWeatherNow || lastW == 0 || (now - lastW > WEATHER_INTERVAL_MS)) {
        fetchWeatherNow = false; bgWeather(); lastW = millis();
      }
      if (topic.length() >= 3 && (now - lastN > 30000UL)) {
        bgNtfyPoll(topic); lastN = millis();
      }
      unsigned long bIntMs = (unsigned long)max(2, bInt) * 60000UL;
      if (triggerBrainNow || (brainOn && key.length() >= 10 && (now - lastB > bIntMs))) {
        triggerBrainNow = false; bgAutoBrain(); lastB = millis();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void runGitHubOTA() {
  if (WiFi.status() != WL_CONNECTED || ghRepo.length() < 3) { toast("Set GitHub Repo!", 2000); return; }
  if (stateDirty) saveNow();
  netPaused = true; server.stop(); MDNS.end(); delay(250);
  drawProgress(10, "Connecting GitHub...");
  WiFiClientSecure client; client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int cur, int tot) {
    static unsigned long ld = 0;
    if (millis() - ld > 180 || cur == tot) {
      ld = millis(); int pct = tot > 0 ? (cur * 100) / tot : 50;
      char b[24]; sprintf(b, "OTA %d%%", pct); drawProgress(pct, b);
    }
  });
  String url = "https://github.com/" + ghRepo + "/releases/download/latest/Riddhi_OS.ino.bin";
  if (httpUpdate.update(client, url) == HTTP_UPDATE_FAILED) {
    drawProgress(0, "OTA Failed!"); delay(2000); ESP.restart();
  }
}

// ================= HOLIDAY & TIME CHECK =================
void checkCalendarSurprises() {
  static unsigned long lastChk = 0;
  if (millis() - lastChk < 1000UL && lastChk != 0) return;
  lastChk = millis();
  struct tm ti;
  if (!getLocalTime(&ti, 2)) return;
  currentHour = ti.tm_hour; currentMin = ti.tm_min; currentSec = ti.tm_sec;
  int m = ti.tm_mon + 1, d = ti.tm_mday;
  if (m == 10 && d == 4) autoHoliday = 1;
  else if (m == 1 && d == 1) autoHoliday = 2;
  else if (m == 2 && d == 14) autoHoliday = 3;
  else if (m == 10 && d == 31) autoHoliday = 4;
  else if (m == 12 && d == 25) autoHoliday = 5;
  else autoHoliday = 0;
}
int getActiveHoliday() { return (manualHoliday >= 0) ? manualHoliday : autoHoliday; }

// ================= CUSTOM AI FACE =================
void parseCustomShapes(const String& data) {
  customShapeCount = 0; int st = 0, len = data.length();
  while (st < len && customShapeCount < 24) {
    int sm = data.indexOf(';', st); if (sm < 0) sm = len;
    String tok = data.substring(st, sm); tok.trim();
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
    st = sm + 1;
  }
}

void drawCustomFace(int by) {
  for (int i = 0; i < customShapeCount; i++) {
    const CustomShape& s = customShapes[i];
    if (s.type == 'R') { u8g2.setDrawColor(s.p[5] ? 1 : 0); rBox(s.p[0], s.p[1] + by, s.p[2], s.p[3], s.p[4]); }
    else if (s.type == 'F') { u8g2.setDrawColor(s.p[5] ? 1 : 0); rFrame(s.p[0], s.p[1] + by, s.p[2], s.p[3], s.p[4]); }
    else if (s.type == 'D' && s.p[2] > 0) { u8g2.setDrawColor(s.p[3] ? 1 : 0); u8g2.drawDisc(s.p[0], s.p[1] + by, s.p[2]); }
    else if (s.type == 'C' && s.p[2] > 0) { u8g2.setDrawColor(s.p[3] ? 1 : 0); u8g2.drawCircle(s.p[0], s.p[1] + by, s.p[2]); }
    else if (s.type == 'L') { u8g2.setDrawColor(s.p[4] ? 1 : 0); u8g2.drawLine(s.p[0], s.p[1] + by, s.p[2], s.p[3] + by); }
    else if (s.type == 'A' && s.p[2] > 0) {
      u8g2.setDrawColor(s.p[4] ? 1 : 0);
      u8g2.drawCircle(s.p[0], s.p[1] + by, s.p[2], s.p[3] ? (U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT) : (U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT));
    }
  }
  u8g2.setDrawColor(1);
}

// ================= AMBIENT SCREENSAVERS =================
void drawAmbientScreensaver(unsigned long now, int br) {
  if (saverMode == 1) {
    u8g2.setFont(u8g2_font_5x7_tf);
    for (int i = 0; i < 16; i++) {
      matY[i] += 2;
      if (matY[i] > 76) matY[i] = -random(8, 30);
      int x = i * 8 + 1;
      for (int k = 0; k < 3; k++) {
        int y = matY[i] - k * 8;
        if (y > 6 && y < 64) {
          char ch[2] = { (char)('0' + ((i * 7 + (now / 120) + k) % 2)), 0 };
          u8g2.drawStr(x, y, ch);
        }
      }
    }
  } else if (saverMode == 2) {
    dvdX += dvdVX; dvdY += dvdVY;
    if (dvdX <= 1 || dvdX >= 84) dvdVX = -dvdVX;
    if (dvdY <= 1 || dvdY >= 47) dvdVY = -dvdVY;
    rFrame((int)dvdX, (int)dvdY, 43, 16, 4);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr((int)dvdX + 4, (int)dvdY + 12, "RIDDHI");
  } else if (saverMode == 3) {
    int cx = 64, cy = 32, r = 28;
    u8g2.drawCircle(cx, cy, r);
    for (int i = 0; i < 12; i++) {
      float a = i * 0.523599f;
      u8g2.drawLine(cx + (int)(sin(a) * (r - 3)), cy - (int)(cos(a) * (r - 3)), cx + (int)(sin(a) * r), cy - (int)(cos(a) * r));
    }
    float ha = ((currentHour % 12) + currentMin / 60.0f) * 0.523599f;
    float ma = (currentMin + currentSec / 60.0f) * 0.10472f;
    float sa = currentSec * 0.10472f;
    u8g2.drawLine(cx, cy, cx + (int)(sin(ha) * 14), cy - (int)(cos(ha) * 14));
    u8g2.drawLine(cx, cy, cx + (int)(sin(ma) * 20), cy - (int)(cos(ma) * 20));
    u8g2.drawLine(cx, cy, cx + (int)(sin(sa) * 23), cy - (int)(cos(sa) * 23));
    u8g2.drawDisc(cx, cy, 2);
  } else {
    u8g2.drawHLine(28, 35 + br, 26); u8g2.drawHLine(74, 35 + br, 26);
    int ph = (now / 120) % 18;
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(96 + ph / 4, 26 - ph / 2, "z");
    u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(106 + ((ph + 6) % 12) / 3, 20 - ((ph + 6) % 16) / 2, "Z");
  }
}

// ================= WEATHER & HOLIDAY OVERLAYS =================
void drawWeatherAndHolidayFX(unsigned long now, int& shiverX) {
  shiverX = 0;
  int hol = getActiveHoliday();
  if (hol == 1) {
    rFrame(106, 48, 16, 12, 2); u8g2.drawHLine(106, 53, 16); u8g2.drawVLine(114, 43, 5);
    u8g2.drawDisc(114, 41 - ((now / 180) % 2), 1);
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(38, 8, "HBD VENU!");
  } else if (hol == 2) {
    int ph = (now / 200) % 2;
    drawSparkle(14, 16, ph ? 4 : 2); drawSparkle(114, 18, ph ? 2 : 4);
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(32, 8, "HAPPY NEW YR!");
  } else if (hol == 3) {
    drawMiniHeart(14, 24 - ((now / 120) % 12)); drawMiniHeart(114, 28 - ((now / 150) % 12));
  } else if (hol == 4) {
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(46, 8, "BOO! ^-^");
  } else if (hol == 5) {
    for (int i = 0; i < 8; i++) { int sx = (i * 17 + 7) % 124, sy = ((now / 45) + i * 11) % 62; if (sx < 22 || sx > 106) u8g2.drawPixel(sx, sy); }
  }

  if (!weatherFxOn || !weatherReady) return;
  int code = 0; float temp = 26.0f;
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(5)) == pdTRUE) { code = wCode; temp = wTemp; xSemaphoreGive(netMutex); }
  if (code >= 51) {
    for (int i = 0; i < 6; i++) { int rx = (i < 3) ? (6 + i * 6) : (106 + (i - 3) * 6), ry = ((now / 25) + i * 13) % 58; u8g2.drawLine(rx, ry, rx - 2, ry + 5); }
  } else if (code <= 1 && currentHour >= 7 && currentHour <= 17) {
    u8g2.drawDisc(120, 7, 2); if ((now / 500) % 2 == 0) drawSparkle(120, 7, 4);
  }
  if (temp < 18.0f) shiverX = ((now / 90) % 2 == 0) ? -1 : 1;
}

// ================= FACE ENGINE =================
void drawFace() {
  unsigned long now = millis(); float t = now * 0.003f;
  unsigned long idle = now - lastAct;

  if (tempMoodEnd > 0 && now > tempMoodEnd) { tempMoodEnd = 0; currentMood = (careLevel < 35) ? MOOD_SICK : MOOD_CHILL; }
  if (currentMood == MOOD_CHILL || currentMood == MOOD_SICK || currentMood == MOOD_DROWSY) {
    if (idle > SLEEP_TIMEOUT_MS) { currentMood = MOOD_SLEEP; wasSleeping = true; }
    else if (idle > DROWSY_TIMEOUT_MS) currentMood = MOOD_DROWSY;
    else if (currentMood == MOOD_CHILL && careLevel < 35) currentMood = MOOD_SICK;
  }

  int br = (int)round(sin(t) * 1.4f);
  if (currentMood == MOOD_SLEEP && saverMode != 0) {
    drawAmbientScreensaver(now, br);
    return;
  }

  int shiverX = 0; drawWeatherAndHolidayFX(now, shiverX);

  eyeX += (eyeTX - eyeX) * 0.14f; eyeY += (eyeTY - eyeY) * 0.14f;
  eyeLH += (eyeTLH - eyeLH) * 0.25f; eyeRH += (eyeTRH - eyeRH) * 0.25f;
  int ox = (int)round(eyeX) + shiverX, oy = (int)round(eyeY) + br;

  switch (currentMood) {
    case MOOD_CHILL: {
      if (blinkPhase == 0 && now > nextBlink) { blinkPhase = 1; bool wink = (random(0, 6) == 0); eyeTLH = 3; eyeTRH = wink ? 24 : 3; blinkEnd = now + (wink ? 240 : 90); }
      else if (blinkPhase == 1 && now > blinkEnd) { blinkPhase = 2; eyeTLH = 24; eyeTRH = 24; blinkEnd = now + 100; }
      else if (blinkPhase == 2 && now > blinkEnd) { blinkPhase = 0; nextBlink = now + random(2400, 4800); }
      if (now > nextLook) { eyeTX = random(0, 3) ? random(-10, 11) : 0; eyeTY = random(-4, 5); nextLook = now + random(2200, 4500); }
      int lh = (int)round(eyeLH), rh = (int)round(eyeRH), cy = 29 + oy, lx = 28 + ox, rx = 74 + ox;
      rBox(lx, cy - lh / 2, 26, lh, 5); rBox(rx, cy - rh / 2, 26, rh, 5);
      if (weatherFxOn && weatherReady && wCode <= 1 && currentHour >= 9 && currentHour <= 16) {
        u8g2.drawHLine(lx + 26, cy - 3, rx - (lx + 26)); u8g2.drawHLine(lx + 26, cy - 2, rx - (lx + 26));
      } else {
        u8g2.setDrawColor(0);
        if (lh > 12) u8g2.drawBox(lx + 5 + ox / 4, cy - lh / 2 + 5, 5, 5);
        if (rh > 12) u8g2.drawBox(rx + 5 + ox / 4, cy - rh / 2 + 5, 5, 5);
        u8g2.setDrawColor(1);
      }
      u8g2.drawCircle(64 + ox / 2, 44 + oy / 2, 4, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    }
    case MOOD_HYPED: {
      int b = (int)round(sin(t * 3.0f) * 2.0f);
      u8g2.drawTriangle(28, 36 + b, 41, 20 + b, 54, 36 + b); u8g2.drawTriangle(74, 36 + b, 87, 20 + b, 100, 36 + b);
      u8g2.setDrawColor(0); u8g2.drawTriangle(33, 36 + b, 41, 26 + b, 49, 36 + b); u8g2.drawTriangle(79, 36 + b, 87, 26 + b, 95, 36 + b); u8g2.setDrawColor(1);
      u8g2.drawCircle(64, 43 + b, 7, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    }
    case MOOD_ZEN: rBox(28, 31 + br, 26, 4, 2); rBox(74, 31 + br, 26, 4, 2); u8g2.drawCircle(64, 43 + br, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT); break;
    case MOOD_GRUMPY: {
      int sh = ((now / 70) % 2 == 0) ? -1 : 1;
      rBox(28 + sh, 22, 26, 20, 4); rBox(74 + sh, 22, 26, 20, 4);
      u8g2.setDrawColor(0); u8g2.drawTriangle(26 + sh, 18, 56 + sh, 18, 56 + sh, 32); u8g2.drawTriangle(72 + sh, 18, 102 + sh, 18, 72 + sh, 32); u8g2.setDrawColor(1);
      u8g2.drawHLine(56, 50, 16); break;
    }
    case MOOD_SHOCKED: u8g2.drawCircle(41, 30, 13); u8g2.drawDisc(41, 30, 5); u8g2.drawCircle(87, 30, 13); u8g2.drawDisc(87, 30, 5); u8g2.drawCircle(64, 49, 4); break;
    case MOOD_SUSPICIOUS: rBox(28, 28, 26, 8, 3); rBox(74, 28, 26, 8, 3); u8g2.setDrawColor(0); u8g2.drawBox(46, 30, 5, 4); u8g2.drawBox(92, 30, 5, 4); u8g2.setDrawColor(1); u8g2.drawLine(56, 48, 72, 46); break;
    case MOOD_SMUG: rBox(28, 24, 26, 16, 5); rBox(74, 24, 26, 16, 5); u8g2.setDrawColor(0); u8g2.drawBox(26, 20, 30, 10); u8g2.drawBox(72, 20, 30, 10); u8g2.setDrawColor(1); u8g2.drawLine(56, 48, 68, 48); u8g2.drawLine(68, 48, 73, 44); break;
    case MOOD_DIZZY: { int dx = (int)(cos(t * 3.5f) * 4.5f), dy = (int)(sin(t * 3.5f) * 4.5f); u8g2.drawCircle(41, 30, 12); u8g2.drawDisc(41 + dx, 30 + dy, 4); u8g2.drawCircle(87, 30, 12); u8g2.drawDisc(87 - dx, 30 - dy, 4); u8g2.drawCircle(64, 49, 3); break; }
    case MOOD_LOVE: { int p = ((now / 280) % 2 == 0) ? 1 : 0, cx[2] = {41, 87}; for (int i = 0; i < 2; i++) { u8g2.drawDisc(cx[i] - 5, 27, 6 + p); u8g2.drawDisc(cx[i] + 5, 27, 6 + p); u8g2.drawTriangle(cx[i] - 11 - p, 29, cx[i] + 11 + p, 29, cx[i], 42 + p); } drawMiniHeart(64, 15 - ((now / 110) % 10)); u8g2.drawCircle(64, 46, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT); break; }
    case MOOD_SICK: rBox(28, 30 + br, 24, 9, 3); rBox(76, 30 + br, 24, 9, 3); u8g2.drawCircle(64, 52, 5, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT); u8g2.drawLine(68, 49, 82, 43); u8g2.drawDisc(83, 43, 2); u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(28, 12, "Bored... Wave!"); break;
    case MOOD_DROWSY: rBox(28, 32 + br, 26, 7, 3); rBox(74, 32 + br, 26, 7, 3); if ((now / 1500) % 3 == 0) u8g2.drawCircle(64, 48 + br, 5); else u8g2.drawHLine(59, 48 + br, 10); break;
    case MOOD_SLEEP: drawAmbientScreensaver(now, br); break;
    case MOOD_THINK: rBox(34, 20, 22, 18, 5); rBox(78, 20, 22, 18, 5); for (int i = 0; i < 3; i++) u8g2.drawDisc(54 + i * 10, 10 - (i == (now / 200) % 3 ? 3 : 0), 2); break;
    case MOOD_CUSTOM: drawCustomFace(br); break;
  }

  if (WiFi.status() == WL_CONNECTED) { u8g2.drawPixel(3, 5); u8g2.drawCircle(3, 5, 2, U8G2_DRAW_UPPER_RIGHT); }
  if (autoBrainOn) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(10, 7, "AI"); }
  if (pomoRun && (now / 500) % 2 == 0) u8g2.drawDisc(24, 4, 2);
}

// ================= DESK SCREENS =================
void drawPC() {
  char batS[10]; sprintf(batS, "B:%d%%", pcBat);
  drawTopBar("PC & MEDIA", batS);
  u8g2.setFont(u8g2_font_5x7_tf);
  char b1[12], b2[12]; sprintf(b1, "CPU %d%%", pcCpu); sprintf(b2, "RAM %d%%", pcRam);
  u8g2.drawStr(3, 24, b1); rFrame(44, 17, 80, 7, 2); if (pcCpu > 2) rBox(46, 19, (76 * constrain(pcCpu, 0, 100)) / 100, 3, 1);
  u8g2.drawStr(3, 35, b2); rFrame(44, 28, 80, 7, 2); if (pcRam > 2) rBox(46, 30, (76 * constrain(pcRam, 0, 100)) / 100, 3, 1);
  u8g2.drawHLine(0, 40, 128);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(3, 52, pcSong.substring(0, 20).c_str());
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(3, 62, "1x:Play/Pause  2x:Next");
}

void drawDoodle() {
  if (!hasDoodle) {
    drawTopBar("PIXEL DOODLE", "WEB");
    u8g2.setFont(u8g2_font_6x10_tf);
    drawCentered("Draw in Web Desk tab", 34);
    drawCentered("to beam art here!", 48);
    return;
  }
  for (int y = 0; y < 32; y++) {
    int row = y * 8;
    for (int x = 0; x < 64; x++) {
      if (doodleBuf[row + (x >> 3)] & (1 << (x & 7))) u8g2.drawBox(x * 2, y * 2, 2, 2);
    }
  }
}

void drawNudge() {
  drawTopBar("WELLNESS", "1x:Done");
  int b = (int)round(sin(millis() * 0.008f) * 2.0f);
  if (nudgeIsWater) {
    u8g2.drawTriangle(24, 20 + b, 16, 36 + b, 32, 36 + b);
    u8g2.drawDisc(24, 37 + b, 8);
    u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(42, 34, "DRINK WATER!");
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(42, 48, "Take a quick sip");
  } else {
    u8g2.drawTriangle(24, 20 + b, 16, 30 + b, 32, 30 + b);
    u8g2.drawBox(21, 30 + b, 6, 14);
    u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(42, 34, "FIX POSTURE!");
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(42, 48, "Sit tall & relax");
  }
  u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(22, 62, "Wave 1x to dismiss");
}

// All 5 tasks at once, selected row inverted
void drawTasks() {
  int done = 0; for (int i = 0; i < 5; i++) if (taskDoneMask & (1 << i)) done++;
  char r[10]; sprintf(r, "%d/5 done", done);
  drawTopBar("TASKS", r);
  u8g2.setFont(u8g2_font_5x7_tf);
  for (int i = 0; i < 5; i++) {
    int y = 14 + i * 10;
    bool sel = (i == taskIdx), dn = (taskDoneMask & (1 << i)) != 0;
    if (sel) { u8g2.drawRBox(1, y, 126, 10, 2); u8g2.setDrawColor(0); }
    u8g2.drawFrame(4, y + 2, 6, 6);
    if (dn) u8g2.drawBox(6, y + 4, 2, 2);
    String t = tasks[i].substring(0, 20);
    u8g2.drawStr(14, y + 8, t.c_str());
    if (dn) u8g2.drawHLine(14, y + 5, u8g2.getStrWidth(t.c_str()));
    u8g2.setDrawColor(1);
  }
}

void drawClock() {
  struct tm ti; drawTopBar(dimMode ? "NIGHT" : "CLOCK", "IST");
  if (getLocalTime(&ti, 5)) {
    char tb[12], db[24], ap[6]; strftime(tb, sizeof(tb), "%I:%M", &ti); strftime(ap, sizeof(ap), "%p", &ti); strftime(db, sizeof(db), "%a, %d %b", &ti);
    u8g2.setFont(u8g2_font_logisoso24_tn); u8g2.drawStr(14, 44, tb);
    u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(98, 34, ap);
    u8g2.drawHLine(16, 49, 96);
    u8g2.drawBox(16, 48, (96 * ti.tm_sec) / 60 + 1, 3);       // seconds progress
    drawCentered(db, 61);
  } else { u8g2.setFont(u8g2_font_6x10_tf); drawCentered("Syncing Wi-Fi time...", 38); }
}

const char* weatherDesc(int c) {
  if (c == 0) return "Clear sky";
  if (c <= 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Foggy";
  if (c <= 57) return "Drizzle";
  if (c <= 67) return "Rain";
  if (c <= 77) return "Snow";
  if (c <= 82) return "Showers";
  if (c >= 95) return "Thunderstorm";
  return "Mixed";
}

void drawWeatherIcon(int code, int x, int y, unsigned long now) {
  if (code <= 1) {
    u8g2.drawDisc(x + 11, y + 11, 5);
    for (int i = 0; i < 8; i++) {
      float a = i * 0.785398f + now * 0.0008f;
      u8g2.drawLine(x + 11 + (int)(cos(a) * 8), y + 11 + (int)(sin(a) * 8), x + 11 + (int)(cos(a) * 11), y + 11 + (int)(sin(a) * 11));
    }
    return;
  }
  u8g2.drawDisc(x + 6, y + 9, 4); u8g2.drawDisc(x + 12, y + 7, 5); u8g2.drawDisc(x + 18, y + 10, 4);
  u8g2.drawBox(x + 6, y + 9, 12, 5);
  bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
  bool snow = (code >= 71 && code <= 77);
  bool storm = (code >= 95);
  for (int i = 0; i < 3; i++) {
    int dx = x + 6 + i * 6, dy = y + 16 + ((now / 120 + i * 3) % 6);
    if (rain || storm) u8g2.drawLine(dx, dy, dx - 1, dy + 2);
    else if (snow) u8g2.drawPixel(dx, dy);
  }
  if (storm) { u8g2.drawLine(x + 12, y + 14, x + 10, y + 18); u8g2.drawLine(x + 10, y + 18, x + 13, y + 18); u8g2.drawLine(x + 13, y + 18, x + 11, y + 22); }
}

void drawWeather() {
  drawTopBar("WEATHER", "1x:Sync");
  u8g2.setFont(u8g2_font_6x10_tf);
  if (WiFi.status() != WL_CONNECTED) { drawCentered("Connect Wi-Fi first", 38); return; }
  if (!weatherReady) { drawCentered("Fetching weather...", 38); return; }
  float t = 0, w = 0; int h = 0, c = 0;
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(10)) == pdTRUE) { t = wTemp; h = wHum; w = wWind; c = wCode; xSemaphoreGive(netMutex); }
  drawWeatherIcon(c, 6, 17, millis());
  char tb[12]; sprintf(tb, "%.1fC", t);
  u8g2.setFont(u8g2_font_logisoso18_tf); u8g2.drawStr(40, 38, tb);
  u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(40, 48, weatherDesc(c));
  char sb[28]; sprintf(sb, "RH:%d%%   Wind:%.0fkm/h", h, w);
  u8g2.drawHLine(0, 51, 128); u8g2.drawStr(10, 61, sb);
}

void drawPomo() {
  char b[10], t[20];
  sprintf(b, "%02d:%02d", (int)((pomoLeft / 1000) / 60), (int)((pomoLeft / 1000) % 60));
  sprintf(t, "%s %dM #%d", pomoBreak ? "BREAK" : "FOCUS", (int)(pomoTotal / 60000UL), pomoSessions + (pomoBreak ? 0 : 1));
  drawTopBar(t, pomoRun ? "RUN" : "PAUSE");
  u8g2.setFont(u8g2_font_logisoso22_tn); u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 43, b);
  int bw = pomoTotal > 0 ? (int)((116UL * pomoLeft) / pomoTotal) : 0;
  rFrame(4, 51, 120, 9, 4); if (bw > 2) rBox(6, 53, min(116, bw), 5, 2);
}

void drawCounter() {
  drawTopBar(tallyLabel.c_str(), "1x:+1");
  char b[16]; sprintf(b, "%d", tallyCount); u8g2.setFont(u8g2_font_logisoso24_tn);
  u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 46, b);
  u8g2.setFont(u8g2_font_5x7_tf); drawCentered("Hold IR: Reset to 0", 60);
}

void drawDice() {
  drawTopBar("DICE / ORACLE", "1x:Roll");
  bool rolling = (millis() < diceRollUntil); int showD = rolling ? random(1, 7) : diceVal;
  rFrame(14, 18, 38, 38, 6); int cx = 33, cy = 37;
  if (showD % 2 == 1) u8g2.drawDisc(cx, cy, 3);
  if (showD >= 2) { u8g2.drawDisc(cx - 10, cy - 10, 3); u8g2.drawDisc(cx + 10, cy + 10, 3); }
  if (showD >= 4) { u8g2.drawDisc(cx + 10, cy - 10, 3); u8g2.drawDisc(cx - 10, cy + 10, 3); }
  if (showD == 6) { u8g2.drawDisc(cx - 10, cy, 3); u8g2.drawDisc(cx + 10, cy, 3); }
  rFrame(66, 22, 48, 30, 8); u8g2.setFont(u8g2_font_logisoso16_tf);
  const char* ans = rolling ? "..." : (oracleYes ? "YES" : "NO");
  u8g2.drawStr(66 + (48 - u8g2.getStrWidth(ans)) / 2, 44, ans);
}

void drawBreath() {
  drawTopBar("4-7-8 BREATH", "4x:Home");
  unsigned long ms = millis() % 19000UL; int r; const char* lbl;
  if (ms < 4000) { r = 6 + (int)(ms * 14 / 4000); lbl = "INHALE 4s"; }
  else if (ms < 11000) { r = 20; lbl = "HOLD 7s"; }
  else { r = 20 - (int)((ms - 11000) * 14 / 8000); lbl = "EXHALE 8s"; }
  u8g2.drawCircle(32, 38, constrain(r, 4, 20)); u8g2.drawDisc(32, 38, constrain(r - 4, 1, 16));
  u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(60, 41, lbl);
}

void drawBanner() {
  drawTopBar("DESK SIGN", "LIVE");
  u8g2.setFont(u8g2_font_6x12_tf); drawCentered(bannerTop.c_str(), 33);
  u8g2.drawHLine(16, 39, 96);
  u8g2.setFont(u8g2_font_6x10_tf); drawCentered(bannerSub.c_str(), 53);
}

void drawWifi() {
  drawTopBar("CONNECT", "2xHold:Reset");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(3, 25, isAP ? "WiFi: Riddhi-OS" : "http://riddhi.local");
  u8g2.drawStr(3, 38, isAP ? "Pass: 12345678" : "or IP address:");
  u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(3, 55, ("http://" + activeIP).c_str());
}

// ================= MENU =================
void drawMenuIcon(int idx, int x, int y) {
  switch (idx) {
    case 0: u8g2.drawFrame(x, y, 7, 7); u8g2.drawPixel(x + 2, y + 2); u8g2.drawPixel(x + 4, y + 2); u8g2.drawHLine(x + 2, y + 4, 3); break;
    case 1: u8g2.drawFrame(x, y, 7, 7); u8g2.drawLine(x + 2, y + 3, x + 3, y + 5); u8g2.drawLine(x + 3, y + 5, x + 5, y + 2); break;
    case 2: u8g2.drawCircle(x + 3, y + 3, 3); u8g2.drawVLine(x + 3, y + 1, 3); u8g2.drawHLine(x + 3, y + 3, 2); break;
    case 3: u8g2.drawDisc(x + 3, y + 3, 2); u8g2.drawPixel(x + 3, y); u8g2.drawPixel(x + 3, y + 6); u8g2.drawPixel(x, y + 3); u8g2.drawPixel(x + 6, y + 3); break;
    case 4: u8g2.drawHLine(x + 1, y, 5); u8g2.drawHLine(x + 1, y + 6, 5); u8g2.drawLine(x + 1, y + 1, x + 5, y + 5); u8g2.drawLine(x + 5, y + 1, x + 1, y + 5); break;
    case 5: u8g2.drawHLine(x + 1, y + 3, 5); u8g2.drawVLine(x + 3, y + 1, 5); break;
    case 6: u8g2.drawFrame(x, y, 7, 7); u8g2.drawPixel(x + 2, y + 2); u8g2.drawPixel(x + 4, y + 4); break;
    case 7: u8g2.drawCircle(x + 3, y + 3, 3); u8g2.drawPixel(x + 3, y + 3); break;
    case 8: u8g2.drawFrame(x, y + 1, 7, 5); u8g2.drawHLine(x + 2, y + 3, 3); break;
    case 9: u8g2.drawFrame(x, y, 7, 5); u8g2.drawHLine(x + 2, y + 6, 3); u8g2.drawPixel(x + 3, y + 5); break;
    case 10: u8g2.drawLine(x + 1, y + 5, x + 5, y + 1); u8g2.drawPixel(x + 1, y + 6); u8g2.drawPixel(x + 2, y + 6); break;
    case 18: u8g2.drawHLine(x, y + 3, 7); u8g2.drawVLine(x + 3, y, 7); u8g2.drawPixel(x + 2, y + 2); u8g2.drawPixel(x + 4, y + 4); break;
    case 19: u8g2.drawPixel(x + 3, y + 6); u8g2.drawHLine(x + 2, y + 4, 3); u8g2.drawHLine(x + 1, y + 2, 5); u8g2.drawHLine(x, y, 7); break;
    default: u8g2.drawFrame(x, y + 1, 7, 5); u8g2.drawPixel(x + 2, y + 3); u8g2.drawPixel(x + 4, y + 2); u8g2.drawPixel(x + 5, y + 4); break;
  }
}

void drawMenu() {
  static float selX = 3.0f, selY = 15.0f;
  static int lastPage = -1;

  int page = menuIndex / 8;
  int totalPages = (MENU_COUNT + 7) / 8;
  char pg[8]; sprintf(pg, "%d/%d", page + 1, totalPages);
  drawTopBar("MENU", pg);

  int selSlot = menuIndex % 8;
  float tx = 3.0f + (selSlot % 4) * 31.0f;
  float ty = 15.0f + (selSlot / 4) * 20.0f;
  if (page != lastPage) { selX = tx; selY = ty; lastPage = page; }
  else { selX += (tx - selX) * 0.30f; selY += (ty - selY) * 0.30f; }

  rBox((int)round(selX), (int)round(selY), 28, 17, 4);

  for (int i = 0; i < 8; i++) {
    int idx = page * 8 + i;
    if (idx >= MENU_COUNT) break;
    int bx = 3 + (i % 4) * 31, by = 15 + (i / 4) * 20;
    if (idx == menuIndex) {
      u8g2.setDrawColor(0); drawMenuIcon(idx, bx + 10, by + 5); u8g2.setDrawColor(1);
    } else {
      rFrame(bx, by, 28, 17, 4); drawMenuIcon(idx, bx + 10, by + 5);
    }
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  drawCentered(menuItems[menuIndex], 63);
}

// ================= MINI-GAMES =================
void startGame(ScreenMode s) {
  currentScreen = s; gState = G_INTRO; lastAct = millis(); newBest = false;
  shipLane = 1; astX = 136; astLane = random(0, 3); astSpeed = 1.9f; laserX = -10; scoreSpace = 0;
  orbY = 32; orbVel = 0; pipeX = 136; pipeGap = random(18, 36); scoreFlappy = 0;
  ballX = 64; ballY = 32; ballVX = 1.6f; ballVY = 1.2f; padY = 26; padDir = 1.6f; scorePong = 0;
  catchSide = 0; dropY = 14; dropSide = random(0, 2); dropSpeed = 1.4f; scoreCatch = 0;
  reactPhase = 0; reactGoAt = millis() + random(1600, 4400); reactResultMs = 0;
  dinoY = 48; dinoVy = 0; cacX = 136; cacSpeed = 2.0f; scoreDino = 0;
  ringR = 26.0f; ringSpeed = 0.55f; scoreRhythm = 0;
}

void endGame(int score, int& hi) {
  gState = G_DEAD; gDeadAt = millis();
  newBest = (score > hi);
  if (newBest) { hi = score; stateDirty = true; }
  boostCare(8);
}

void drawGameHud(const char* title, int hi, int sc) {
  char b[16]; sprintf(b, "HI:%02d SC:%02d", hi, sc); drawTopBar(title, b);
}
void drawGameIntro(const char* action) {
  u8g2.setFont(u8g2_font_5x7_tf);
  drawCentered(action, 34); drawCentered("Hold: Exit", 46);
}
void drawGameOver(const char* msg) {
  u8g2.setDrawColor(0); rBox(24, 16, 80, 32, 4); u8g2.setDrawColor(1); rFrame(24, 16, 80, 32, 4);
  u8g2.setFont(u8g2_font_6x12_tf); drawCentered(msg, 31);
  u8g2.setFont(u8g2_font_5x7_tf); drawCentered(newBest ? "NEW BEST!" : "Wave: Retry", 43);
}

void gameTap() {
  unsigned long now = millis(); lastAct = now;
  if (currentScreen == SCR_REACT) {
    if (gState == G_INTRO || reactPhase >= 2) {
      startGame(SCR_REACT); gState = G_RUN; reactPhase = 0; reactGoAt = now + random(1600, 4400); return;
    }
    if (reactPhase == 0) { reactPhase = 3; }
    else if (reactPhase == 1) {
      reactResultMs = (int)(now - reactGoAt);
      if (reactResultMs < bestReactMs) { bestReactMs = reactResultMs; stateDirty = true; }
      reactPhase = 2; boostCare(8);
    }
    return;
  }

  if (gState == G_INTRO || (gState == G_DEAD && now - gDeadAt > 450)) { startGame(currentScreen); gState = G_RUN; return; }
  if (gState != G_RUN) return;

  if (currentScreen == SCR_SPACE) { shipLane = (shipLane + 1) % 3; if (laserX < 0) { laserX = 22; laserLane = shipLane; } }
  else if (currentScreen == SCR_FLAPPY) orbVel = -2.8f;
  else if (currentScreen == SCR_PONG) padDir = -padDir;
  else if (currentScreen == SCR_CATCH) catchSide = 1 - catchSide;
  else if (currentScreen == SCR_DINO) { if (dinoY >= 47.5f) dinoVy = -4.2f; }
  else if (currentScreen == SCR_RHYTHM) {
    if (fabs(ringR - 11.0f) <= 4.5f) { scoreRhythm++; ringR = 26.0f; if (ringSpeed < 1.5f) ringSpeed += 0.05f; }
    else endGame(scoreRhythm, hiRhythm);
  }
}

void drawSpaceGame() {
  int ly[3] = {22, 38, 54};
  if (gState == G_RUN) {
    astX -= astSpeed;
    if (laserX >= 0) {
      laserX += 4;
      if (laserX > 128) laserX = -10;
      else if (laserLane == astLane && laserX >= astX) { scoreSpace += 2; laserX = -10; astX = 134; astLane = random(0, 3); if (astSpeed < 4.0f) astSpeed += 0.08f; }
    }
    if (astX < -10) { astX = 134; astLane = random(0, 3); scoreSpace++; }
    if (astLane == shipLane && astX < 22 && astX + 10 > 10) endGame(scoreSpace, hiSpace);
  }
  drawGameHud("SPACE DEF", hiSpace, scoreSpace);
  int sy = ly[shipLane]; u8g2.drawTriangle(10, sy - 5, 10, sy + 5, 22, sy);
  if (laserX >= 0) u8g2.drawHLine(laserX, ly[laserLane], 6);
  if (gState != G_INTRO) u8g2.drawDisc((int)astX, ly[astLane], 5);
  if (gState == G_INTRO) drawGameIntro("Wave: Dodge + Fire");
  else if (gState == G_DEAD) drawGameOver("BLASTED!");
}

void drawFlappyGame() {
  if (gState == G_RUN) {
    orbVel += 0.16f; orbY += orbVel; pipeX -= 1.45f;
    if (pipeX < -12) { pipeX = 132; pipeGap = random(18, 36); scoreFlappy++; }
    if (orbY < 15 || orbY > 60 || (30 > pipeX && 22 < pipeX + 12 && (orbY - 4 < pipeGap || orbY + 4 > pipeGap + 22))) endGame(scoreFlappy, hiFlappy);
  }
  drawGameHud("FLAPPY ORB", hiFlappy, scoreFlappy);
  u8g2.drawDisc(26, (int)orbY, 4);
  if (gState != G_INTRO) { u8g2.drawBox((int)pipeX, 13, 12, pipeGap - 13); u8g2.drawBox((int)pipeX, pipeGap + 22, 12, 42); }
  if (gState == G_INTRO) drawGameIntro("Wave: Flap");
  else if (gState == G_DEAD) drawGameOver("BONKED!");
}

void drawPongGame() {
  if (gState == G_RUN) {
    padY = constrain(padY + padDir, 14.0f, 46.0f); if (padY <= 14.0f || padY >= 46.0f) padDir = -padDir;
    ballX += ballVX; ballY += ballVY;
    if (ballY <= 15 || ballY >= 61) ballVY = -ballVY;
    if (ballX >= 123) ballVX = -fabs(ballVX);
    if (ballX <= 14 && ballX >= 9 && ballY >= padY - 2 && ballY <= padY + 18) { ballVX = min(4.2f, (float)fabs(ballVX) * 1.05f); scorePong++; }
    else if (ballX < 4) endGame(scorePong, hiPong);
  }
  drawGameHud("SOLO PONG", hiPong, scorePong);
  u8g2.drawBox(9, (int)padY, 3, 16); u8g2.drawVLine(125, 13, 51);
  if (gState != G_INTRO) u8g2.drawDisc((int)ballX, (int)ballY, 2);
  if (gState == G_INTRO) drawGameIntro("Wave: Flip paddle");
  else if (gState == G_DEAD) drawGameOver("MISSED!");
}

void drawCatchGame() {
  int cx[2] = {38, 90};
  if (gState == G_RUN) {
    dropY += dropSpeed;
    if (dropY >= 52) {
      if (dropSide == catchSide) { scoreCatch++; dropY = 15; dropSide = random(0, 2); if (dropSpeed < 3.4f) dropSpeed += 0.08f; }
      else endGame(scoreCatch, hiCatch);
    }
  }
  drawGameHud("ORB CATCH", hiCatch, scoreCatch);
  rBox(cx[catchSide] - 12, 55, 24, 5, 2);
  if (gState != G_INTRO) u8g2.drawDisc(cx[dropSide], (int)dropY, 4);
  if (gState == G_INTRO) drawGameIntro("Wave: Switch side");
  else if (gState == G_DEAD) drawGameOver("DROPPED!");
}

void drawReactGame() {
  char b[14]; sprintf(b, "BEST:%dms", bestReactMs); drawTopBar("REFLEX TEST", b);
  if (gState == G_INTRO) { drawGameIntro("Wave to Start"); return; }
  if (reactPhase == 0) {
    if (millis() >= reactGoAt) reactPhase = 1;
    u8g2.setFont(u8g2_font_6x12_tf); drawCentered("WAIT FOR IT...", 40);
  } else if (reactPhase == 1) {
    rBox(6, 16, 116, 44, 6);
    u8g2.setDrawColor(0); u8g2.setFont(u8g2_font_logisoso16_tf); drawCentered("WAVE NOW!", 46); u8g2.setDrawColor(1);
  } else if (reactPhase == 2) {
    char res[16]; sprintf(res, "%d ms", reactResultMs);
    u8g2.setFont(u8g2_font_logisoso20_tn); drawCentered(res, 42);
    u8g2.setFont(u8g2_font_5x7_tf); drawCentered("Wave:Retry  Hold:Exit", 58);
  } else {
    u8g2.setFont(u8g2_font_6x12_tf); drawCentered("TOO EARLY!", 38);
    u8g2.setFont(u8g2_font_5x7_tf); drawCentered("Wave:Retry  Hold:Exit", 56);
  }
}

void drawDinoGame() {
  if (gState == G_RUN) {
    dinoVy += 0.28f; dinoY += dinoVy;
    if (dinoY > 48.0f) { dinoY = 48.0f; dinoVy = 0; }
    cacX -= cacSpeed;
    if (cacX < -10) { cacX = 132 + random(0, 30); scoreDino++; if (cacSpeed < 4.2f) cacSpeed += 0.08f; }
    if (cacX < 28 && cacX > 14 && dinoY > 38) endGame(scoreDino, hiDino);
  }
  drawGameHud("CYBER DINO", hiDino, scoreDino);
  u8g2.drawHLine(0, 58, 128);
  rBox(18, (int)dinoY, 10, 10, 2); u8g2.drawBox(24, (int)dinoY - 4, 7, 5);
  if (gState != G_INTRO) u8g2.drawBox((int)cacX, 46, 7, 12);
  if (gState == G_INTRO) drawGameIntro("Wave: Jump");
  else if (gState == G_DEAD) drawGameOver("CRASHED!");
}

void drawRhythmGame() {
  if (gState == G_RUN) {
    ringR -= ringSpeed;
    if (ringR < 5.0f) endGame(scoreRhythm, hiRhythm);
  }
  drawGameHud("BEAT TAP", hiRhythm, scoreRhythm);
  u8g2.drawCircle(64, 38, 11); u8g2.drawCircle(64, 38, 12);
  if (gState == G_RUN) u8g2.drawCircle(64, 38, (int)ringR);
  if (gState == G_INTRO) drawGameIntro("Wave when ring aligns");
  else if (gState == G_DEAD) drawGameOver("OFF BEAT!");
}

// ================= MULTI-ACTION AI EXECUTOR =================
void executeAiActionPipe(const String& pipeStr) {
  int st = 0, len = pipeStr.length();
  while (st < len) {
    int bar = pipeStr.indexOf('|', st); if (bar < 0) bar = len;
    String item = pipeStr.substring(st, bar); item.trim();
    int col = item.indexOf(':');
    String op = (col > 0) ? item.substring(0, col) : item; op.toUpperCase();
    String val = (col > 0) ? item.substring(col + 1) : "";

    if (op == "POMO") {
      int mn = constrain((int)val.toInt(), 1, 99);
      pomoBreak = false; pomoTotal = (unsigned long)mn * 60000UL; pomoLeft = pomoTotal;
      pomoRun = true; pomoTick = millis(); queueScreen(SCR_POMO);
    } else if (op == "BANNER") {
      int til = val.indexOf('~');
      bannerTop = (til >= 0 ? val.substring(0, til) : val).substring(0, 16);
      bannerSub = (til >= 0 ? val.substring(til + 1) : "AI Sign").substring(0, 20);
      queueScreen(SCR_BANNER);
    } else if (op == "TASK_SET") {
      int til = val.indexOf('~');
      if (til > 0) {
        int idx = constrain((int)val.substring(0, til).toInt(), 0, 4);
        tasks[idx] = val.substring(til + 1).substring(0, 16);
        taskDoneMask &= ~(1 << idx); taskIdx = idx; stateDirty = true; queueScreen(SCR_TASKS);
      }
    } else if (op == "TASK_TOG") {
      int idx = constrain((int)val.toInt(), 0, 4);
      taskDoneMask ^= (1 << idx); taskIdx = idx; stateDirty = true; boostCare(10); queueScreen(SCR_TASKS);
    } else if (op == "TALLY") { tallyCount = (int)val.toInt(); stateDirty = true; queueScreen(SCR_COUNTER); }
    else if (op == "DIM") { dimMode = (val.toInt() != 0); }
    else if (op == "FLIP") { flippedDisplay = (val.toInt() != 0); u8g2.setFlipMode(flippedDisplay ? 1 : 0); }
    else if (op == "HOLIDAY") { manualHoliday = constrain((int)val.toInt(), -1, 5); }
    else if (op == "SAVER") { saverMode = constrain((int)val.toInt(), 0, 3); stateDirty = true; }
    else if (op == "CHEER") { boostCare(25); }
    else if (op == "SCREEN") {
      val.toUpperCase();
      if (val == "FACE") queueScreen(SCR_FACE);
      else if (val == "TASKS") queueScreen(SCR_TASKS);
      else if (val == "CLOCK") queueScreen(SCR_CLOCK);
      else if (val == "WEATHER") { fetchWeatherNow = true; queueScreen(SCR_WEATHER); }
      else if (val == "POMO") queueScreen(SCR_POMO);
      else if (val == "COUNTER") queueScreen(SCR_COUNTER);
      else if (val == "DICE") { diceVal = random(1, 7); oracleYes = random(0, 2); diceRollUntil = millis() + 650; queueScreen(SCR_DICE); }
      else if (val == "BREATH") queueScreen(SCR_BREATH);
      else if (val == "BANNER") queueScreen(SCR_BANNER);
      else if (val == "PC") queueScreen(SCR_PC);
      else if (val == "DOODLE") queueScreen(SCR_DOODLE);
      else if (val == "SPACE") { startGame(SCR_SPACE); pendingAt = 0; }
      else if (val == "FLAPPY") { startGame(SCR_FLAPPY); pendingAt = 0; }
      else if (val == "PONG") { startGame(SCR_PONG); pendingAt = 0; }
      else if (val == "CATCH") { startGame(SCR_CATCH); pendingAt = 0; }
      else if (val == "REACT") { startGame(SCR_REACT); pendingAt = 0; }
      else if (val == "DINO") { startGame(SCR_DINO); pendingAt = 0; }
      else if (val == "RHYTHM") { startGame(SCR_RHYTHM); pendingAt = 0; }
    }
    st = bar + 1;
  }
}

// ================= IR GESTURES =================
void onGesture(int waves, bool held) {
  unsigned long now = millis();
  bool sleepy = wasSleeping || displayOff;
  wakeUp(); pendingAt = 0;

  if (currentScreen == SCR_NUDGE) {
    currentScreen = SCR_FACE; currentMood = MOOD_HYPED; tempMoodEnd = now + 2000;
    boostCare(15); toast("Great Job! +15%", 1500); return;
  }
  if (sleepy && currentScreen == SCR_FACE) {
    currentMood = MOOD_SHOCKED; tempMoodEnd = now + 700; boostCare(10); toast("Whoa! Awake!", 900); return;
  }
  if (waves >= 4) { currentScreen = SCR_FACE; currentMood = MOOD_CHILL; toast("Home", 800); return; }

  if (held) {
    if (isGame(currentScreen)) { currentScreen = SCR_FACE; gState = G_INTRO; toast("GG!", 1000); }
    else if (currentScreen == SCR_COUNTER) { tallyCount = 0; stateDirty = true; toast("Reset 0", 1000); }
    else if (currentScreen == SCR_POMO) {
      pomoRun = false; pomoBreak = !pomoBreak;
      pomoTotal = (pomoBreak ? 5UL : (unsigned long)POMO_PRESETS[pomoPreset]) * 60000UL; pomoLeft = pomoTotal;
    }
    else if (currentScreen == SCR_WIFI) {
      // two holds within 4s to avoid wiping Wi-Fi by accident
      if (wifiResetArm != 0 && now - wifiResetArm < 4000) { WiFi.disconnect(true, true); delay(500); ESP.restart(); }
      else { wifiResetArm = now; toast("Hold again: RESET", 2500); }
    }
    else if (currentScreen == SCR_FACE) { currentMood = MOOD_LOVE; tempMoodEnd = now + 2600; boostCare(25); toast("Cheered Up! <3", 1500); }
    return;
  }
  if (waves == 3) {
    if (currentScreen == SCR_MENU) {
      static const ScreenMode mapM[MENU_COUNT] = {
        SCR_FACE, SCR_TASKS, SCR_CLOCK, SCR_WEATHER, SCR_POMO, SCR_COUNTER, SCR_DICE,
        SCR_BREATH, SCR_BANNER, SCR_PC, SCR_DOODLE,
        SCR_SPACE, SCR_FLAPPY, SCR_PONG, SCR_CATCH, SCR_REACT, SCR_DINO, SCR_RHYTHM,
        SCR_AI, SCR_WIFI
      };
      ScreenMode chosen = mapM[menuIndex];
      if (chosen == SCR_WEATHER) fetchWeatherNow = true;
      if (isGame(chosen)) startGame(chosen); else currentScreen = chosen;
    } else currentScreen = SCR_MENU;
    return;
  }
  if (waves == 2) {
    if (currentScreen == SCR_MENU) menuIndex = (menuIndex + 1) % MENU_COUNT;
    else if (currentScreen == SCR_TASKS) taskIdx = (taskIdx + 1) % 5;
    else if (currentScreen == SCR_PC) { pcCmdQueue = "NEXT"; toast("Next Track >>", 1000); }
    else if (currentScreen == SCR_FACE) { currentMood = (Mood)(((int)currentMood + 1) % 9); tempMoodEnd = 0; }
    else if (currentScreen == SCR_POMO && !pomoRun) {
      pomoPreset = (pomoPreset + 1) % 4; pomoBreak = false;
      pomoTotal = (unsigned long)POMO_PRESETS[pomoPreset] * 60000UL; pomoLeft = pomoTotal;
      toast(String("Focus ") + POMO_PRESETS[pomoPreset] + "m", 900);
    }
    else currentScreen = SCR_MENU;
    return;
  }
  if (waves == 1) {
    if (isGame(currentScreen)) gameTap();
    else if (currentScreen == SCR_PC) { pcCmdQueue = "PLAYPAUSE"; toast("Play / Pause", 1000); }
    else if (currentScreen == SCR_TASKS) { taskDoneMask ^= (1 << taskIdx); stateDirty = true; boostCare(10); }
    else if (currentScreen == SCR_COUNTER) { tallyCount++; stateDirty = true; if (tallyCount % 10 == 0) toast("Milestone! " + String(tallyCount), 1600); }
    else if (currentScreen == SCR_DICE) { diceVal = random(1, 7); oracleYes = random(0, 2); diceRollUntil = now + 650; }
    else if (currentScreen == SCR_WEATHER) { fetchWeatherNow = true; toast("Syncing...", 1000); }
    else if (currentScreen == SCR_POMO) { pomoRun = !pomoRun; pomoTick = now; }
    else if (currentScreen == SCR_MENU) menuIndex = (menuIndex + 1) % MENU_COUNT;
    else if (currentScreen == SCR_FACE) { boostCare(20); currentMood = MOOD_HYPED; tempMoodEnd = now + 2000; const char* q[] = {"Hey Venu!", "Feeling Good!", "Let's Go!"}; toast(q[random(0, 3)], 1200); }
  }
}

void pollIR() {
  unsigned long now = millis(); bool raw = (digitalRead(IR_PIN) == LOW);
  if (raw != irPrevRaw) { irDebMs = now; irPrevRaw = raw; }
  if (now - irDebMs > 28 && raw != irStable) {
    irStable = raw;
    if (irStable) { irDownAt = now; irHeld = false; if (isGame(currentScreen) && !displayOff) gameTap(); }
    else if (!irHeld && !(isGame(currentScreen) && !displayOff)) { irWaves++; irUpAt = now; }
  }
  if (irStable && !irHeld && (now - irDownAt > 950)) { irHeld = true; irWaves = 0; onGesture(0, true); }
  if (!irStable && irWaves > 0 && (now - irUpAt > 500)) { int c = irWaves; irWaves = 0; onGesture(c, false); }
}

// ================= WEB UI (v7.0) =================
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Riddhi">
<meta name="theme-color" content="#090b10">
<title>Riddhi OS</title><style>
:root{--bg:#090b10;--card:#121622;--btn:#1b2234;--cyan:#22d3ee;--amber:#fbbf24;--emerald:#34d399;--rose:#f43f5e;--purp:#a855f7;}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:var(--bg);color:#f1f5f9;margin:0;padding:14px 12px 86px;}
.hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;padding:0 2px;}
.hdr h2{margin:0;font-size:20px;color:#fff;}
.badge{font-size:11px;font-weight:700;padding:4px 10px;border-radius:99px;background:rgba(168,85,247,.16);color:#d8b4fe;border:1px solid rgba(168,85,247,.38);}
.badge.off{background:rgba(244,63,94,.16);color:#fda4af;border-color:rgba(244,63,94,.4);}
.pg{display:none;}.pg.on{display:block;animation:up .15s ease;}
@keyframes up{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.box{background:var(--card);border:1px solid rgba(255,255,255,.07);border-radius:18px;padding:13px;margin-bottom:11px;}
.ai-box{border-color:rgba(168,85,247,.38);}
.lbl{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:1px;color:#64748b;margin-bottom:9px;}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:8px;}.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px;}
.row{display:flex;gap:7px;margin-bottom:8px;align-items:center;}
button{background:var(--btn);color:#e2e8f0;border:1px solid rgba(255,255,255,.07);border-radius:12px;padding:10px 7px;font-size:12px;font-weight:600;cursor:pointer;}
button:active{transform:scale(.96);filter:brightness(1.2);}
.cy{background:var(--cyan);color:#04131a;border:none;}
.am{background:var(--amber);color:#1c1102;border:none;}
.em{background:var(--emerald);color:#022217;border:none;}
.pu{background:linear-gradient(135deg,#7c3aed,#ec4899);color:#fff;border:none;}
.ro{background:rgba(244,63,94,.18);border-color:var(--rose);color:#fda4af;}
.done{background:rgba(52,211,153,.18);border-color:var(--emerald);color:#6ee7b7;text-decoration:line-through;}
input,select{width:100%;padding:9px 10px;border-radius:11px;border:1px solid rgba(255,255,255,.1);background:#090b10;color:#fff;margin-bottom:8px;font-size:13px;}
input[type=range]{padding:0;accent-color:var(--cyan);}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:9px;}
.chip{background:rgba(168,85,247,.14);border:1px solid rgba(168,85,247,.35);color:#e9d5ff;padding:5px 10px;border-radius:99px;font-size:11px;font-weight:600;}
#aiOut,#bLog,#devInfo{background:#090b10;border:1px solid rgba(255,255,255,.08);border-radius:11px;padding:10px;min-height:42px;font-size:12px;margin-bottom:9px;line-height:1.5;}
.gitem{display:flex;justify-content:space-between;align-items:center;padding:9px 11px;background:#090b10;border-radius:12px;margin-bottom:7px;border:1px solid rgba(255,255,255,.06);}
.gitem b{display:block;font-size:12.5px;}.gitem span{font-size:11px;color:#64748b;}
.sub{font-size:11px;color:#64748b;margin-top:6px;}
.sbar{width:100%;height:7px;background:#090b10;border-radius:6px;overflow:hidden;margin:5px 0 9px;}
.sfill{height:100%;background:linear-gradient(90deg,var(--cyan),var(--emerald));transition:width .25s;}
nav{position:fixed;bottom:0;left:0;right:0;background:rgba(9,11,16,.95);backdrop-filter:blur(10px);border-top:1px solid rgba(255,255,255,.08);display:flex;justify-content:space-around;padding:8px 4px 12px;}
.nb{background:none;border:none;color:#64748b;font-size:11px;display:flex;flex-direction:column;align-items:center;gap:3px;padding:4px 12px;border-radius:12px;}
.nb.on{color:var(--cyan);background:rgba(34,211,238,.12);}
.nb span{font-size:16px;}
#tst{position:fixed;top:14px;left:50%;transform:translateX(-50%);background:var(--cyan);color:#04131a;padding:6px 14px;border-radius:99px;font-size:12px;font-weight:700;display:none;z-index:99;}
</style></head><body>

<div id="tst">OK</div>
<div class="hdr"><h2>Riddhi OS</h2><span id="badge" class="badge">v7.0</span></div>

<div class="box" style="text-align:center;padding:10px">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:7px">
    <span class="lbl" style="margin:0">Live OLED (Tap: 1x &bull; Hold: IR Hold)</span>
    <button id="pvBtn" class="cy" style="padding:4px 10px;font-size:11px;border-radius:99px" onclick="togPv()">LIVE: ON</button>
  </div>
  <canvas id="oledCv" width="128" height="64" style="width:100%;max-width:256px;aspect-ratio:2/1;background:#04070d;border:2px solid rgba(34,211,238,.45);border-radius:10px;image-rendering:pixelated;touch-action:none;user-select:none;-webkit-user-select:none;-webkit-touch-callout:none;cursor:pointer"></canvas>
</div>

<!-- TAB 1: AI -->
<div id="t-ai" class="pg on">
  <div class="box ai-box">
    <div class="lbl">Voice &amp; Multi-Action AI Controller</div>
    <div id="aiOut">Try: "Start 45m focus timer, set sign to CODING / BUSY, and change task 1 to Revise notes"</div>
    <div class="chips">
      <button class="chip" onclick="qAI('Start 25 min timer and set sign to STUDYING / FOCUS')">Study Setup</button>
      <button class="chip" onclick="qAI('Draw a cool cyber visor face')">Cyber Visor</button>
      <button class="chip" onclick="qAI('Open cyber dino game')">Cyber Dino</button>
      <button class="chip" onclick="qAI('Switch screensaver to matrix rain')">Matrix Rain</button>
    </div>
    <div class="row">
      <button id="micBtn" style="width:44px" onclick="startMic()">&#127908;</button>
      <input id="aiIn" style="margin:0;flex:1" placeholder="Speak or type command..." onkeydown="if(event.key==='Enter')runAI()">
      <button class="pu" style="width:56px" onclick="runAI()">Go</button>
    </div>
    <div class="g2">
      <button id="ttsBtn" style="font-size:11.5px;padding:8px" onclick="togTTS()">Voice Reply: OFF</button>
      <button style="font-size:11.5px;padding:8px" onclick="togKey()">API Key &amp; Model</button>
    </div>
    <div id="kBox" style="display:none;margin-top:10px;border-top:1px solid rgba(255,255,255,.08);padding-top:10px">
      <input id="ak" type="password" placeholder="Paste Google AI Studio API Key">
      <select id="am">
        <option value="gemini-3.8-flash">gemini-3.8-flash (Recommended)</option>
        <option value="gemini-3.5-flash-lite">gemini-3.5-flash-lite (Fastest)</option>
        <option value="gemini-2.5-flash">gemini-2.5-flash (Stable fallback)</option>
      </select>
      <button class="cy" style="width:100%" onclick="saveK()">Save Key to Phone + ESP32</button>
    </div>
  </div>

  <div class="box">
    <div class="lbl">Autonomous Core 0 Brain</div>
    <div id="bLog">Autonomous brain idle.</div>
    <div class="g2" style="margin-bottom:8px">
      <button id="brnBtn" onclick="cmd('/brain?tog=1','Brain Toggled!')">Auto Brain: OFF</button>
      <button class="am" onclick="cmd('/brain?now=1','Riddhi is thinking...')">Think Now</button>
    </div>
    <div class="row" style="margin:0">
      <span style="font-size:12px;color:#94a3b8">Think every:</span>
      <select id="bInt" style="margin:0;flex:1" onchange="cmd('/brain?int='+this.value,'Interval Saved!')">
        <option value="10">10 minutes</option><option value="20" selected>20 minutes</option>
        <option value="30">30 minutes</option><option value="60">60 minutes</option>
      </select>
    </div>
  </div>
</div>

<!-- TAB 2: DESK -->
<div id="t-desk" class="pg">
  <div class="box">
    <div class="lbl">Mood, Care (<span id="carePct">90%</span>) &amp; Wellness Nudge</div>
    <div class="sbar"><div id="careBar" class="sfill" style="width:90%"></div></div>
    <div class="g2" style="margin-bottom:8px">
      <button class="em" onclick="cmd('/cheer','Cheered Up!')">Cheer Up (+25%)</button>
      <button id="wFxBtn" onclick="cmd('/wfx','Weather FX Toggled')">Weather FX: ON</button>
    </div>
    <div class="row" style="margin:0">
      <button id="ndgBtn" style="flex:1" onclick="cmd('/nudge?tog=1','Nudge Toggled')">Hydration/Posture: OFF</button>
      <select id="ndgM" style="width:86px;margin:0" onchange="cmd('/nudge?m='+this.value,'Saved!')">
        <option value="30">30m</option><option value="45" selected>45m</option><option value="60">60m</option>
      </select>
    </div>
  </div>

  <div class="box">
    <div class="lbl">Finger-Draw Pixel Canvas (64&times;32 &rarr; OLED)</div>
    <canvas id="dCv" width="64" height="32" style="width:100%;aspect-ratio:2/1;background:#04070d;border:1px solid rgba(34,211,238,.45);border-radius:10px;image-rendering:pixelated;touch-action:none;cursor:crosshair;margin-bottom:8px"></canvas>
    <div class="g3">
      <button class="cy" onclick="sendDoodle()">Beam to OLED</button>
      <button id="erBtn" onclick="togErase()">Mode: Draw</button>
      <button class="ro" onclick="clrDoodle()">Clear</button>
    </div>
  </div>

  <div class="box">
    <div class="lbl">Sticky-Note Task Carousel</div>
    <div id="tList" style="display:flex;flex-direction:column;gap:6px;margin-bottom:9px"></div>
    <div class="row" style="margin:0">
      <select id="tSlot" style="width:62px;margin:0"><option value="0">#1</option><option value="1">#2</option><option value="2">#3</option><option value="3">#4</option><option value="4">#5</option></select>
      <input id="tTxt" style="margin:0;flex:1" maxlength="16" placeholder="Edit task...">
      <button class="cy" style="width:56px" onclick="saveTask()">Set</button>
    </div>
  </div>

  <div class="box">
    <div class="lbl">Desk Apps &amp; Expressions</div>
    <div class="g3" style="margin-bottom:8px">
      <button class="cy" onclick="cmd('/scr?s=0','Face')">Face</button>
      <button onclick="cmd('/scr?s=2','Tasks')">Tasks</button>
      <button onclick="cmd('/scr?s=3','Clock')">Clock</button>
      <button onclick="cmd('/scr?s=4','Weather')">Weather</button>
      <button onclick="cmd('/scr?s=5','Timer')">Timer</button>
      <button onclick="cmd('/scr?s=6','Counter')">Counter</button>
      <button onclick="cmd('/scr?s=7','Dice')">Dice</button>
      <button onclick="cmd('/scr?s=8','Breath')">4-7-8</button>
      <button onclick="cmd('/scr?s=9','Sign')">Sign</button>
      <button class="am" onclick="cmd('/scr?s=10','PC Monitor')">PC/Media</button>
      <button onclick="cmd('/scr?s=11','Doodle')">Doodle</button>
      <button onclick="cmd('/scr?s=12','Nudge')">Test Nudge</button>
    </div>
    <div class="g3">
      <button onclick="cmd('/mood?m=0','Chill')">Chill</button>
      <button class="am" onclick="cmd('/mood?m=1','Hyped')">Hyped</button>
      <button onclick="cmd('/mood?m=2','Zen')">Zen</button>
      <button onclick="cmd('/mood?m=4','Shocked')">Shocked</button>
      <button onclick="cmd('/mood?m=5','Sus')">Sus</button>
      <button onclick="cmd('/mood?m=6','Smug')">Smug</button>
      <button onclick="cmd('/mood?m=7','Dizzy')">Dizzy</button>
      <button onclick="cmd('/mood?m=8','Love')">Love</button>
      <button onclick="cmd('/mood?m=11','Sleep')">Sleep</button>
    </div>
  </div>

  <div class="box">
    <div class="lbl">Desk Sign &amp; Tally</div>
    <input id="bTop" maxlength="16" placeholder="Top line (e.g. FOCUS MODE)">
    <div class="row">
      <input id="bSub" style="margin:0;flex:1" maxlength="20" placeholder="Bottom line (e.g. Do Not Disturb)">
      <button class="cy" style="width:62px" onclick="setBanner()">Show</button>
    </div>
    <div class="g2">
      <button class="em" onclick="cmd('/cnt?inc=1','+1!')">Tally +1 (<span id="cVal">0</span>)</button>
      <button onclick="cmd('/dice','Rolled!')">Roll Dice</button>
    </div>
  </div>
</div>

<!-- TAB 3: ARCADE -->
<div id="t-play" class="pg">
  <div class="box">
    <div class="lbl">7 IR Mini-Games</div>
    <div class="gitem"><div><b>Reaction Reflex (ms)</b><span>Wave the instant it flashes!</span></div><button class="am" style="width:64px" onclick="cmd('/scr?s=17','Reflex!')">Play</button></div>
    <div class="gitem"><div><b>Cyber Dino Runner</b><span>Wave to jump over obstacles</span></div><button class="am" style="width:64px" onclick="cmd('/scr?s=18','Dino!')">Play</button></div>
    <div class="gitem"><div><b>Rhythm Beat Tap</b><span>Wave right when the ring aligns</span></div><button class="am" style="width:64px" onclick="cmd('/scr?s=19','Beat!')">Play</button></div>
    <div class="gitem"><div><b>Space Defender</b><span>Wave to switch lanes &amp; fire</span></div><button class="em" style="width:64px" onclick="cmd('/scr?s=13','Space!')">Play</button></div>
    <div class="gitem"><div><b>Flappy Orb</b><span>Wave to flap through pillars</span></div><button class="em" style="width:64px" onclick="cmd('/scr?s=14','Flappy!')">Play</button></div>
    <div class="gitem"><div><b>Solo Pong</b><span>Wave to reverse paddle</span></div><button class="em" style="width:64px" onclick="cmd('/scr?s=15','Pong!')">Play</button></div>
    <div class="gitem" style="margin:0"><div><b>Orb Catch</b><span>Wave to toggle catcher side</span></div><button class="em" style="width:64px" onclick="cmd('/scr?s=16','Catch!')">Play</button></div>
  </div>
  <div class="box">
    <div class="lbl">Virtual IR Remote</div>
    <div class="g2">
      <button class="cy" onclick="cmd('/g?w=1','1x')">1x Action</button>
      <button onclick="cmd('/g?w=2','2x')">2x Next</button>
      <button onclick="cmd('/g?w=3','3x')">3x Menu / OK</button>
      <button onclick="cmd('/g?w=4','4x')">4x Home</button>
    </div>
  </div>
</div>

<!-- TAB 4: SYSTEM -->
<div id="t-sys" class="pg">
  <div class="box">
    <div class="lbl">Device</div>
    <div id="devInfo">Loading...</div>
    <div class="lbl" style="margin-top:4px">Brightness</div>
    <input type="range" id="bri" min="5" max="255" value="255" onchange="cmd('/bright?v='+this.value,'Brightness set')">
    <div class="g2">
      <button id="adBtn" onclick="cmd('/autodim','Night dim toggled')">Night Dim (23-06h): OFF</button>
      <button onclick="cmd('/dim','Dim toggled')">Dim Now</button>
    </div>
  </div>
  <div class="box">
    <div class="lbl">Idle Screensaver &amp; Holiday Preview</div>
    <select id="savSel" onchange="cmd('/cfg?sav='+this.value,'Screensaver Set!')">
      <option value="0">Screensaver: Classic Zzz Sleep</option>
      <option value="1">Screensaver: Matrix Digital Rain</option>
      <option value="2">Screensaver: Bouncing DVD RIDDHI</option>
      <option value="3">Screensaver: Retro Analog Clock</option>
    </select>
    <select id="holSel" onchange="cmd('/holiday?h='+this.value,'Updated!')">
      <option value="-1">Holiday: Auto (Calendar Date)</option>
      <option value="1">Preview: Birthday Cake (Oct 4)</option>
      <option value="2">Preview: New Year Fireworks</option>
      <option value="3">Preview: Floating Hearts</option>
      <option value="4">Preview: Spooky Halloween</option>
      <option value="5">Preview: Christmas Snow</option>
      <option value="0">Holiday: Off</option>
    </select>
    <div class="sub">OLED switches fully off after 10 min of sleep. Any wave wakes it.</div>
  </div>
  <div class="box">
    <div class="lbl">Webhook &amp; ntfy.sh Alerts</div>
    <div class="row" style="margin:0">
      <input id="ntfyIn" style="margin:0;flex:1" placeholder="Private ntfy.sh topic (optional)">
      <button class="cy" style="width:64px" onclick="saveNtfy()">Save</button>
    </div>
    <div class="sub">Local webhook: <b>/alert?t=Title&amp;m=Hello&amp;mood=HYPED</b></div>
  </div>
  <div class="box">
    <div class="lbl">1-Tap GitHub Releases Update</div>
    <input id="ghIn" placeholder="Public repo (e.g. yourname/Riddhi)">
    <div class="g2">
      <button onclick="saveGh()">Save Repo</button>
      <button class="am" onclick="pullGh()">Pull Latest .bin</button>
    </div>
  </div>
  <div class="box">
    <div class="lbl">Manual .bin Firmware Upload</div>
    <input type="file" id="otaF" accept=".bin">
    <button class="cy" style="width:100%" onclick="upOTA()">Flash Firmware</button>
    <div id="oBar" class="sbar" style="display:none"><div id="oFill" class="sfill" style="width:0%"></div></div>
    <div id="oSt" class="sub"></div>
  </div>
  <div class="box">
    <div class="lbl">Wi-Fi &amp; Display</div>
    <button style="width:100%;margin-bottom:8px" onclick="scanW()">Scan 2.4GHz Networks</button>
    <div id="wList" class="chips"></div>
    <input id="ssid" placeholder="Wi-Fi SSID"><input id="pass" type="password" placeholder="Wi-Fi Password">
    <div class="g2">
      <button class="cy" onclick="saveW()">Save &amp; Reboot</button>
      <button onclick="cmd('/flip','Flipped!')">Flip Display</button>
    </div>
    <button class="ro" style="width:100%;margin-top:8px" onclick="if(confirm('Forget Wi-Fi and reboot?'))cmd('/resetwifi','Resetting')">Reset Wi-Fi</button>
    <div id="wSt" class="sub"></div>
  </div>
</div>

<nav>
  <button class="nb on" onclick="tab('ai',this)"><span>&#10024;</span>AI</button>
  <button class="nb" onclick="tab('desk',this)"><span>&#9632;</span>Desk</button>
  <button class="nb" onclick="tab('play',this)"><span>&#9654;</span>Arcade</button>
  <button class="nb" onclick="tab('sys',this)"><span>&#9881;</span>System</button>
</nav>

<script>
let hist=[], ttsOn=false;
const $=id=>document.getElementById(id);
const k0=localStorage.getItem('rk')||''; $('ak').value=k0;
const m0=localStorage.getItem('rm')||'';
if([...$('am').options].some(o=>o.value===m0)) $('am').value=m0;
if(!k0) $('kBox').style.display='block';

function tab(id,el){
  document.querySelectorAll('.pg').forEach(p=>p.classList.remove('on'));
  document.querySelectorAll('.nb').forEach(b=>b.classList.remove('on'));
  $('t-'+id).classList.add('on'); el.classList.add('on');
  sync();
}
function tst(m){const t=$('tst');t.innerText=m;t.style.display='block';setTimeout(()=>t.style.display='none',1300);}
function cmd(u,m){fetch(u).catch(()=>tst('Riddhi offline'));if(m)tst(m);setTimeout(sync,400);}
function post(u,obj){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(obj).toString()});}
function togTTS(){ttsOn=!ttsOn;$('ttsBtn').innerText='Voice Reply: '+(ttsOn?'ON':'OFF');}
function togKey(){const b=$('kBox');b.style.display=b.style.display==='none'?'block':'none';}
function saveK(){
  const k=$('ak').value.trim(), m=$('am').value;
  localStorage.setItem('rk',k); localStorage.setItem('rm',m);
  $('kBox').style.display='none';
  post('/cfg',{akey:k,amod:m});
  tst('Saved to Phone & ESP32!');
}
function saveTask(){
  const i=$('tSlot').value, txt=$('tTxt').value.trim();
  if(!txt) return; cmd('/task?i='+i+'&t='+encodeURIComponent(txt),'Task Saved!');
  $('tTxt').value='';
}
function setBanner(){cmd('/banner?t='+encodeURIComponent($('bTop').value||'FOCUS')+'&s='+encodeURIComponent($('bSub').value||'Riddhi OS'),'Sign Updated!');}
function saveGh(){post('/cfg',{gh:$('ghIn').value.trim()}).then(()=>tst('Repo Saved!'));}
function saveNtfy(){post('/cfg',{ntfy:$('ntfyIn').value.trim()}).then(()=>tst('ntfy Saved!'));}
function pullGh(){if(confirm('Download & flash latest Riddhi_OS.ino.bin from GitHub?')) cmd('/ghota','Updating...');}
function qAI(s){$('aiIn').value=s;runAI();}

function startMic(){
  const SR=window.SpeechRecognition||window.webkitSpeechRecognition;
  if(!SR){tst('Use keyboard mic on iOS/HTTP');return;}
  const r=new SR(); r.lang='en-IN'; r.interimResults=false;
  const mb=$('micBtn'); mb.style.background='#f43f5e';
  r.onresult=(e)=>{mb.style.background=''; $('aiIn').value=e.results[0][0].transcript; runAI();};
  r.onerror=()=>{mb.style.background=''; tst('Use keyboard dictation mic');};
  r.onend=()=>{mb.style.background='';};
  r.start();
}

function fmtUp(s){const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h>0?h+'h '+m+'m':m+'m '+(s%60)+'s';}
async function sync(){
  try{
    const r=await fetch('/status'); if(!r.ok) throw 0;
    const j=await r.json();
    $('badge').innerText='v'+j.fw+' \u2022 online'; $('badge').className='badge';
    $('cVal').innerText=j.cnt;
    $('carePct').innerText=j.care+'%';
    $('careBar').style.width=j.care+'%';
    $('wFxBtn').innerText='Weather FX: '+(j.wfx?'ON':'OFF');
    $('ndgBtn').innerText='Hydration/Posture: '+(j.ndg?'ON':'OFF');
    $('ndgBtn').className=j.ndg?'em':'';
    $('ndgM').value=String(j.ndgm||45);
    $('savSel').value=String(j.sav||0);
    $('brnBtn').innerText='Auto Brain: '+(j.brn?'ON':'OFF');
    $('brnBtn').className=j.brn?'em':'';
    $('bInt').value=String(j.bint||20);
    $('bLog').innerText=j.blog||'Autonomous brain idle.';
    $('adBtn').innerText='Night Dim (23-06h): '+(j.adim?'ON':'OFF');
    $('adBtn').className=j.adim?'em':'';
    if(document.activeElement!==$('bri')) $('bri').value=j.bri;
    $('devInfo').innerHTML='IP: <b>'+j.ip+'</b><br>Wi-Fi: '+(j.rssi?j.rssi+' dBm':'AP mode')+'<br>Free heap: '+Math.round(j.heap/1024)+' KB<br>Uptime: '+fmtUp(j.up);
    if(!$('ghIn').value) $('ghIn').value=j.gh||'';
    if(!$('ntfyIn').value) $('ntfyIn').value=j.ntfy||'';
    const tl=$('tList'); tl.innerHTML='';
    (j.tasks||[]).forEach((t,i)=>{
      const b=document.createElement('button');
      const dn=(j.tmask&(1<<i))!==0;
      b.className=dn?'done':''; b.style.textAlign='left';
      b.innerText=(dn?'[x] ':'[ ] ')+(i+1)+'. '+t;
      b.onclick=()=>cmd('/task?tog='+i,'Toggled!');
      tl.appendChild(b);
    });
  }catch(e){$('badge').innerText='offline'; $('badge').className='badge off';}
}
sync(); setInterval(()=>{if(!document.hidden)sync();},8000);

// ===== FINGER-DRAW PIXEL CANVAS =====
const dCv=$('dCv'), dCtx=dCv.getContext('2d');
let dGrid=new Uint8Array(64*32), dDown=false, dErase=false;
function renderD(){
  dCtx.fillStyle='#04070d'; dCtx.fillRect(0,0,64,32);
  dCtx.fillStyle='#22d3ee';
  for(let y=0;y<32;y++) for(let x=0;x<64;x++) if(dGrid[y*64+x]) dCtx.fillRect(x,y,1,1);
}
function paintD(e){
  const r=dCv.getBoundingClientRect();
  const x=Math.floor((e.clientX-r.left)*64/r.width), y=Math.floor((e.clientY-r.top)*32/r.height);
  if(x>=0&&x<64&&y>=0&&y<32){ dGrid[y*64+x]=dErase?0:1; renderD(); }
}
dCv.addEventListener('pointerdown',e=>{e.preventDefault();dDown=true;paintD(e);});
dCv.addEventListener('pointermove',e=>{if(dDown){e.preventDefault();paintD(e);}});
window.addEventListener('pointerup',()=>dDown=false);
function togErase(){dErase=!dErase;$('erBtn').innerText='Mode: '+(dErase?'Erase':'Draw');}
function clrDoodle(){dGrid.fill(0);renderD();}
function sendDoodle(){
  let hex='';
  for(let y=0;y<32;y++){
    for(let bx=0;bx<8;bx++){
      let byte=0;
      for(let bit=0;bit<8;bit++) if(dGrid[y*64+bx*8+bit]) byte|=(1<<bit);
      hex+=byte.toString(16).padStart(2,'0');
    }
  }
  post('/doodle',{hex:hex}).then(()=>tst('Beamed to OLED!'));
}
renderD();

async function scanW(){
  const st=$('wSt'), box=$('wList');
  st.innerText='Scanning...'; box.innerHTML='';
  try{
    const lines=(await(await fetch('/scan')).text()).split('\n').filter(x=>x.trim());
    st.innerText=lines.length?'Tap network:':'None found';
    lines.forEach(n=>{const b=document.createElement('button');b.className='chip';b.innerText=n;b.onclick=()=>$('ssid').value=n;box.appendChild(b);});
  }catch(e){st.innerText='Scan failed';}
}
function saveW(){const s=$('ssid').value.trim();if(!s)return;$('wSt').innerText='Rebooting...';post('/savewifi',{s:s,p:$('pass').value});}

function upOTA(){
  const f=$('otaF').files[0], st=$('oSt'), bar=$('oBar'), fill=$('oFill');
  if(!f){st.innerText='Pick a .bin file first';return;}
  pvOn=false; $('pvBtn').innerText='LIVE: OFF';
  bar.style.display='block'; fill.style.width='0%'; st.innerText='Uploading...';
  const fd=new FormData(); fd.append('update',f,f.name);
  const x=new XMLHttpRequest(); x.open('POST','/update',true);
  x.upload.onprogress=(e)=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);fill.style.width=p+'%';st.innerText='Flashing '+p+'%';}};
  x.onload=()=>{if(x.status===200&&x.responseText==='OK'){fill.style.width='100%';st.innerText='Done! Rebooting...';setTimeout(()=>location.reload(),5000);}else st.innerText='Failed!';};
  x.send(fd);
}

function serShapes(a){if(!Array.isArray(a))return'';const cl=(v,l,h)=>Math.max(l,Math.min(h,Math.round(+v||0))),o=[];
for(const s of a.slice(0,24)){const t=(s.type||'').toLowerCase(),c=(s.c===0)?0:1;
if(t==='rbox')o.push(`R,${cl(s.x,0,127)},${cl(s.y,0,63)},${cl(s.w,2,128)},${cl(s.h,2,64)},${cl(s.r,0,20)},${c}`);
else if(t==='disc')o.push(`D,${cl(s.x,0,127)},${cl(s.y,0,63)},${cl(s.r,1,32)},${c}`);
else if(t==='circle')o.push(`C,${cl(s.x,0,127)},${cl(s.y,0,63)},${cl(s.r,1,32)},${c}`);
else if(t==='line')o.push(`L,${cl(s.x1,0,127)},${cl(s.y1,0,63)},${cl(s.x2,0,127)},${cl(s.y2,0,63)},${c}`);
else if(t==='arc')o.push(`A,${cl(s.x,0,127)},${cl(s.y,0,63)},${cl(s.r,1,30)},${s.dir?1:0},${c}`);}return o.join(';');}

function serActs(arr){if(!Array.isArray(arr))return'';const o=[];
for(const a of arr.slice(0,6)){
  const op=(a.op||'').toUpperCase();
  if(op==='POMO') o.push('POMO:'+(a.arg||25));
  else if(op==='BANNER') o.push('BANNER:'+(a.a||'FOCUS').replace(/[|~]/g,'')+'~'+(a.b||'Riddhi').replace(/[|~]/g,''));
  else if(op==='TASK_SET') o.push('TASK_SET:'+(a.idx||0)+'~'+(a.text||'Task').replace(/[|~]/g,''));
  else if(op==='TASK_TOG') o.push('TASK_TOG:'+(a.idx||0));
  else if(op==='TALLY') o.push('TALLY:'+(a.val||0));
  else if(op==='DIM') o.push('DIM:'+(a.val?1:0));
  else if(op==='FLIP') o.push('FLIP:'+(a.val?1:0));
  else if(op==='HOLIDAY') o.push('HOLIDAY:'+(a.val??-1));
  else if(op==='SAVER') o.push('SAVER:'+(a.val||0));
  else if(op==='CHEER') o.push('CHEER:1');
  else if(op==='SCREEN') o.push('SCREEN:'+(a.val||'FACE').toUpperCase());
}return o.join('|');}

async function runAI(){
  const k=localStorage.getItem('rk')||'', m=localStorage.getItem('rm')||'gemini-3.8-flash', p=$('aiIn').value.trim(), out=$('aiOut');
  if(!k){$('kBox').style.display='block';out.innerText='Add Gemini API Key first.';return;}
  if(!p)return; $('aiIn').value=''; out.innerText='Executing...'; fetch('/aithink');
  const sys='You are Riddhi v7.0, an AI-controlled ESP32 desk robot (128x64 OLED). Return ONLY JSON: {"mood":"CHILL|HYPED|ZEN|GRUMPY|SHOCKED|SUS|SMUG|DIZZY|LOVE|SLEEP","reply":"short ASCII <54 chars","shapes":[],"actions":[]}. Custom face shapes: {"type":"rbox","x":,"y":,"w":,"h":,"r":,"c":1}, {"type":"disc|circle","x":,"y":,"r":}, {"type":"line","x1":,"y1":,"x2":,"y2":}, {"type":"arc","x":,"y":,"r":,"dir":0|1}. Supported actions: {"op":"POMO","arg":25}, {"op":"BANNER","a":"TOP","b":"SUB"}, {"op":"TASK_SET","idx":0,"text":"Task"}, {"op":"TASK_TOG","idx":0}, {"op":"TALLY","val":10}, {"op":"DIM","val":1}, {"op":"FLIP","val":1}, {"op":"HOLIDAY","val":1}, {"op":"SAVER","val":0..3 (0=Zzz,1=Matrix,2=DVD,3=Analog)}, {"op":"CHEER"}, {"op":"SCREEN","val":"FACE|TASKS|CLOCK|WEATHER|POMO|COUNTER|DICE|BREATH|BANNER|PC|DOODLE|SPACE|FLAPPY|PONG|CATCH|REACT|DINO|RHYTHM"}.';
  const req=[...hist,{role:'user',parts:[{text:p}]}];
  try{
    const r=await fetch('https://generativelanguage.googleapis.com/v1beta/models/'+m+':generateContent?key='+encodeURIComponent(k),{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({system_instruction:{parts:[{text:sys}]},generationConfig:{responseMimeType:'application/json'},contents:req})});
    const d=await r.json();
    if(!r.ok) throw new Error((d.error&&d.error.message)||('HTTP '+r.status));
    const j=JSON.parse(d.candidates[0].content.parts[0].text.replace(/^```json|```$/gi,'').trim());
    hist.push({role:'user',parts:[{text:p}]},{role:'model',parts:[{text:j.reply||'OK'}]});
    if(hist.length>6) hist=hist.slice(-6);
    out.innerText=j.reply||'Done!';
    const b={m:(j.mood||'HYPED').toUpperCase(), t:(j.reply||'').replace(/[^\x20-\x7E]/g,'').slice(0,60)};
    const sh=serShapes(j.shapes); if(sh) b.shapes=sh;
    const ac=serActs(j.actions); if(ac) b.acts=ac;
    await post('/ai',b);
    setTimeout(sync,350);
    if(ttsOn&&'speechSynthesis'in window){window.speechSynthesis.cancel();window.speechSynthesis.speak(new SpeechSynthesisUtterance(j.reply||''));}
  }catch(e){out.innerText='Err: '+e.message; fetch('/mood?m=7');}
}

// ===== LIVE OLED PREVIEW =====
let pvOn=true, pvBusy=false, cvTimer=null, cvHeld=false;
const cv=$('oledCv'), ctx=cv.getContext('2d'), imgD=ctx.createImageData(128,64);
function togPv(){pvOn=!pvOn;const b=$('pvBtn');b.innerText='LIVE: '+(pvOn?'ON':'OFF');b.className=pvOn?'cy':'';}
async function drawOled(){
  if(!pvOn||pvBusy||document.hidden) return;
  pvBusy=true;
  try{
    const r=await fetch('/oled');
    if(r.ok){
      const bytes=new Uint8Array(await r.arrayBuffer());
      if(bytes.length>=1024){
        const d=imgD.data;
        for(let y=0;y<64;y++){
          const rowOff=(y>>3)*128, bit=1<<(y&7);
          for(let x=0;x<128;x++){
            const on=(bytes[rowOff+x]&bit)!==0, idx=(y*128+x)*4;
            d[idx]=on?34:4; d[idx+1]=on?211:7; d[idx+2]=on?238:13; d[idx+3]=255;
          }
        }
        ctx.putImageData(imgD,0,0);
      }
    }
  }catch(e){}
  pvBusy=false;
}
setInterval(drawOled, 400);

cv.addEventListener('contextmenu', e=>e.preventDefault());
cv.addEventListener('pointerdown', e=>{
  e.preventDefault(); cvHeld=false; cv.style.borderColor='#fbbf24';
  cvTimer=setTimeout(()=>{ cvHeld=true; cv.style.borderColor='#34d399'; fetch('/g?h=1'); tst('IR Hold'); setTimeout(drawOled,120); }, 650);
});
function endCv(isUp){
  clearTimeout(cvTimer); cv.style.borderColor='rgba(34,211,238,.45)';
  if(isUp && !cvHeld){ fetch('/g?w=1'); setTimeout(drawOled,80); }
}
cv.addEventListener('pointerup', ()=>endCv(true));
cv.addEventListener('pointercancel', ()=>endCv(false));
cv.addEventListener('pointerleave', ()=>endCv(false));
</script></body></html>
)rawliteral";

void setupWeb() {
  server.on("/", []() { server.send_P(200, "text/html", HTML_PAGE); });

  server.on("/oled", []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "application/octet-stream", (PGM_P)u8g2.getBufferPtr(), 1024);
  });

  server.on("/status", []() {
    String tArr = "[";
    for (int i = 0; i < 5; i++) { if (i > 0) tArr += ","; tArr += "\"" + jsonEscape(tasks[i]) + "\""; }
    tArr += "]";
    String logCopy = "";
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(50)) == pdTRUE) { logCopy = lastBrainLog; xSemaphoreGive(netMutex); }
    String j = "{\"fw\":\"" FW_VER "\",\"cnt\":" + String(tallyCount) + ",\"care\":" + String(careLevel) +
               ",\"wfx\":" + String(weatherFxOn ? 1 : 0) + ",\"sav\":" + String(saverMode) +
               ",\"ndg\":" + String(nudgeOn ? 1 : 0) + ",\"ndgm\":" + String(nudgeMin) +
               ",\"tmask\":" + String(taskDoneMask) + ",\"tasks\":" + tArr +
               ",\"gh\":\"" + jsonEscape(ghRepo) + "\",\"ntfy\":\"" + jsonEscape(ntfyTopic) +
               "\",\"brn\":" + String(autoBrainOn ? 1 : 0) + ",\"bint\":" + String(brainIntervalMin) +
               ",\"bri\":" + String(brightness) + ",\"adim\":" + String(autoDim ? 1 : 0) +
               ",\"ip\":\"" + activeIP + "\",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) +
               ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"up\":" + String(millis() / 1000UL) +
               ",\"blog\":\"" + jsonEscape(logCopy) + "\"}";
    server.send(200, "application/json", j);
  });

  server.on("/pc", []() {
    if (server.hasArg("cpu")) pcCpu = constrain((int)server.arg("cpu").toInt(), 0, 100);
    if (server.hasArg("ram")) pcRam = constrain((int)server.arg("ram").toInt(), 0, 100);
    if (server.hasArg("bat")) pcBat = constrain((int)server.arg("bat").toInt(), 0, 100);
    if (server.hasArg("song")) pcSong = server.arg("song").substring(0, 24);
    String reply = pcCmdQueue;
    pcCmdQueue = "";
    server.send(200, "text/plain", reply.length() > 0 ? reply : "OK");
  });

  server.on("/alert", HTTP_ANY, []() {
    String t = server.hasArg("t") ? server.arg("t") : "ALERT";
    String m = server.hasArg("m") ? server.arg("m") : "Ping!";
    String md = server.hasArg("mood") ? server.arg("mood") : "HYPED";
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      alertTitle = t; alertMsg = m; alertMoodStr = md; alertPending = true;
      xSemaphoreGive(netMutex);
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/doodle", HTTP_POST, []() {
    String hex = server.arg("hex");
    bool ok = hex.length() >= 512;
    for (int i = 0; ok && i < 512; i++) if (!isxdigit((unsigned char)hex[i])) ok = false;
    if (ok) {
      for (int i = 0; i < 256; i++) {
        char buf[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        doodleBuf[i] = (uint8_t)strtol(buf, NULL, 16);
      }
      hasDoodle = true; currentScreen = SCR_DOODLE; wakeUp();
    }
    server.send(200, "text/plain", ok ? "OK" : "BAD");
  });

  server.on("/nudge", []() {
    if (server.hasArg("tog")) { nudgeOn = !nudgeOn; lastNudgeMs = millis(); saveNow(); }
    if (server.hasArg("m")) { nudgeMin = constrain((int)server.arg("m").toInt(), 5, 180); lastNudgeMs = millis(); saveNow(); }
    server.send(200, "text/plain", "OK");
  });

  server.on("/brain", []() {
    if (server.hasArg("tog")) { autoBrainOn = !autoBrainOn; saveNow(); }
    if (server.hasArg("int")) { brainIntervalMin = constrain((int)server.arg("int").toInt(), 2, 120); saveNow(); }
    if (server.hasArg("now")) {
      if (geminiKey.length() < 10) toast("Save API Key first!", 1800);
      else { triggerBrainNow = true; currentScreen = SCR_FACE; currentMood = MOOD_THINK; tempMoodEnd = millis() + 10000; wakeUp(); toast("AI Thinking...", 4000); }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/cfg", []() {
    if (server.hasArg("gh")) ghRepo = server.arg("gh");
    if (server.hasArg("sav")) saverMode = constrain((int)server.arg("sav").toInt(), 0, 3);
    if (server.hasArg("ntfy") || server.hasArg("akey") || server.hasArg("amod")) {
      if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (server.hasArg("ntfy")) ntfyTopic = server.arg("ntfy");
        if (server.hasArg("akey")) geminiKey = server.arg("akey");
        if (server.hasArg("amod")) geminiModel = server.arg("amod");
        xSemaphoreGive(netMutex);
      }
    }
    saveNow(); server.send(200, "text/plain", "OK");
  });

  server.on("/cheer", []() { boostCare(25); currentScreen = SCR_FACE; currentMood = MOOD_LOVE; tempMoodEnd = millis() + 2600; wakeUp(); toast("Thanks! <3", 1500); server.send(200, "text/plain", "OK"); });
  server.on("/wfx", []() { weatherFxOn = !weatherFxOn; stateDirty = true; server.send(200, "text/plain", "OK"); });
  server.on("/holiday", []() { manualHoliday = server.arg("h").toInt(); currentScreen = SCR_FACE; wakeUp(); server.send(200, "text/plain", "OK"); });

  server.on("/task", []() {
    if (server.hasArg("tog")) { int i = constrain((int)server.arg("tog").toInt(), 0, 4); taskDoneMask ^= (1 << i); taskIdx = i; stateDirty = true; boostCare(8); }
    else if (server.hasArg("i") && server.hasArg("t")) { int i = constrain((int)server.arg("i").toInt(), 0, 4); tasks[i] = server.arg("t").substring(0, 16); taskDoneMask &= ~(1 << i); taskIdx = i; stateDirty = true; }
    currentScreen = SCR_TASKS; wakeUp(); server.send(200, "text/plain", "OK");
  });

  server.on("/ghota", []() { server.send(200, "text/plain", "OK"); triggerGhOta = true; });
  server.on("/dim", []() { dimMode = !dimMode; server.send(200, "text/plain", "OK"); });
  server.on("/autodim", []() { autoDim = !autoDim; saveNow(); server.send(200, "text/plain", "OK"); });
  server.on("/bright", []() { brightness = (uint8_t)constrain((int)server.arg("v").toInt(), 5, 255); dimMode = false; stateDirty = true; server.send(200, "text/plain", "OK"); });
  server.on("/flip", []() { flippedDisplay = !flippedDisplay; u8g2.setFlipMode(flippedDisplay ? 1 : 0); server.send(200, "text/plain", "OK"); });
  server.on("/cnt", []() { if (server.hasArg("inc")) tallyCount += server.arg("inc").toInt(); stateDirty = true; currentScreen = SCR_COUNTER; wakeUp(); server.send(200, "text/plain", "OK"); });
  server.on("/dice", []() { currentScreen = SCR_DICE; diceVal = random(1, 7); oracleYes = random(0, 2); diceRollUntil = millis() + 650; wakeUp(); server.send(200, "text/plain", "OK"); });
  server.on("/banner", []() { if (server.hasArg("t")) bannerTop = server.arg("t").substring(0, 16); if (server.hasArg("s")) bannerSub = server.arg("s").substring(0, 20); currentScreen = SCR_BANNER; wakeUp(); server.send(200, "text/plain", "OK"); });

  server.on("/g", []() {
    bool isHold = (server.hasArg("h") && server.arg("h").toInt() != 0);
    onGesture(server.arg("w").toInt(), isHold);
    server.send(200, "text/plain", "OK");
  });

  server.on("/mood", []() { currentScreen = SCR_FACE; currentMood = (Mood)constrain((int)server.arg("m").toInt(), 0, (int)MOOD_CUSTOM); tempMoodEnd = 0; wakeUp(); server.send(200, "text/plain", "OK"); });
  server.on("/scr", []() {
    int v = constrain((int)server.arg("s").toInt(), 0, (int)SCR_WIFI);
    ScreenMode s = (ScreenMode)v;
    if (s == SCR_WEATHER) fetchWeatherNow = true;
    if (isGame(s)) startGame(s); else currentScreen = s;
    wakeUp(); server.send(200, "text/plain", "OK");
  });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(400); if (!Update.hasError()) ESP.restart();
  }, []() {
    HTTPUpload& up = server.upload(); static unsigned long ld = 0;
    if (up.status == UPLOAD_FILE_START) { netPaused = true; if (stateDirty) saveNow(); if (displayOff) wakeUp(); drawProgress(5, "Starting OTA..."); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (up.status == UPLOAD_FILE_WRITE) {
      Update.write(up.buf, up.currentSize);
      if (millis() - ld > 180) { ld = millis(); char b[24]; sprintf(b, "Flashing %d KB...", (int)(up.totalSize / 1024)); drawProgress(constrain((int)((up.totalSize / 1024) * 100 / 1200), 8, 96), b); }
    } else if (up.status == UPLOAD_FILE_END) { drawProgress(Update.end(true) ? 100 : 0, Update.hasError() ? "OTA Failed!" : "Rebooting..."); if (Update.hasError()) netPaused = false; }
  });

  server.on("/aithink", []() { currentScreen = SCR_FACE; currentMood = MOOD_THINK; tempMoodEnd = millis() + 15000; wakeUp(); toast("Thinking...", 15000); server.send(200, "text/plain", "OK"); });
  server.on("/ai", HTTP_ANY, []() {
    String mStr = server.arg("m"), text = server.arg("t"), shapes = server.arg("shapes"), acts = server.arg("acts");
    aiReply = text; currentScreen = SCR_FACE; currentMood = parseMoodName(mStr); tempMoodEnd = 0; wakeUp();
    toastEnd = 0;                                    // clear the "Thinking..." toast
    queueScreen(SCR_AI);
    if (shapes.length() > 0) {
      parseCustomShapes(shapes);
      if (customShapeCount > 0) { currentMood = MOOD_CUSTOM; pendingAt = 0; toast(text.substring(0, 20), 2400); }
    }
    if (acts.length() > 0) executeAiActionPipe(acts);
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
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ================= SETUP & MAIN LOOP =================
void setup() {
  Serial.begin(115200);
  pinMode(IR_PIN, INPUT);
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.setBusClock(400000);
  u8g2.begin();
  for (int i = 0; i < 16; i++) matY[i] = random(-30, 64);

  netMutex = xSemaphoreCreateMutex();
  loadAll();
  applyContrast();
  playBootAnimation();

  WiFi.persistent(true); WiFi.setHostname("riddhi"); WiFi.mode(WIFI_STA); WiFi.begin();
  for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++) { drawProgress((i * 100) / 24, "Connecting Wi-Fi..."); delay(250); }

  if (WiFi.status() == WL_CONNECTED) {
    isAP = false; activeIP = WiFi.localIP().toString();
    WiFi.setSleep(false);                       // snappier web UI + live preview
    MDNS.begin("riddhi");
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  } else {
    isAP = true; WiFi.disconnect(false); WiFi.setAutoReconnect(false); delay(150);
    WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID, AP_PASS, 1); activeIP = WiFi.softAPIP().toString();
  }

  setupWeb();
  xTaskCreatePinnedToCore(core0Task, "Core0AI", 12288, NULL, 1, NULL, 0);   // TLS needs a bigger stack

  u8g2.clearBuffer(); drawWifi(); u8g2.sendBuffer(); delay(2000);
  lastAct = millis(); lastSaveMs = millis(); lastCareDecay = millis(); lastNudgeMs = millis();
  toast("Riddhi v7.0 ready!", 1800);
}

void loop() {
  server.handleClient();
  pollIR();
  checkCalendarSurprises();
  applyContrast();

  unsigned long now = millis();
  static unsigned long lastFrame = 0;

  if (triggerGhOta) { triggerGhOta = false; runGitHubOTA(); }

  if (alertPending && !isGame(currentScreen)) {
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      alertPending = false;
      currentScreen = SCR_FACE;
      currentMood = parseMoodName(alertMoodStr);
      tempMoodEnd = now + 5000;
      wakeUp();
      aiReply = alertTitle + ": " + alertMsg;
      toast(alertMsg.substring(0, 22), 3500);
      queueScreen(SCR_AI, 1800);
      xSemaphoreGive(netMutex);
    }
  }

  if (brainHasUpdate && !isGame(currentScreen)) {
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      brainHasUpdate = false;
      currentMood = parseMoodName(brainMoodStr);
      tempMoodEnd = now + 8000;
      wakeUp();
      if (brainToastStr.length() > 0) toast(brainToastStr.substring(0, 22), 4000);
      if (brainBannerT.length() > 0) { bannerTop = brainBannerT.substring(0, 16); bannerSub = brainBannerS.substring(0, 20); }
      if (brainScrStr == "TASKS") currentScreen = SCR_TASKS;
      else if (brainScrStr == "BANNER") currentScreen = SCR_BANNER;
      else currentScreen = SCR_FACE;
      xSemaphoreGive(netMutex);
    }
  }

  if (nudgeOn && !isGame(currentScreen) && (now - lastNudgeMs >= (unsigned long)nudgeMin * 60000UL)) {
    lastNudgeMs = now;
    nudgeIsWater = !nudgeIsWater;
    currentScreen = SCR_NUDGE;
    wakeUp();
  }

  if (now - lastCareDecay > CARE_DECAY_MS) {
    lastCareDecay = now;
    if (careLevel > 0) { careLevel = max(0, careLevel - 5); stateDirty = true; }
  }

  if (pomoRun) {
    unsigned long el = now - pomoTick; pomoTick = now;
    if (pomoLeft > el) pomoLeft -= el;
    else {
      pomoLeft = 0; wakeUp();
      currentScreen = SCR_FACE; currentMood = MOOD_HYPED; tempMoodEnd = now + 4000;
      if (!pomoBreak) {                                   // focus done -> start break
        pomoSessions++; boostCare(20);
        pomoBreak = true; pomoTotal = (pomoSessions % 4 == 0 ? 15UL : 5UL) * 60000UL;
        pomoLeft = pomoTotal; pomoRun = true; pomoTick = now;
        toast("Focus done! Break", 3000);
      } else {                                            // break done -> wait for next focus
        pomoBreak = false; pomoTotal = (unsigned long)POMO_PRESETS[pomoPreset] * 60000UL;
        pomoLeft = pomoTotal; pomoRun = false;
        toast("Break over! 1x:Start", 3000);
      }
    }
  }

  if (pendingAt > 0 && now >= pendingAt) { pendingAt = 0; currentScreen = pendingScreen; }
  if (stateDirty && (now - lastSaveMs > 30000UL)) saveNow();

  // Transient screens drift back to the face so the OLED isn't left on a static page
  if (!screenIsSticky(currentScreen)) {
    unsigned long lim = (currentScreen == SCR_MENU) ? MENU_IDLE_MS : SCREEN_IDLE_MS;
    if (now - lastAct > lim) { currentScreen = SCR_FACE; lastAct = now; }
  }

  // Panel fully off after long natural sleep (protects OLED); any wave/web action wakes it
  if (!displayOff && wasSleeping && currentScreen == SCR_FACE && currentMood == MOOD_SLEEP && now - lastAct > DISPLAY_OFF_MS) {
    displayOff = true; u8g2.setPowerSave(1);
  }
  if (displayOff) return;

  // Frame loop (~60 FPS)
  if (now - lastFrame >= 16) {
    lastFrame = now;
    if (currentScreen != lastDrawnScreen) { lastDrawnScreen = currentScreen; transStart = now; }

    u8g2.clearBuffer();
    switch (currentScreen) {
      case SCR_FACE:    drawFace(); break;
      case SCR_MENU:    drawMenu(); break;
      case SCR_TASKS:   drawTasks(); break;
      case SCR_CLOCK:   drawClock(); break;
      case SCR_WEATHER: drawWeather(); break;
      case SCR_POMO:    drawPomo(); break;
      case SCR_COUNTER: drawCounter(); break;
      case SCR_DICE:    drawDice(); break;
      case SCR_BREATH:  drawBreath(); break;
      case SCR_BANNER:  drawBanner(); break;
      case SCR_PC:      drawPC(); break;
      case SCR_DOODLE:  drawDoodle(); break;
      case SCR_NUDGE:   drawNudge(); break;
      case SCR_SPACE:   drawSpaceGame(); break;
      case SCR_FLAPPY:  drawFlappyGame(); break;
      case SCR_PONG:    drawPongGame(); break;
      case SCR_CATCH:   drawCatchGame(); break;
      case SCR_REACT:   drawReactGame(); break;
      case SCR_DINO:    drawDinoGame(); break;
      case SCR_RHYTHM:  drawRhythmGame(); break;
      case SCR_AI:      drawTopBar("AI / ALERT", "1x:Face"); u8g2.setFont(u8g2_font_6x10_tf); drawWrapped(aiReply, 26, 11, 3, 20); break;
      case SCR_WIFI:    drawWifi(); break;
    }
    if (irWaves > 0 && !isGame(currentScreen)) {
      for (int i = 0; i < irWaves && i < 4; i++) u8g2.drawDisc(122 - (i * 7), 5, 2);
    }
    drawToast(now);

    // iris-open transition when the screen changes (200 ms)
    if (now - transStart < 200) {
      int w = (int)((now - transStart) * 128UL / 200UL);
      u8g2.setDrawColor(0);
      u8g2.drawBox(0, 0, 64 - w / 2, 64);
      u8g2.drawBox(64 + w / 2, 0, 64 - w / 2, 64);
      u8g2.setDrawColor(1);
    }
    u8g2.sendBuffer();
  }
}
