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

#define IR_PIN 25
#define SDA_PIN 21
#define SCL_PIN 22

const char* AP_SSID = "Riddhi-OS";
const char* AP_PASS = "12345678";

// Madanapalle, AP coordinates for Open-Meteo
const char* WEATHER_LAT = "13.5503";
const char* WEATHER_LON = "78.5029";
#define GMT_OFFSET_SEC 19800            // IST (UTC +5:30)
#define DROWSY_TIMEOUT_MS 35000UL       // Droopy eyes + yawn at 35s
#define SLEEP_TIMEOUT_MS  50000UL       // Deep sleep at 50s
#define WEATHER_INTERVAL_MS 900000UL    // 15 mins
#define CARE_DECAY_MS 1800000UL         // -5% Care every 30 mins idle
// =====================================================================

WebServer server(80);
Preferences prefs;
SemaphoreHandle_t netMutex = NULL;

enum ScreenMode {
  SCR_FACE = 0, SCR_MENU, SCR_TASKS, SCR_CLOCK, SCR_WEATHER,
  SCR_POMO, SCR_COUNTER, SCR_DICE, SCR_BREATH, SCR_BANNER,
  SCR_SPACE, SCR_FLAPPY, SCR_PONG, SCR_CATCH,
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

// Quick IR Menu (15 items)
const char* menuItems[] = {
  "Riddhi Face", "Sticky Tasks", "Digital Clock", "Live Weather",
  "Focus Timer", "Tally Counter", "Dice & Oracle", "4-7-8 Breath",
  "Desk Banner", "Game: Space Def", "Game: Flappy Orb", "Game: Solo Pong",
  "Game: Orb Catch", "AI Console", "Wi-Fi & Link"
};
const int MENU_COUNT = 15;
int menuIndex = 0;
float menuScrollY = 15.0f;

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

// Care / Boredom System
int careLevel = 90;
unsigned long lastCareDecay = 0;

// Sticky-Note Task Carousel (5 persistent tasks)
String tasks[5] = {
  "Drink water", "Study 45 mins", "Code Riddhi", "Evening walk", "Skincare"
};
int taskDoneMask = 0;
int taskIdx = 0;

// Holiday & Time
int autoHoliday = 0;
int manualHoliday = -1;
int currentHour = 12, currentMin = 0;

// Persistent Data
int tallyCount = 0;
String tallyLabel = "COUNT";
int hiSpace = 0, hiFlappy = 0, hiPong = 0, hiCatch = 0;
String ghRepo = "";
bool stateDirty = false;
unsigned long lastSaveMs = 0;

// Autonomous AI Brain State (Shared with Core 0)
String geminiKey = "";
String geminiModel = "gemini-3.8-flash";
bool autoBrainOn = false;
int brainIntervalMin = 20;
volatile bool triggerBrainNow = false;
volatile bool brainHasUpdate = false;
String brainMoodStr = "", brainToastStr = "", brainBannerT = "", brainBannerS = "", brainScrStr = "";
String lastBrainLog = "Autonomous brain idle.";

// Desk Banner, AI, Pomodoro
String bannerTop = "FOCUS MODE";
String bannerSub = "Do Not Disturb";
String aiReply = "Ready! Speak or type any multi-step command.";
ScreenMode pendingScreen = SCR_FACE;
unsigned long pendingAt = 0;
String toastMsg = "";
unsigned long toastEnd = 0;

bool pomoRun = false, pomoBreak = false;
unsigned long pomoTotal = 25UL * 60000UL, pomoLeft = 25UL * 60000UL, pomoTick = 0;

// Dice & Oracle
int diceVal = 6;
bool oracleYes = true;
unsigned long diceRollUntil = 0;

// Background Weather (Core 0)
volatile bool netPaused = false, triggerGhOta = false, fetchWeatherNow = false, weatherReady = false;
float wTemp = 26.0f, wWind = 0;
int wHum = 0, wCode = 0;

// 4 Mini-Games State
enum GState { G_INTRO = 0, G_RUN, G_DEAD };
GState gState = G_INTRO;
unsigned long gDeadAt = 0;
int shipLane = 1, astLane = 0, laserX = -10, laserLane = 1, scoreSpace = 0;
float astX = 132, astSpeed = 3.2f;
float orbY = 32, orbVel = 0, pipeX = 132;
int pipeGap = 26, scoreFlappy = 0;
float ballX = 64, ballY = 32, ballVX = 2.6f, ballVY = 1.9f, padY = 26, padDir = 2.6f;
int scorePong = 0;
int catchSide = 0, dropSide = 0, scoreCatch = 0;
float dropY = 14, dropSpeed = 2.4f;

// IR Sensor Engine
bool irStable = false, irPrevRaw = false, irHeld = false;
unsigned long irDebMs = 0, irDownAt = 0, irUpAt = 0;
int irWaves = 0;
String activeIP = "192.168.4.1";
bool isAP = false;

// ================= STORAGE =================
void saveNow() {
  prefs.begin("riddhi62", false);
  prefs.putInt("care", careLevel);
  prefs.putBool("wFx", weatherFxOn);
  prefs.putInt("tMask", taskDoneMask);
  for (int i = 0; i < 5; i++) prefs.putString(("tk" + String(i)).c_str(), tasks[i]);
  prefs.putInt("tCnt", tallyCount);
  prefs.putString("tLbl", tallyLabel);
  prefs.putInt("hiS", hiSpace);
  prefs.putInt("hiF", hiFlappy);
  prefs.putInt("hiP", hiPong);
  prefs.putInt("hiC", hiCatch);
  prefs.putString("gh", ghRepo);
  prefs.putString("gKey", geminiKey);
  prefs.putString("gMod", geminiModel);
  prefs.putBool("aBrn", autoBrainOn);
  prefs.putInt("bInt", brainIntervalMin);
  prefs.end();
  stateDirty = false;
  lastSaveMs = millis();
}

void loadAll() {
  prefs.begin("riddhi62", true);
  careLevel    = prefs.getInt("care", 90);
  weatherFxOn  = prefs.getBool("wFx", true);
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
  ghRepo           = prefs.getString("gh", "");
  geminiKey        = prefs.getString("gKey", "");
  geminiModel      = prefs.getString("gMod", "gemini-3.8-flash");
  autoBrainOn      = prefs.getBool("aBrn", false);
  brainIntervalMin = prefs.getInt("bInt", 20);
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
    } else if (c == '"') {
      break;
    } else {
      res += c;
    }
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
bool isGame(ScreenMode s) { return (s == SCR_SPACE || s == SCR_FLAPPY || s == SCR_PONG || s == SCR_CATCH); }

void drawProgress(int pct, const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso18_tf);
  u8g2.drawStr((128 - u8g2.getStrWidth("RIDDHI 6.2")) / 2, 29, "RIDDHI 6.2");
  rFrame(16, 38, 96, 8, 4);
  if (pct > 3) rBox(18, 40, (92 * constrain(pct, 0, 100)) / 100, 4, 2);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr((128 - u8g2.getStrWidth(msg)) / 2, 58, msg);
  u8g2.sendBuffer();
}

// ================= CYBER-HORIZON BOOT ANIMATION =================
void playBootAnimation() {
  for (int w = 4; w <= 120; w += 12) {
    u8g2.clearBuffer();
    u8g2.drawHLine(64 - w / 2, 32, w);
    u8g2.drawDisc(64 - w / 2, 32, 2); u8g2.drawDisc(64 + w / 2, 32, 2);
    u8g2.sendBuffer(); delay(22);
  }
  for (int f = 1; f <= 8; f++) {
    u8g2.clearBuffer();
    int d = f * 5;
    u8g2.drawPixel(64 - d, 32 - d / 2); u8g2.drawPixel(64 + d, 32 - d / 2);
    u8g2.drawPixel(64 - d, 32 + d / 2); u8g2.drawPixel(64 + d, 32 + d / 2);
    u8g2.setFont(u8g2_font_logisoso16_tf);
    u8g2.drawStr((128 - u8g2.getStrWidth("RIDDHI")) / 2, 36, "RIDDHI");
    u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(36, 50, "AI OS v6.2");
    u8g2.sendBuffer(); delay(55);
  }
  for (int h = 2; h <= 24; h += 3) {
    u8g2.clearBuffer();
    rBox(28, 32 - h / 2, 26, h, 5); rBox(74, 32 - h / 2, 26, h, 5);
    if (h > 12) {
      u8g2.setDrawColor(0);
      u8g2.drawBox(33, 32 - h / 2 + 5, 5, 5); u8g2.drawBox(79, 32 - h / 2 + 5, 5, 5);
      u8g2.setDrawColor(1);
    }
    u8g2.sendBuffer(); delay(28);
  }
}

// ================= CORE 0: WEATHER + AUTONOMOUS AI BRAIN =================
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

  String body = "{\"generationConfig\":{\"responseMimeType\":\"application/json\",\"maxOutputTokens\":120},"
                "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"" + prompt + "\"}]}]}";

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(9000);
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + modCopy + ":generateContent?key=" + keyCopy;

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    if (code == HTTP_CODE_OK) {
      String resp = http.getString();
      String inner = extractJsonStr(resp, "text");
      if (inner.length() > 5) {
        String m = extractJsonStr(inner, "mood");
        String ts = extractJsonStr(inner, "toast");
        String sc = extractJsonStr(inner, "scr");
        String bt = extractJsonStr(inner, "bt");
        String bs = extractJsonStr(inner, "bs");
        if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          brainMoodStr = m; brainToastStr = ts; brainScrStr = sc;
          brainBannerT = bt; brainBannerS = bs;
          lastBrainLog = "[" + String(hrCopy) + ":" + (mnCopy < 10 ? "0" : "") + String(mnCopy) + "] " + m + ": " + ts;
          brainHasUpdate = true;
          xSemaphoreGive(netMutex);
        }
      }
    } else {
      if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        lastBrainLog = "Brain HTTP " + String(code);
        xSemaphoreGive(netMutex);
      }
    }
  }
  http.end();
}

void core0Task(void* pv) {
  unsigned long lastW = 0, lastB = 0;
  for (;;) {
    if (!netPaused && WiFi.status() == WL_CONNECTED) {
      unsigned long now = millis();
      if (fetchWeatherNow || lastW == 0 || (now - lastW > WEATHER_INTERVAL_MS)) {
        fetchWeatherNow = false; bgWeather(); lastW = millis();
      }
      unsigned long bIntMs = (unsigned long)max(2, brainIntervalMin) * 60000UL;
      if (triggerBrainNow || (autoBrainOn && geminiKey.length() >= 10 && (now - lastB > bIntMs))) {
        triggerBrainNow = false;
        bgAutoBrain();
        lastB = millis();
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
  if (millis() - lastChk < 30000UL && lastChk != 0) return;
  lastChk = millis();
  struct tm ti;
  if (!getLocalTime(&ti, 5)) return;
  currentHour = ti.tm_hour; currentMin = ti.tm_min;
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

// ================= AUTONOMOUS FACE ENGINE =================
void drawFace() {
  unsigned long now = millis(); float t = now * 0.003f;
  unsigned long idle = now - lastAct;

  if (tempMoodEnd > 0 && now > tempMoodEnd) { tempMoodEnd = 0; currentMood = (careLevel < 35) ? MOOD_SICK : MOOD_CHILL; }
  if (currentMood == MOOD_CHILL || currentMood == MOOD_SICK || currentMood == MOOD_DROWSY) {
    if (idle > SLEEP_TIMEOUT_MS) { currentMood = MOOD_SLEEP; wasSleeping = true; }
    else if (idle > DROWSY_TIMEOUT_MS) currentMood = MOOD_DROWSY;
    else if (currentMood == MOOD_CHILL && careLevel < 35) currentMood = MOOD_SICK;
  }

  int shiverX = 0; drawWeatherAndHolidayFX(now, shiverX);

  eyeX += (eyeTX - eyeX) * 0.22f; eyeY += (eyeTY - eyeY) * 0.22f;
  eyeLH += (eyeTLH - eyeLH) * 0.42f; eyeRH += (eyeTRH - eyeRH) * 0.42f;
  int br = (int)round(sin(t) * 1.4f), ox = (int)round(eyeX) + shiverX, oy = (int)round(eyeY) + br;

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
    case MOOD_SLEEP: { u8g2.drawHLine(28, 35 + br, 26); u8g2.drawHLine(74, 35 + br, 26); int ph = (now / 120) % 18; u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(96 + ph / 4, 26 - ph / 2, "z"); u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(106 + ((ph + 6) % 12) / 3, 20 - ((ph + 6) % 16) / 2, "Z"); break; }
    case MOOD_THINK: rBox(34, 20, 22, 18, 5); rBox(78, 20, 22, 18, 5); for (int i = 0; i < 3; i++) u8g2.drawDisc(54 + i * 10, 10 - (i == (now / 200) % 3 ? 3 : 0), 2); break;
    case MOOD_CUSTOM: drawCustomFace(br); break;
  }

  if (WiFi.status() == WL_CONNECTED) { u8g2.drawPixel(3, 5); u8g2.drawCircle(3, 5, 2, U8G2_DRAW_UPPER_RIGHT); }
  if (autoBrainOn) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(10, 7, "AI"); }
  if (pomoRun && (now / 500) % 2 == 0) u8g2.drawDisc(24, 4, 2);

  if (now < toastEnd && toastMsg.length() > 0) {
    u8g2.setFont(u8g2_font_5x7_tf);
    int tw = min(124, u8g2.getStrWidth(toastMsg.c_str()) + 12), tx = (128 - tw) / 2;
    u8g2.setDrawColor(0); rBox(tx - 1, 51, tw + 2, 12, 5); u8g2.setDrawColor(1);
    rBox(tx, 52, tw, 10, 4); u8g2.setDrawColor(0); u8g2.drawStr(tx + 6, 60, toastMsg.c_str()); u8g2.setDrawColor(1);
  }
}

// ================= SCREENS & GAMES =================
void drawTasks() {
  char r[12]; sprintf(r, "%d/5", taskIdx + 1); drawTopBar("STICKY TASK", r);
  bool done = (taskDoneMask & (1 << taskIdx));
  rFrame(4, 16, 120, 34, 5); u8g2.drawFrame(10, 27, 11, 11); if (done) u8g2.drawBox(12, 29, 7, 7);
  u8g2.setFont(u8g2_font_6x12_tf); String t = tasks[taskIdx].substring(0, 15);
  u8g2.drawStr(26, 37, t.c_str()); if (done) u8g2.drawHLine(26, 33, u8g2.getStrWidth(t.c_str()));
  u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(8, 61, "1x:Check   2x:Next Task");
}

void startGame(ScreenMode s) {
  currentScreen = s; gState = G_INTRO; lastAct = millis();
  shipLane = 1; astX = 136; astLane = random(0, 3); astSpeed = 3.2f; laserX = -10; scoreSpace = 0;
  orbY = 32; orbVel = 0; pipeX = 136; pipeGap = random(18, 36); scoreFlappy = 0;
  ballX = 64; ballY = 32; ballVX = 2.6f; ballVY = 1.9f; padY = 26; scorePong = 0;
  catchSide = 0; dropY = 14; dropSide = random(0, 2); dropSpeed = 2.3f; scoreCatch = 0;
}

void gameTap() {
  unsigned long now = millis(); lastAct = now;
  if (gState == G_INTRO || (gState == G_DEAD && now - gDeadAt > 450)) { startGame(currentScreen); gState = G_RUN; return; }
  if (gState != G_RUN) return;
  if (currentScreen == SCR_SPACE) { shipLane = (shipLane + 1) % 3; if (laserX < 0) { laserX = 22; laserLane = shipLane; } }
  else if (currentScreen == SCR_FLAPPY) orbVel = -4.6f;
  else if (currentScreen == SCR_PONG) padDir = -padDir;
  else if (currentScreen == SCR_CATCH) catchSide = 1 - catchSide;
}

void drawSpaceGame() {
  int ly[3] = {22, 38, 54};
  if (gState == G_RUN) {
    astX -= astSpeed;
    if (laserX >= 0) { laserX += 6; if (laserX > 128) laserX = -10; else if (laserLane == astLane && laserX >= astX) { scoreSpace += 2; laserX = -10; astX = 134; astLane = random(0, 3); if (astSpeed < 6.5f) astSpeed += 0.15f; } }
    if (astX < -10) { astX = 134; astLane = random(0, 3); scoreSpace++; }
    if (astLane == shipLane && astX < 22 && astX + 10 > 10) { gState = G_DEAD; gDeadAt = millis(); if (scoreSpace > hiSpace) { hiSpace = scoreSpace; stateDirty = true; } boostCare(8); }
  }
  char b[16]; sprintf(b, "HI:%02d SC:%02d", hiSpace, scoreSpace); drawTopBar("SPACE DEF", b);
  int sy = ly[shipLane]; u8g2.drawTriangle(10, sy - 5, 10, sy + 5, 22, sy);
  if (laserX >= 0) u8g2.drawHLine(laserX, ly[laserLane], 6);
  if (gState != G_INTRO) u8g2.drawDisc((int)astX, ly[astLane], 5);
  if (gState == G_INTRO) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(14, 38, "Wave:Dodge+Fire Hold:Exit"); }
  else if (gState == G_DEAD) { u8g2.setDrawColor(0); rBox(26, 20, 76, 24, 4); u8g2.setDrawColor(1); rFrame(26, 20, 76, 24, 4); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(38, 36, "BLASTED!"); }
}

void drawFlappyGame() {
  if (gState == G_RUN) {
    orbVel += 0.42f; orbY += orbVel; pipeX -= 2.6f;
    if (pipeX < -12) { pipeX = 132; pipeGap = random(18, 36); scoreFlappy++; }
    if (orbY < 15 || orbY > 60 || (30 > pipeX && 22 < pipeX + 12 && (orbY - 4 < pipeGap || orbY + 4 > pipeGap + 22))) {
      gState = G_DEAD; gDeadAt = millis(); if (scoreFlappy > hiFlappy) { hiFlappy = scoreFlappy; stateDirty = true; } boostCare(8);
    }
  }
  char b[16]; sprintf(b, "HI:%02d SC:%02d", hiFlappy, scoreFlappy); drawTopBar("FLAPPY ORB", b);
  u8g2.drawDisc(26, (int)orbY, 4);
  if (gState != G_INTRO) { u8g2.drawBox((int)pipeX, 13, 12, pipeGap - 13); u8g2.drawBox((int)pipeX, pipeGap + 22, 12, 42); }
  if (gState == G_INTRO) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(18, 38, "Wave:Flap  Hold:Exit"); }
  else if (gState == G_DEAD) { u8g2.setDrawColor(0); rBox(26, 20, 76, 24, 4); u8g2.setDrawColor(1); rFrame(26, 20, 76, 24, 4); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(38, 36, "BONKED!"); }
}

void drawPongGame() {
  if (gState == G_RUN) {
    padY = constrain(padY + padDir, 14.0f, 46.0f); if (padY <= 14.0f || padY >= 46.0f) padDir = -padDir;
    ballX += ballVX; ballY += ballVY;
    if (ballY <= 15 || ballY >= 61) ballVY = -ballVY;
    if (ballX >= 123) ballVX = -fabs(ballVX);
    if (ballX <= 14 && ballX >= 9 && ballY >= padY - 2 && ballY <= padY + 18) { ballVX = fabs(ballVX) * 1.05f; scorePong++; }
    else if (ballX < 4) { gState = G_DEAD; gDeadAt = millis(); if (scorePong > hiPong) { hiPong = scorePong; stateDirty = true; } boostCare(8); }
  }
  char b[16]; sprintf(b, "HI:%02d SC:%02d", hiPong, scorePong); drawTopBar("SOLO PONG", b);
  u8g2.drawBox(9, (int)padY, 3, 16); u8g2.drawVLine(125, 13, 51);
  if (gState != G_INTRO) u8g2.drawDisc((int)ballX, (int)ballY, 2);
  if (gState == G_INTRO) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(14, 38, "Wave:Flip Dir  Hold:Exit"); }
  else if (gState == G_DEAD) { u8g2.setDrawColor(0); rBox(26, 20, 76, 24, 4); u8g2.setDrawColor(1); rFrame(26, 20, 76, 24, 4); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(38, 36, "MISSED!"); }
}

void drawCatchGame() {
  int cx[2] = {38, 90};
  if (gState == G_RUN) {
    dropY += dropSpeed;
    if (dropY >= 52) {
      if (dropSide == catchSide) { scoreCatch++; dropY = 15; dropSide = random(0, 2); if (dropSpeed < 5.5f) dropSpeed += 0.15f; }
      else { gState = G_DEAD; gDeadAt = millis(); if (scoreCatch > hiCatch) { hiCatch = scoreCatch; stateDirty = true; } boostCare(8); }
    }
  }
  char b[16]; sprintf(b, "HI:%02d SC:%02d", hiCatch, scoreCatch); drawTopBar("ORB CATCH", b);
  rBox(cx[catchSide] - 12, 55, 24, 5, 2);
  if (gState != G_INTRO) u8g2.drawDisc(cx[dropSide], (int)dropY, 4);
  if (gState == G_INTRO) { u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(16, 38, "Wave:Switch  Hold:Exit"); }
  else if (gState == G_DEAD) { u8g2.setDrawColor(0); rBox(26, 20, 76, 24, 4); u8g2.setDrawColor(1); rFrame(26, 20, 76, 24, 4); u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(38, 36, "DROPPED!"); }
}

void drawClock() {
  struct tm ti; drawTopBar(dimMode ? "NIGHT" : "CLOCK", "IST");
  if (getLocalTime(&ti, 10)) {
    char tb[12], db[24], ap[6]; strftime(tb, sizeof(tb), "%I:%M", &ti); strftime(ap, sizeof(ap), "%p", &ti); strftime(db, sizeof(db), "%a, %d %b", &ti);
    u8g2.setFont(u8g2_font_logisoso24_tn); u8g2.drawStr(14, 44, tb);
    u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(98, 34, ap);
    if (!dimMode) { u8g2.drawHLine(16, 49, 96); u8g2.drawStr(32, 61, db); }
  } else { u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(10, 38, "Syncing Wi-Fi time..."); }
}

void drawWeather() {
  drawTopBar("WEATHER", "1x:Sync");
  if (WiFi.status() != WL_CONNECTED) { u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(8, 38, "Connect Wi-Fi first"); return; }
  if (!weatherReady) { u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(14, 38, "Fetching weather..."); return; }
  float t = 0, w = 0; int h = 0;
  if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(10)) == pdTRUE) { t = wTemp; h = wHum; w = wWind; xSemaphoreGive(netMutex); }
  char tb[12]; sprintf(tb, "%.1fC", t); u8g2.setFont(u8g2_font_logisoso18_tf); u8g2.drawStr(24, 38, tb);
  char sb[28]; sprintf(sb, "RH:%d%%   Wind:%.0fkm/h", h, w); u8g2.drawHLine(0, 50, 128); u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(10, 61, sb);
}

void drawPomo() {
  char b[10], t[16]; sprintf(b, "%02d:%02d", (int)((pomoLeft / 1000) / 60), (int)((pomoLeft / 1000) % 60));
  sprintf(t, "%s %dM", pomoBreak ? "BREAK" : "FOCUS", (int)(pomoTotal / 60000UL));
  drawTopBar(t, pomoRun ? "RUN" : "PAUSE");
  u8g2.setFont(u8g2_font_logisoso22_tn); u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 43, b);
  int bw = pomoTotal > 0 ? (int)((116UL * pomoLeft) / pomoTotal) : 0;
  rFrame(4, 51, 120, 9, 4); if (bw > 2) rBox(6, 53, min(116, bw), 5, 2);
}

void drawCounter() {
  drawTopBar(tallyLabel.c_str(), "1x:+1");
  char b[16]; sprintf(b, "%d", tallyCount); u8g2.setFont(u8g2_font_logisoso24_tn);
  u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 46, b);
  u8g2.setFont(u8g2_font_5x7_tf); u8g2.drawStr(22, 60, "Hold IR: Reset to 0");
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
  unsigned long sec = (millis() / 1000) % 19; int r = 6; const char* lbl = "INHALE";
  if (sec < 4) { r = 6 + (int)((millis() % 19000) * 14 / 4000); lbl = "INHALE 4s"; }
  else if (sec < 11) { r = 20; lbl = "HOLD 7s"; }
  else { r = 20 - (int)(((millis() % 19000) - 11000) * 14 / 8000); lbl = "EXHALE 8s"; }
  u8g2.drawCircle(32, 38, constrain(r, 4, 20)); u8g2.drawDisc(32, 38, constrain(r - 4, 1, 16));
  u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(60, 41, lbl);
}

void drawBanner() {
  drawTopBar("DESK SIGN", "LIVE");
  u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr((128 - u8g2.getStrWidth(bannerTop.c_str())) / 2, 33, bannerTop.c_str());
  u8g2.drawHLine(16, 39, 96);
  u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr((128 - u8g2.getStrWidth(bannerSub.c_str())) / 2, 53, bannerSub.c_str());
}

void drawMenu() {
  drawTopBar("RIDDHI v6.2", "2x:Next");
  int st = (menuIndex >= 4) ? menuIndex - 3 : 0;
  float targetY = 15.0f + (menuIndex - st) * 12.0f;
  menuScrollY += (targetY - menuScrollY) * 0.35f;
  rBox(2, (int)round(menuScrollY), 118, 11, 4);
  u8g2.setFont(u8g2_font_6x10_tf);
  for (int i = 0; i < 4 && (st + i) < MENU_COUNT; i++) {
    int idx = st + i, y = 24 + i * 12;
    u8g2.setDrawColor(idx == menuIndex ? 0 : 1); u8g2.drawStr(8, y, menuItems[idx]);
  }
  u8g2.setDrawColor(1); u8g2.drawVLine(125, 15, 46); rBox(124, 15 + (menuIndex * 36) / (MENU_COUNT - 1), 3, 10, 1);
}

void drawWifi() {
  drawTopBar("CONNECT", "Hold:Reset");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(3, 25, isAP ? "WiFi: Riddhi-OS" : "http://riddhi.local");
  u8g2.drawStr(3, 38, isAP ? "Pass: 12345678" : "or IP address:");
  u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(3, 55, ("http://" + activeIP).c_str());
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
      pomoRun = true; pomoTick = millis(); pendingScreen = SCR_POMO;
    } else if (op == "BANNER") {
      int til = val.indexOf('~');
      bannerTop = (til >= 0 ? val.substring(0, til) : val).substring(0, 16);
      bannerSub = (til >= 0 ? val.substring(til + 1) : "AI Sign").substring(0, 20);
      pendingScreen = SCR_BANNER;
    } else if (op == "TASK_SET") {
      int til = val.indexOf('~');
      if (til > 0) {
        int idx = constrain((int)val.substring(0, til).toInt(), 0, 4);
        tasks[idx] = val.substring(til + 1).substring(0, 16);
        taskDoneMask &= ~(1 << idx); taskIdx = idx; stateDirty = true; pendingScreen = SCR_TASKS;
      }
    } else if (op == "TASK_TOG") {
      int idx = constrain((int)val.toInt(), 0, 4);
      taskDoneMask ^= (1 << idx); taskIdx = idx; stateDirty = true; boostCare(10); pendingScreen = SCR_TASKS;
    } else if (op == "TALLY") {
      tallyCount = (int)val.toInt(); stateDirty = true; pendingScreen = SCR_COUNTER;
    } else if (op == "DIM") {
      dimMode = (val.toInt() != 0); u8g2.setContrast(dimMode ? 5 : 255);
    } else if (op == "FLIP") {
      flippedDisplay = (val.toInt() != 0); u8g2.setFlipMode(flippedDisplay ? 1 : 0);
    } else if (op == "HOLIDAY") {
      manualHoliday = constrain((int)val.toInt(), -1, 5);
    } else if (op == "CHEER") {
      boostCare(25);
    } else if (op == "SCREEN") {
      val.toUpperCase();
      if (val == "FACE") pendingScreen = SCR_FACE;
      else if (val == "TASKS") pendingScreen = SCR_TASKS;
      else if (val == "CLOCK") pendingScreen = SCR_CLOCK;
      else if (val == "WEATHER") { fetchWeatherNow = true; pendingScreen = SCR_WEATHER; }
      else if (val == "POMO") pendingScreen = SCR_POMO;
      else if (val == "COUNTER") pendingScreen = SCR_COUNTER;
      else if (val == "DICE") { diceVal = random(1, 7); oracleYes = random(0, 2); diceRollUntil = millis() + 650; pendingScreen = SCR_DICE; }
      else if (val == "BREATH") pendingScreen = SCR_BREATH;
      else if (val == "BANNER") pendingScreen = SCR_BANNER;
      else if (val == "SPACE") { startGame(SCR_SPACE); pendingAt = 0; }
      else if (val == "FLAPPY") { startGame(SCR_FLAPPY); pendingAt = 0; }
      else if (val == "PONG") { startGame(SCR_PONG); pendingAt = 0; }
      else if (val == "CATCH") { startGame(SCR_CATCH); pendingAt = 0; }
    }
    st = bar + 1;
  }
}

// ================= IR GESTURES =================
void onGesture(int waves, bool held) {
  unsigned long now = millis(); lastAct = now; pendingAt = 0;
  if (wasSleeping && currentScreen == SCR_FACE) {
    wasSleeping = false; currentMood = MOOD_SHOCKED; tempMoodEnd = now + 700; boostCare(10); toast("Whoa! Awake!", 900); return;
  }
  if (waves >= 4) { currentScreen = SCR_FACE; currentMood = MOOD_CHILL; toast("Home", 800); return; }
  if (held) {
    if (isGame(currentScreen)) { currentScreen = SCR_FACE; gState = G_INTRO; toast("GG!", 1000); }
    else if (currentScreen == SCR_COUNTER) { tallyCount = 0; stateDirty = true; toast("Reset 0", 1000); }
    else if (currentScreen == SCR_POMO) { pomoRun = false; pomoBreak = !pomoBreak; pomoTotal = (pomoBreak ? 5UL : 25UL) * 60000UL; pomoLeft = pomoTotal; }
    else if (currentScreen == SCR_WIFI) { WiFi.disconnect(true, true); delay(500); ESP.restart(); }
    else if (currentScreen == SCR_FACE) { currentMood = MOOD_LOVE; tempMoodEnd = now + 2600; boostCare(25); toast("Cheered Up! <3", 1500); }
    return;
  }
  if (waves == 3) {
    if (currentScreen == SCR_MENU) {
      ScreenMode mapM[] = { SCR_FACE, SCR_TASKS, SCR_CLOCK, SCR_WEATHER, SCR_POMO, SCR_COUNTER, SCR_DICE, SCR_BREATH, SCR_BANNER, SCR_SPACE, SCR_FLAPPY, SCR_PONG, SCR_CATCH, SCR_AI, SCR_WIFI };
      ScreenMode chosen = mapM[menuIndex];
      if (isGame(chosen)) startGame(chosen); else currentScreen = chosen;
    } else currentScreen = SCR_MENU;
    return;
  }
  if (waves == 2) {
    if (currentScreen == SCR_MENU) menuIndex = (menuIndex + 1) % MENU_COUNT;
    else if (currentScreen == SCR_TASKS) taskIdx = (taskIdx + 1) % 5;
    else if (currentScreen == SCR_FACE) { currentMood = (Mood)(((int)currentMood + 1) % 9); tempMoodEnd = 0; }
    else currentScreen = SCR_MENU;
    return;
  }
  if (waves == 1) {
    if (isGame(currentScreen)) gameTap();
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
  if (now - irDebMs > 35 && raw != irStable) {
    irStable = raw;
    if (irStable) { irDownAt = now; irHeld = false; if (isGame(currentScreen)) gameTap(); }
    else if (!irHeld && !isGame(currentScreen)) { irWaves++; irUpAt = now; }
  }
  if (irStable && !irHeld && (now - irDownAt > 950)) { irHeld = true; irWaves = 0; onGesture(0, true); }
  if (!irStable && irWaves > 0 && (now - irUpAt > 520)) { int c = irWaves; irWaves = 0; onGesture(c, false); }
}

// ================= 4-TAB CYBER WEB UI (v6.2 + LIVE OLED PREVIEW) =================
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Riddhi AI">
<title>Riddhi OS v6.2</title><style>
:root{--bg:#090b10;--card:#121622;--btn:#1b2234;--cyan:#22d3ee;--amber:#fbbf24;--emerald:#34d399;--rose:#f43f5e;--purp:#a855f7;}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:var(--bg);color:#f1f5f9;margin:0;padding:16px 14px 86px;}
.hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;padding:0 2px;}
.hdr h2{margin:0;font-size:20px;color:#fff;}
.badge{font-size:11px;font-weight:700;padding:4px 10px;border-radius:99px;background:rgba(168,85,247,.16);color:#d8b4fe;border:1px solid rgba(168,85,247,.38);}
.pg{display:none;}.pg.on{display:block;animation:up .16s ease;}
@keyframes up{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.box{background:var(--card);border:1px solid rgba(255,255,255,.07);border-radius:18px;padding:14px;margin-bottom:12px;}
.ai-box{border-color:rgba(168,85,247,.38);}
.lbl{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:1px;color:#64748b;margin-bottom:10px;}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:8px;}.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;}
.row{display:flex;gap:7px;margin-bottom:8px;align-items:center;}
button{background:var(--btn);color:#e2e8f0;border:1px solid rgba(255,255,255,.07);border-radius:13px;padding:11px 8px;font-size:12.5px;font-weight:600;cursor:pointer;}
button:active{transform:scale(.96);filter:brightness(1.2);}
.cy{background:var(--cyan);color:#04131a;border:none;}
.am{background:var(--amber);color:#1c1102;border:none;}
.em{background:var(--emerald);color:#022217;border:none;}
.pu{background:linear-gradient(135deg,#7c3aed,#ec4899);color:#fff;border:none;}
.ro{background:rgba(244,63,94,.18);border-color:var(--rose);color:#fda4af;}
.done{background:rgba(52,211,153,.18);border-color:var(--emerald);color:#6ee7b7;text-decoration:line-through;}
input,select{width:100%;padding:10px;border-radius:12px;border:1px solid rgba(255,255,255,.1);background:#090b10;color:#fff;margin-bottom:8px;font-size:13px;}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px;}
.chip{background:rgba(168,85,247,.14);border:1px solid rgba(168,85,247,.35);color:#e9d5ff;padding:6px 10px;border-radius:99px;font-size:11px;font-weight:600;}
#aiOut,#bLog{background:#090b10;border:1px solid rgba(255,255,255,.08);border-radius:12px;padding:11px;min-height:44px;font-size:12.5px;margin-bottom:10px;line-height:1.4;}
.gitem{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;background:#090b10;border-radius:13px;margin-bottom:8px;border:1px solid rgba(255,255,255,.06);}
.gitem b{display:block;font-size:13px;}.gitem span{font-size:11px;color:#64748b;}
.sub{font-size:11px;color:#64748b;margin-top:6px;}
.sbar{width:100%;height:7px;background:#090b10;border-radius:6px;overflow:hidden;margin:6px 0 10px;}
.sfill{height:100%;background:linear-gradient(90deg,var(--cyan),var(--emerald));transition:width .25s;}
nav{position:fixed;bottom:0;left:0;right:0;background:rgba(9,11,16,.95);backdrop-filter:blur(10px);border-top:1px solid rgba(255,255,255,.08);display:flex;justify-content:space-around;padding:8px 4px 12px;}
.nb{background:none;border:none;color:#64748b;font-size:11px;display:flex;flex-direction:column;align-items:center;gap:3px;padding:4px 12px;border-radius:12px;}
.nb.on{color:var(--cyan);background:rgba(34,211,238,.12);}
.nb span{font-size:16px;}
#tst{position:fixed;top:14px;left:50%;transform:translateX(-50%);background:var(--cyan);color:#04131a;padding:6px 14px;border-radius:99px;font-size:12px;font-weight:700;display:none;z-index:99;}
</style></head><body>

<div id="tst">OK</div>
<div class="hdr"><h2>Riddhi OS</h2><span class="badge">v6.2 &bull; AI Brain</span></div>

<!-- LIVE INTERACTIVE OLED PREVIEW -->
<div class="box" style="text-align:center;padding:10px">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px">
    <span class="lbl" style="margin:0">Live OLED (Tap: 1x &bull; Hold: IR Hold)</span>
    <button id="pvBtn" class="cy" style="padding:4px 10px;font-size:11px;border-radius:99px" onclick="togPv()">LIVE: ON</button>
  </div>
  <canvas id="oledCv" width="128" height="64" style="width:100%;max-width:256px;aspect-ratio:2/1;background:#04070d;border:2px solid rgba(34,211,238,.45);border-radius:10px;image-rendering:pixelated;touch-action:none;user-select:none;-webkit-user-select:none;-webkit-touch-callout:none;cursor:pointer"></canvas>
</div>

<!-- TAB 1: AI AGENT & AUTONOMOUS BRAIN -->
<div id="t-ai" class="pg on">
  <div class="box ai-box">
    <div class="lbl">Voice &amp; Multi-Action AI Controller</div>
    <div id="aiOut">Try: "Start 45m focus timer, set sign to CODING / BUSY, and change task 1 to Revise notes"</div>
    <div class="chips">
      <button class="chip" onclick="qAI('Start 25 min timer and set sign to STUDYING / FOCUS')">Study Setup</button>
      <button class="chip" onclick="qAI('Draw a cool cyber visor face')">Cyber Visor</button>
      <button class="chip" onclick="qAI('Check off task 1 and cheer up')">Done Task 1</button>
      <button class="chip" onclick="qAI('Turn on night dim and show clock')">Night Mode</button>
    </div>
    <div class="row">
      <button id="micBtn" style="width:46px" onclick="startMic()">&#127908;</button>
      <input id="aiIn" style="margin:0;flex:1" placeholder="Speak or type command..." onkeydown="if(event.key==='Enter')runAI()">
      <button class="pu" style="width:58px" onclick="runAI()">Go</button>
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
      </select>
      <button class="cy" style="width:100%" onclick="saveK()">Save Key to Phone + ESP32</button>
    </div>
  </div>

  <div class="box">
    <div class="lbl">Autonomous Core 0 Brain (No Phone Needed)</div>
    <div id="bLog">Autonomous brain idle.</div>
    <div class="g2" style="margin-bottom:8px">
      <button id="brnBtn" onclick="cmd('/brain?tog=1','Brain Toggled!')">Auto Brain: OFF</button>
      <button class="am" onclick="cmd('/brain?now=1','Riddhi is thinking...')">Think Now</button>
    </div>
    <div class="row" style="margin:0">
      <span style="font-size:12px;color:#94a3b8">Think every:</span>
      <select id="bInt" style="margin:0;flex:1" onchange="cmd('/brain?int='+this.value,'Interval Saved!')">
        <option value="10">10 minutes</option>
        <option value="20" selected>20 minutes</option>
        <option value="30">30 minutes</option>
        <option value="60">60 minutes</option>
      </select>
    </div>
    <div class="sub">ESP32 checks time, weather, Care %, and unchecked tasks on Core 0 and acts on its own.</div>
  </div>
</div>

<!-- TAB 2: DESK -->
<div id="t-desk" class="pg">
  <div class="box">
    <div class="lbl">Mood &amp; Care Status (<span id="carePct">90%</span>)</div>
    <div class="sbar"><div id="careBar" class="sfill" style="width:90%"></div></div>
    <div class="g2">
      <button class="em" onclick="cmd('/cheer','Cheered Up!')">Cheer Up (+25%)</button>
      <button id="wFxBtn" onclick="cmd('/wfx','Weather FX Toggled')">Weather FX: ON</button>
    </div>
  </div>
  <div class="box">
    <div class="lbl">Sticky-Note Task Carousel (Tap to Check)</div>
    <div id="tList" style="display:flex;flex-direction:column;gap:6px;margin-bottom:10px"></div>
    <div class="row" style="margin:0">
      <select id="tSlot" style="width:66px;margin:0"><option value="0">#1</option><option value="1">#2</option><option value="2">#3</option><option value="3">#4</option><option value="4">#5</option></select>
      <input id="tTxt" style="margin:0;flex:1" maxlength="16" placeholder="Edit task text...">
      <button class="cy" style="width:60px" onclick="saveTask()">Set</button>
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
    </div>
    <div class="g3">
      <button onclick="cmd('/mood?m=0','Chill')">Chill</button>
      <button class="am" onclick="cmd('/mood?m=1','Hyped')">Hyped</button>
      <button onclick="cmd('/mood?m=5','Sus')">Sus</button>
      <button onclick="cmd('/mood?m=6','Smug')">Smug</button>
      <button onclick="cmd('/mood?m=8','Love')">Love</button>
      <button onclick="cmd('/mood?m=11','Sleep')">Sleep</button>
    </div>
  </div>
  <div class="box">
    <div class="lbl">Desk Sign &amp; Tally</div>
    <input id="bTop" maxlength="16" placeholder="Top line (e.g. FOCUS MODE)">
    <div class="row">
      <input id="bSub" style="margin:0;flex:1" maxlength="20" placeholder="Bottom line (e.g. Do Not Disturb)">
      <button class="cy" style="width:66px" onclick="setBanner()">Show</button>
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
    <div class="lbl">IR Mini-Games</div>
    <div class="gitem"><div><b>Space Defender</b><span>Wave to switch lanes &amp; blast asteroids</span></div><button class="em" style="width:66px" onclick="cmd('/scr?s=10','Space!')">Play</button></div>
    <div class="gitem"><div><b>Flappy Orb</b><span>Wave to flap through neon pillars</span></div><button class="em" style="width:66px" onclick="cmd('/scr?s=11','Flappy!')">Play</button></div>
    <div class="gitem"><div><b>Solo Pong</b><span>Wave to reverse paddle direction</span></div><button class="em" style="width:66px" onclick="cmd('/scr?s=12','Pong!')">Play</button></div>
    <div class="gitem" style="margin:0"><div><b>Orb Catch</b><span>Wave to toggle left/right catcher</span></div><button class="em" style="width:66px" onclick="cmd('/scr?s=13','Catch!')">Play</button></div>
  </div>
  <div class="box">
    <div class="lbl">Virtual IR Remote</div>
    <div class="g2">
      <button class="cy" onclick="cmd('/g?w=1','1x')">1x Action / Check</button>
      <button onclick="cmd('/g?w=2','2x')">2x Next</button>
      <button onclick="cmd('/g?w=3','3x')">3x Menu / OK</button>
      <button onclick="cmd('/g?w=4','4x')">4x Home</button>
    </div>
  </div>
</div>

<!-- TAB 4: SYSTEM -->
<div id="t-sys" class="pg">
  <div class="box">
    <div class="lbl">Holiday &amp; Birthday Preview</div>
    <select id="holSel" onchange="cmd('/holiday?h='+this.value,'Updated!')">
      <option value="-1">Auto (By Calendar Date)</option>
      <option value="1">Preview: Birthday Cake (Oct 4)</option>
      <option value="2">Preview: New Year Fireworks</option>
      <option value="3">Preview: Floating Hearts</option>
      <option value="4">Preview: Spooky Halloween</option>
      <option value="5">Preview: Christmas Snow</option>
      <option value="0">Off</option>
    </select>
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
    <div class="g3">
      <button class="cy" onclick="saveW()">Save</button>
      <button onclick="cmd('/flip','Flipped!')">Flip</button>
      <button onclick="cmd('/dim','Dimmed')">Dim</button>
    </div>
    <button class="ro" style="width:100%;margin-top:8px" onclick="cmd('/resetwifi','Resetting')">Reset Wi-Fi</button>
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
const k0=localStorage.getItem('rk')||''; document.getElementById('ak').value=k0;
const m0=localStorage.getItem('rm')||'';
if(m0==='gemini-3.8-flash'||m0==='gemini-3.5-flash-lite') document.getElementById('am').value=m0;
if(!k0) document.getElementById('kBox').style.display='block';

function tab(id,el){
  document.querySelectorAll('.pg').forEach(p=>p.classList.remove('on'));
  document.querySelectorAll('.nb').forEach(b=>b.classList.remove('on'));
  document.getElementById('t-'+id).classList.add('on'); el.classList.add('on');
  sync();
}
function tst(m){const t=document.getElementById('tst');t.innerText=m;t.style.display='block';setTimeout(()=>t.style.display='none',1300);}
function cmd(u,m){fetch(u);if(m)tst(m);setTimeout(sync,400);}
function togTTS(){ttsOn=!ttsOn;document.getElementById('ttsBtn').innerText='Voice Reply: '+(ttsOn?'ON':'OFF');}
function togKey(){const b=document.getElementById('kBox');b.style.display=b.style.display==='none'?'block':'none';}
function saveK(){
  const k=document.getElementById('ak').value.trim(), m=document.getElementById('am').value;
  localStorage.setItem('rk',k); localStorage.setItem('rm',m);
  document.getElementById('kBox').style.display='none';
  fetch('/cfg?akey='+encodeURIComponent(k)+'&amod='+encodeURIComponent(m));
  tst('Saved to Phone & ESP32!');
}
function saveTask(){
  const i=document.getElementById('tSlot').value, txt=document.getElementById('tTxt').value.trim();
  if(!txt) return; cmd('/task?i='+i+'&t='+encodeURIComponent(txt),'Task Saved!');
  document.getElementById('tTxt').value='';
}
function setBanner(){cmd('/banner?t='+encodeURIComponent(document.getElementById('bTop').value||'FOCUS')+'&s='+encodeURIComponent(document.getElementById('bSub').value||'Riddhi OS'),'Sign Updated!');}
function saveGh(){cmd('/cfg?gh='+encodeURIComponent(document.getElementById('ghIn').value.trim()),'Repo Saved!');}
function pullGh(){if(confirm('Download & flash latest Riddhi_OS.ino.bin from GitHub?')) cmd('/ghota','Updating...');}
function qAI(s){document.getElementById('aiIn').value=s;runAI();}

function startMic(){
  const SR=window.SpeechRecognition||window.webkitSpeechRecognition;
  if(!SR){tst('Use keyboard mic on iOS/HTTP');return;}
  const r=new SR(); r.lang='en-IN'; r.interimResults=false;
  const mb=document.getElementById('micBtn'); mb.style.background='#f43f5e';
  r.onresult=(e)=>{mb.style.background=''; document.getElementById('aiIn').value=e.results[0][0].transcript; runAI();};
  r.onerror=()=>{mb.style.background=''; tst('Mic blocked on HTTP - use keyboard mic');};
  r.onend=()=>{mb.style.background='';};
  r.start();
}

async function sync(){
  try{
    const r=await fetch('/status'); if(!r.ok) return;
    const j=await r.json();
    document.getElementById('cVal').innerText=j.cnt;
    document.getElementById('carePct').innerText=j.care+'%';
    document.getElementById('careBar').style.width=j.care+'%';
    document.getElementById('wFxBtn').innerText='Weather FX: '+(j.wfx?'ON':'OFF');
    document.getElementById('brnBtn').innerText='Auto Brain: '+(j.brn?'ON':'OFF');
    document.getElementById('brnBtn').className=j.brn?'em':'';
    document.getElementById('bInt').value=String(j.bint||20);
    document.getElementById('bLog').innerText=j.blog||'Autonomous brain idle.';
    if(!document.getElementById('ghIn').value) document.getElementById('ghIn').value=j.gh||'';
    const tl=document.getElementById('tList'); tl.innerHTML='';
    (j.tasks||[]).forEach((t,i)=>{
      const b=document.createElement('button');
      const dn=(j.tmask&(1<<i))!==0;
      b.className=dn?'done':''; b.style.textAlign='left';
      b.innerText=(dn?'[x] ':'[ ] ')+(i+1)+'. '+t;
      b.onclick=()=>cmd('/task?tog='+i,'Toggled!');
      tl.appendChild(b);
    });
  }catch(e){}
}
sync();

async function scanW(){
  const st=document.getElementById('wSt'), box=document.getElementById('wList');
  st.innerText='Scanning...'; box.innerHTML='';
  const lines=(await(await fetch('/scan')).text()).split('\n').filter(x=>x.trim());
  st.innerText=lines.length?'Tap network:':'None found';
  lines.forEach(n=>{const b=document.createElement('button');b.className='chip';b.innerText=n;b.onclick=()=>document.getElementById('ssid').value=n;box.appendChild(b);});
}
function saveW(){const s=document.getElementById('ssid').value.trim();if(!s)return;document.getElementById('wSt').innerText='Rebooting...';fetch('/savewifi?s='+encodeURIComponent(s)+'&p='+encodeURIComponent(document.getElementById('pass').value));}

function upOTA(){
  const f=document.getElementById('otaF').files[0], st=document.getElementById('oSt'), bar=document.getElementById('oBar'), fill=document.getElementById('oFill');
  if(!f){st.innerText='Pick a .bin file first';return;}
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
  else if(op==='CHEER') o.push('CHEER:1');
  else if(op==='SCREEN') o.push('SCREEN:'+(a.val||'FACE').toUpperCase());
}return o.join('|');}

async function runAI(){
  const k=localStorage.getItem('rk')||'', m=localStorage.getItem('rm')||'gemini-3.8-flash', p=document.getElementById('aiIn').value.trim(), out=document.getElementById('aiOut');
  if(!k){document.getElementById('kBox').style.display='block';out.innerText='Add Gemini API Key first.';return;}
  if(!p)return; document.getElementById('aiIn').value=''; out.innerText='Executing...'; fetch('/aithink');
  const sys='You are Riddhi v6.2, an AI-controlled ESP32 desk robot (128x64 OLED). Return ONLY JSON: {"mood":"CHILL|HYPED|ZEN|GRUMPY|SHOCKED|SUS|SMUG|DIZZY|LOVE|SLEEP","reply":"short ASCII <54 chars","shapes":[],"actions":[]}. Supported actions array items: {"op":"POMO","arg":25}, {"op":"BANNER","a":"TOP","b":"SUB"}, {"op":"TASK_SET","idx":0,"text":"Task"}, {"op":"TASK_TOG","idx":0}, {"op":"TALLY","val":10}, {"op":"DIM","val":1}, {"op":"FLIP","val":1}, {"op":"HOLIDAY","val":1}, {"op":"CHEER"}, {"op":"SCREEN","val":"FACE|TASKS|CLOCK|WEATHER|POMO|COUNTER|DICE|BREATH|BANNER|SPACE|FLAPPY|PONG|CATCH"}. For custom faces put primitives (rbox,disc,circle,line,arc) in shapes[].';
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
    const b=new URLSearchParams();
    b.set('m',(j.mood||'HYPED').toUpperCase());
    b.set('t',(j.reply||'').replace(/[^\x20-\x7E]/g,'').slice(0,60));
    const sh=serShapes(j.shapes); if(sh) b.set('shapes',sh);
    const ac=serActs(j.actions); if(ac) b.set('acts',ac);
    await fetch('/ai',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b.toString()});
    setTimeout(sync,350);
    if(ttsOn&&'speechSynthesis'in window){window.speechSynthesis.cancel();window.speechSynthesis.speak(new SpeechSynthesisUtterance(j.reply||''));}
  }catch(e){out.innerText='Err: '+e.message; fetch('/mood?m=7');}
}

// ================= LIVE INTERACTIVE OLED PREVIEW =================
let pvOn=true, pvBusy=false, cvTimer=null, cvHeld=false;
const cv=document.getElementById('oledCv'), ctx=cv.getContext('2d'), imgD=ctx.createImageData(128,64);

function togPv(){
  pvOn=!pvOn;
  const b=document.getElementById('pvBtn');
  b.innerText='LIVE: '+(pvOn?'ON':'OFF');
  b.className=pvOn?'cy':'';
}

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
setInterval(drawOled, 350);

cv.addEventListener('contextmenu', e=>e.preventDefault());
cv.addEventListener('pointerdown', e=>{
  e.preventDefault();
  cvHeld=false;
  cv.style.borderColor='#fbbf24';
  cvTimer=setTimeout(()=>{
    cvHeld=true;
    cv.style.borderColor='#34d399';
    fetch('/g?h=1');
    tst('IR Hold');
    setTimeout(drawOled, 120);
  }, 650);
});
function endCv(isUp){
  clearTimeout(cvTimer);
  cv.style.borderColor='rgba(34,211,238,.45)';
  if(isUp && !cvHeld){
    fetch('/g?w=1');
    setTimeout(drawOled, 80);
  }
}
cv.addEventListener('pointerup', ()=>endCv(true));
cv.addEventListener('pointercancel', ()=>endCv(false));
cv.addEventListener('pointerleave', ()=>endCv(false));
</script></body></html>
)rawliteral";

void setupWeb() {
  server.on("/", []() { server.send_P(200, "text/html", HTML_PAGE); });

  server.on("/oled", []() {
    server.setContentLength(1024);
    server.send(200, "application/octet-stream", "");
    server.client().write(u8g2.getBufferPtr(), 1024);
  });

  server.on("/status", []() {
    String tArr = "[";
    for (int i = 0; i < 5; i++) { if (i > 0) tArr += ","; tArr += "\"" + jsonEscape(tasks[i]) + "\""; }
    tArr += "]";
    String logCopy = "";
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(50)) == pdTRUE) { logCopy = lastBrainLog; xSemaphoreGive(netMutex); }
    String j = "{\"cnt\":" + String(tallyCount) + ",\"care\":" + String(careLevel) +
               ",\"wfx\":" + String(weatherFxOn ? 1 : 0) + ",\"tmask\":" + String(taskDoneMask) +
               ",\"tasks\":" + tArr + ",\"gh\":\"" + jsonEscape(ghRepo) +
               "\",\"brn\":" + String(autoBrainOn ? 1 : 0) + ",\"bint\":" + String(brainIntervalMin) +
               ",\"blog\":\"" + jsonEscape(logCopy) + "\"}";
    server.send(200, "application/json", j);
  });

  server.on("/brain", []() {
    if (server.hasArg("tog")) { autoBrainOn = !autoBrainOn; saveNow(); }
    if (server.hasArg("int")) { brainIntervalMin = constrain((int)server.arg("int").toInt(), 2, 120); saveNow(); }
    if (server.hasArg("now")) {
      if (geminiKey.length() < 10) { toast("Save API Key first!", 1800); }
      else { triggerBrainNow = true; currentScreen = SCR_FACE; currentMood = MOOD_THINK; tempMoodEnd = millis() + 10000; toast("AI Thinking...", 4000); }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/cfg", []() {
    if (server.hasArg("gh")) ghRepo = server.arg("gh");
    if (server.hasArg("akey") || server.hasArg("amod")) {
      if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (server.hasArg("akey")) geminiKey = server.arg("akey");
        if (server.hasArg("amod")) geminiModel = server.arg("amod");
        xSemaphoreGive(netMutex);
      }
    }
    saveNow(); server.send(200, "text/plain", "OK");
  });

  server.on("/cheer", []() { boostCare(25); currentScreen = SCR_FACE; currentMood = MOOD_LOVE; tempMoodEnd = millis() + 2600; lastAct = millis(); wasSleeping = false; toast("Thanks! <3", 1500); server.send(200, "text/plain", "OK"); });
  server.on("/wfx", []() { weatherFxOn = !weatherFxOn; stateDirty = true; server.send(200, "text/plain", "OK"); });
  server.on("/holiday", []() { manualHoliday = server.arg("h").toInt(); currentScreen = SCR_FACE; lastAct = millis(); wasSleeping = false; server.send(200, "text/plain", "OK"); });

  server.on("/task", []() {
    if (server.hasArg("tog")) { int i = constrain((int)server.arg("tog").toInt(), 0, 4); taskDoneMask ^= (1 << i); taskIdx = i; stateDirty = true; boostCare(8); }
    else if (server.hasArg("i") && server.hasArg("t")) { int i = constrain((int)server.arg("i").toInt(), 0, 4); tasks[i] = server.arg("t").substring(0, 16); taskDoneMask &= ~(1 << i); taskIdx = i; stateDirty = true; }
    currentScreen = SCR_TASKS; lastAct = millis(); server.send(200, "text/plain", "OK");
  });

  server.on("/ghota", []() { server.send(200, "text/plain", "OK"); triggerGhOta = true; });
  server.on("/dim", []() { dimMode = !dimMode; u8g2.setContrast(dimMode ? 5 : 255); server.send(200, "text/plain", "OK"); });
  server.on("/flip", []() { flippedDisplay = !flippedDisplay; u8g2.setFlipMode(flippedDisplay ? 1 : 0); server.send(200, "text/plain", "OK"); });
  server.on("/cnt", []() { if (server.hasArg("inc")) tallyCount += server.arg("inc").toInt(); if (server.hasArg("reset")) tallyCount = 0; stateDirty = true; currentScreen = SCR_COUNTER; lastAct = millis(); server.send(200, "text/plain", "OK"); });
  server.on("/dice", []() { currentScreen = SCR_DICE; diceVal = random(1, 7); oracleYes = random(0, 2); diceRollUntil = millis() + 650; lastAct = millis(); server.send(200, "text/plain", "OK"); });
  server.on("/banner", []() { if (server.hasArg("t")) bannerTop = server.arg("t").substring(0, 16); if (server.hasArg("s")) bannerSub = server.arg("s").substring(0, 20); currentScreen = SCR_BANNER; lastAct = millis(); server.send(200, "text/plain", "OK"); });

  server.on("/g", []() {
    bool isHold = (server.hasArg("h") && server.arg("h").toInt() != 0);
    onGesture(server.arg("w").toInt(), isHold);
    server.send(200, "text/plain", "OK");
  });

  server.on("/mood", []() { currentScreen = SCR_FACE; currentMood = (Mood)server.arg("m").toInt(); tempMoodEnd = 0; lastAct = millis(); wasSleeping = false; server.send(200, "text/plain", "OK"); });
  server.on("/scr", []() { ScreenMode s = (ScreenMode)server.arg("s").toInt(); if (isGame(s)) startGame(s); else currentScreen = s; lastAct = millis(); server.send(200, "text/plain", "OK"); });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(400); if (!Update.hasError()) ESP.restart();
  }, []() {
    HTTPUpload& up = server.upload(); static unsigned long ld = 0;
    if (up.status == UPLOAD_FILE_START) { netPaused = true; if (stateDirty) saveNow(); drawProgress(5, "Starting OTA..."); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (up.status == UPLOAD_FILE_WRITE) {
      Update.write(up.buf, up.currentSize);
      if (millis() - ld > 180) { ld = millis(); char b[24]; sprintf(b, "Flashing %d KB...", up.totalSize / 1024); drawProgress(constrain((int)((up.totalSize / 1024) * 100 / 1150), 8, 96), b); }
    } else if (up.status == UPLOAD_FILE_END) { drawProgress(Update.end(true) ? 100 : 0, Update.hasError() ? "OTA Failed!" : "Rebooting..."); if (Update.hasError()) netPaused = false; }
  });

  server.on("/aithink", []() { currentScreen = SCR_FACE; currentMood = MOOD_THINK; tempMoodEnd = millis() + 15000; toast("Thinking...", 15000); server.send(200, "text/plain", "OK"); });
  server.on("/ai", HTTP_ANY, []() {
    String mStr = server.arg("m"), text = server.arg("t"), shapes = server.arg("shapes"), acts = server.arg("acts");
    aiReply = text; currentScreen = SCR_FACE; currentMood = parseMoodName(mStr); tempMoodEnd = 0; lastAct = millis(); wasSleeping = false;
    pendingScreen = SCR_AI; pendingAt = millis() + 1500;
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
  server.begin();
}

// ================= SETUP & LOOP =================
void setup() {
  Serial.begin(115200);
  pinMode(IR_PIN, INPUT);
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  netMutex = xSemaphoreCreateMutex();
  loadAll();
  playBootAnimation();

  WiFi.persistent(true); WiFi.mode(WIFI_STA); WiFi.begin();
  for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++) { drawProgress((i * 100) / 24, "Connecting Wi-Fi..."); delay(250); }

  if (WiFi.status() == WL_CONNECTED) {
    isAP = false; activeIP = WiFi.localIP().toString();
    MDNS.begin("riddhi");
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  } else {
    isAP = true; WiFi.disconnect(false); WiFi.setAutoReconnect(false); delay(150);
    WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID, AP_PASS, 1); activeIP = WiFi.softAPIP().toString();
  }

  setupWeb();
  xTaskCreatePinnedToCore(core0Task, "Core0AI", 8192, NULL, 1, NULL, 0);

  u8g2.clearBuffer(); drawWifi(); u8g2.sendBuffer(); delay(2400);
  lastAct = millis(); lastSaveMs = millis(); lastCareDecay = millis();
  toast("Riddhi v6.2 AI!", 1800);
}

void loop() {
  server.handleClient();
  pollIR();
  checkCalendarSurprises();

  unsigned long now = millis();
  static unsigned long lastFrame = 0;

  if (triggerGhOta) { triggerGhOta = false; runGitHubOTA(); }

  if (brainHasUpdate && !isGame(currentScreen)) {
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      brainHasUpdate = false;
      currentMood = parseMoodName(brainMoodStr);
      tempMoodEnd = now + 8000;
      lastAct = now; wasSleeping = false;
      if (brainToastStr.length() > 0) toast(brainToastStr.substring(0, 22), 4000);
      if (brainBannerT.length() > 0) { bannerTop = brainBannerT.substring(0, 16); bannerSub = brainBannerS.substring(0, 20); }
      if (brainScrStr == "TASKS") currentScreen = SCR_TASKS;
      else if (brainScrStr == "BANNER") currentScreen = SCR_BANNER;
      else currentScreen = SCR_FACE;
      xSemaphoreGive(netMutex);
    }
  }

  if (now - lastCareDecay > CARE_DECAY_MS) {
    lastCareDecay = now;
    if (careLevel > 0) { careLevel = max(0, careLevel - 5); stateDirty = true; }
  }

  if (pomoRun) {
    unsigned long el = now - pomoTick; pomoTick = now;
    if (pomoLeft > el) pomoLeft -= el;
    else { pomoLeft = 0; pomoRun = false; currentScreen = SCR_FACE; currentMood = MOOD_HYPED; tempMoodEnd = now + 4000; boostCare(20); toast("Timer Done!", 2500); }
  }

  if (pendingAt > 0 && now >= pendingAt) { pendingAt = 0; currentScreen = pendingScreen; }
  if (stateDirty && (now - lastSaveMs > 30000UL)) saveNow();

  if (now - lastFrame >= 33) {
    lastFrame = now;
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
      case SCR_SPACE:   drawSpaceGame(); break;
      case SCR_FLAPPY:  drawFlappyGame(); break;
      case SCR_PONG:    drawPongGame(); break;
      case SCR_CATCH:   drawCatchGame(); break;
      case SCR_AI:      drawTopBar("AI REPLY", "1x:Face"); u8g2.setFont(u8g2_font_6x10_tf); drawWrapped(aiReply, 26, 11, 3, 20); break;
      case SCR_WIFI:    drawWifi(); break;
    }
    if (irWaves > 0 && !isGame(currentScreen)) {
      for (int i = 0; i < irWaves && i < 4; i++) u8g2.drawDisc(122 - (i * 7), 5, 2);
    }
    u8g2.sendBuffer();
  }
}
