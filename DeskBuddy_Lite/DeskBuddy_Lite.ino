// ================================================================
//  DeskBuddy Lite  —  by TECHNO ROBO X
//  A tiny desk robot: animated eyes, clock, date, weather, a live
//  Instagram follower counter, party mode, a focus timer and a
//  snoring sleep mode.
//
//  Hardware : Seeed XIAO ESP32-C3 + 1.3" SH1106 OLED (I2C) + TTP223 touch
//  Wiring   : OLED SDA -> D4, SCL -> D5, VCC -> 3V3, GND -> GND
//             TTP223 I/O -> D1, VCC -> 3V3, GND -> GND
//  Libraries: U8g2 (olikraus), ArduinoJson (v6 or v7)
//  Board    : XIAO_ESP32C3  (ESP32 core by Espressif)
//
//  1) Put your Wi-Fi, OpenWeatherMap key and Instagram token in secrets.h
//  2) Upload.
//
//  Touch pad:  tap         = next screen (eyes > time > date > weather > Instagram)
//              hold 1.5 s  = PARTY MODE  (30 s)
//              hold 3 s    = FOCUS TIMER (25 min)   tap = stop
//  Left alone for 2 minutes it falls asleep and snores. Tap to wake it up.
//
//  Full build video: https://youtu.be/4eB-Sj1Lqx8
//  This is the free "Lite" version. Use it, learn from it, have fun!
// ================================================================
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include "secrets.h"

// ---------------- pins ----------------
#define OLED_SDA   6      // D4
#define OLED_SCL   7      // D5
#define TOUCH_PIN  3      // D1  (TTP223 output, HIGH when touched)
#define BUZZER_PIN 21     // D6  (optional active buzzer — set USE_BUZZER 0 if you have none)
#define USE_BUZZER 1

// ---------------- timings ----------------
#define SLIDE_MS          6000UL            // each info screen shows for 6 s, then the next one
#define SLEEP_AFTER_MS    (2UL * 60UL * 1000UL)   // untouched for 2 min -> falls asleep
#define PARTY_MS          30000UL           // party lasts 30 s
#define FOCUS_MINUTES     25
#define HOLD_PARTY_MS     1500UL
#define HOLD_FOCUS_MS     3000UL
#define WEATHER_EVERY_MS  (10UL * 60UL * 1000UL)
#define INSTA_EVERY_MS    (15UL * 60UL * 1000UL)

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// ---------------- data ----------------
float  weatherTemp = NAN;
String weatherText = "";
long   igFollowers = -1;
String igUser = "", igError = "";
String igToken = IG_TOKEN;
String igUserId = "", igPageToken = "";
unsigned long lastWeather = 0, lastInsta = 0;

enum Screen { EYES, TIME_S, DATE_S, WEATHER_S, INSTA_S, SCREEN_COUNT };
int screen = EYES;
unsigned long screenSince = 0;

// ================================================================
//  HTTPS helper
// ================================================================
int httpsGet(const String &url, String &body) {
  WiFiClientSecure client;
  client.setInsecure();                 // simple: no certificate check
  HTTPClient http;
  body = "";
  if (!http.begin(client, url)) return -1;
  http.setTimeout(10000);
  int code = http.GET();
  if (code > 0) body = http.getString();
  http.end();
  return code;
}

// ================================================================
//  Weather — OpenWeatherMap "current weather" API
// ================================================================
void fetchWeather() {
  lastWeather = millis();
  if (strlen(OWM_API_KEY) < 10) { weatherText = "Add API key"; return; }
  String body;
  int code = httpsGet(String("https://api.openweathermap.org/data/2.5/weather?q=") + OWM_CITY +
                      "&units=metric&appid=" + OWM_API_KEY, body);
  DynamicJsonDocument doc(2048);
  if (code != 200 || deserializeJson(doc, body)) {
    Serial.printf("[weather] failed, HTTP %d\n", code);
    weatherText = (code == 401) ? "Bad API key" : "No data";
    return;
  }
  weatherTemp = doc["main"]["temp"] | NAN;
  weatherText = String((const char *)(doc["weather"][0]["main"] | ""));
  Serial.printf("[weather] %.1f C %s\n", weatherTemp, weatherText.c_str());
}

// ================================================================
//  Instagram follower counter (Meta Graph API)
//
//  Works with two kinds of token (see README):
//   "IGAA..."  Instagram Login  -> your OWN professional account
//   "EAA..."   Facebook Login   -> any Business/Creator account you name in
//                                  IG_TARGET, using "Business Discovery"
// ================================================================
String igErrorText(JsonDocument &doc, int code) {   // works with ArduinoJson v6 and v7
  if (!doc["error"].is<JsonObject>()) return code > 0 ? "HTTP " + String(code) : "Network error";
  int ec = doc["error"]["code"] | 0;
  if (ec == 190) return "Token expired";
  if (ec == 110 || (doc["error"]["error_subcode"] | 0) == 2207013) return "Not a Business/Creator acct";
  if (ec == 10 || ec == 200) return "Permission missing";
  if (ec == 4 || ec == 17 || ec == 32) return "Rate limited";
  return "Error " + String(ec);
}

// (A) Instagram Login token: read your own account
bool instaOwnAccount() {
  String body;
  int code = httpsGet("https://graph.instagram.com/me?fields=username,followers_count&access_token=" + igToken, body);
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, body) || code != 200) { igError = igErrorText(doc, code); return false; }
  igFollowers = doc["followers_count"] | -1L;
  igUser = String((const char *)(doc["username"] | ""));
  return igFollowers >= 0;
}

// (B) Facebook Login token: find your Instagram id through your Facebook Page...
bool instaFindMyId() {
  String body;
  int code = httpsGet("https://graph.facebook.com/me/accounts?fields=access_token,instagram_business_account"
                      "&access_token=" + igToken, body);
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, body) || code != 200) { igError = igErrorText(doc, code); return false; }
  for (JsonObject page : doc["data"].as<JsonArray>()) {
    const char *id = page["instagram_business_account"]["id"] | "";
    if (strlen(id)) {
      igUserId = id;
      igPageToken = String((const char *)(page["access_token"] | ""));
      return true;
    }
  }
  igError = "Link Instagram to a FB Page";
  return false;
}

// ...then ask "Business Discovery" for any Business/Creator account's followers
bool instaBusinessDiscovery() {
  if (igUserId.isEmpty() && !instaFindMyId()) return false;
  String target = IG_TARGET;
  String fields = target.length()
      ? "business_discovery.username%28" + target + "%29%7Busername%2Cfollowers_count%7D"   // (  ) { , }
      : String("username%2Cfollowers_count");
  String token = igPageToken.length() ? igPageToken : igToken;
  String body;
  int code = httpsGet("https://graph.facebook.com/" + igUserId + "?fields=" + fields + "&access_token=" + token, body);
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, body) || code != 200) { igError = igErrorText(doc, code); return false; }
  JsonObject src = target.length() ? doc["business_discovery"].as<JsonObject>() : doc.as<JsonObject>();
  if (src.isNull()) { igError = "Not a Business/Creator acct"; return false; }
  igFollowers = src["followers_count"] | -1L;
  igUser = String((const char *)(src["username"] | ""));
  return igFollowers >= 0;
}

void fetchInstagram() {
  lastInsta = millis();
  if (igToken.length() < 20) { igError = "Add IG token"; return; }
  bool ok = igToken.startsWith("EA") ? instaBusinessDiscovery() : instaOwnAccount();
  if (ok) { igError = ""; Serial.printf("[insta] @%s  %ld followers\n", igUser.c_str(), igFollowers); }
  else    Serial.printf("[insta] %s\n", igError.c_str());
}

// ================================================================
//  Eyes
// ================================================================
float lookX = 0, lookY = 0, fromX = 0, fromY = 0, toX = 0, toY = 0;
unsigned long moveStart = 0, nextLook = 2000, nextBlink = 3000, blinkStart = 0;

void drawEyes(unsigned long now) {
  if (now > nextLook) {                                       // glance somewhere new
    fromX = lookX; fromY = lookY;
    bool home = (toX != 0 || toY != 0) && random(100) < 60;
    toX = home ? 0 : random(-4, 5); toY = home ? 0 : random(-3, 4);
    moveStart = now; nextLook = now + 2000 + random(2500);
  }
  float k = constrain((now - moveStart) / 600.0f, 0.0f, 1.0f);
  float e = 0.5f - 0.5f * cosf(k * PI);                       // smooth ease
  lookX = fromX + (toX - fromX) * e; lookY = fromY + (toY - fromY) * e;

  if (now > nextBlink && !blinkStart) { blinkStart = now; nextBlink = now + 2500 + random(3500); }
  float close = 0;
  if (blinkStart) {
    unsigned long b = now - blinkStart;
    if (b < 80) close = b / 80.0f; else if (b < 200) close = 1 - (b - 80) / 120.0f; else blinkStart = 0;
  }

  for (int i = 0; i < 2; i++) {
    int cx = (i ? 84 : 44), cy = 30, r = 15;
    oled.drawDisc(cx, cy, r);
    oled.setDrawColor(0);
    oled.drawDisc(cx + (int)lookX, cy + (int)lookY, 5);       // pupil
    oled.setDrawColor(1);
    oled.drawDisc(cx + (int)lookX - 2, cy + (int)lookY - 2, 1);  // catch-light
    if (close > 0.02f) {                                      // eyelid
      oled.setDrawColor(0);
      oled.drawBox(cx - r - 1, cy - r - 1, r * 2 + 3, (int)(close * (r * 2 + 2)));
      oled.setDrawColor(1);
    }
  }
}

// ================================================================
//  Info screens
// ================================================================
void centered(const char *s, int y) { oled.drawStr((128 - oled.getStrWidth(s)) / 2, y, s); }

void drawTime() {
  struct tm t; char buf[16];
  if (!getLocalTime(&t, 10)) { oled.setFont(u8g2_font_6x10_tf); centered("Syncing time...", 36); return; }
  oled.setFont(u8g2_font_logisoso32_tn);
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  centered(buf, 46);
  oled.drawBox(14, 56, (t.tm_sec * 100) / 59, 3);             // seconds bar
}

void drawDate() {
  struct tm t; char buf[24];
  static const char *DAYS[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  static const char *MON[]  = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  if (!getLocalTime(&t, 10)) { oled.setFont(u8g2_font_6x10_tf); centered("Syncing time...", 36); return; }
  oled.setFont(u8g2_font_helvB12_tf); centered(DAYS[t.tm_wday], 26);
  snprintf(buf, sizeof(buf), "%d %s %d", t.tm_mday, MON[t.tm_mon], t.tm_year + 1900);
  oled.setFont(u8g2_font_7x13_tf);    centered(buf, 48);
}

void drawWeather() {
  char buf[16];
  oled.setFont(u8g2_font_6x10_tf); centered(OWM_CITY, 12);
  if (isnan(weatherTemp)) { centered(weatherText.length() ? weatherText.c_str() : "Loading...", 38); return; }
  oled.setFont(u8g2_font_logisoso24_tf);
  snprintf(buf, sizeof(buf), "%.0f%cC", weatherTemp, 176);
  centered(buf, 44);
  oled.setFont(u8g2_font_6x10_tf); centered(weatherText.c_str(), 60);
}

void drawInsta() {
  char buf[24];
  oled.drawRFrame(4, 4, 18, 18, 5); oled.drawCircle(13, 13, 4); oled.drawPixel(18, 8);   // little camera icon
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(28, 16, igUser.length() ? ("@" + igUser).c_str() : "Instagram");
  if (igFollowers < 0) { centered(igError.length() ? igError.c_str() : "Loading...", 42); return; }
  oled.setFont(u8g2_font_logisoso24_tn);
  snprintf(buf, sizeof(buf), "%ld", igFollowers);
  centered(buf, 50);
  oled.setFont(u8g2_font_5x7_tf); centered("followers", 62);
}

// ================================================================
//  Modes: party, focus, sleep
// ================================================================
enum Mode { NORMAL, SLEEPING, PARTY, FOCUS, FOCUS_DONE };
int mode = NORMAL;
unsigned long modeSince = 0, lastTouch = 0, beepOff = 0;

void beep(int ms) {
#if USE_BUZZER
  digitalWrite(BUZZER_PIN, HIGH); beepOff = millis() + ms;
#endif
}
void setMode(int m) { mode = m; modeSince = millis(); lastTouch = millis(); }   // a mode change counts as activity

// Sleeping face: closed eyes that breathe + floating Z's
void drawSleep(unsigned long now) {
  int breathe = (now / 1500) % 2;                            // 1-pixel breathing
  for (int i = 0; i < 2; i++) {
    int cx = (i ? 84 : 44), cy = 34 + breathe;
    for (int t = 0; t < 3; t++) {                            // thick closed-eye curve
      oled.drawLine(cx - 13, cy - 2 + t, cx - 6, cy + 3 + t);
      oled.drawLine(cx - 6, cy + 3 + t, cx + 6, cy + 3 + t);
      oled.drawLine(cx + 6, cy + 3 + t, cx + 13, cy - 2 + t);
    }
  }
  oled.drawCircle(64, 52 + breathe, 2 + (now / 750) % 2);    // little snoring mouth
  for (int z = 0; z < 3; z++) {                              // Z's float up and fade
    float ph = fmodf(now / 2400.0f + z / 3.0f, 1.0f);
    int zx = 102 + (int)(ph * 12), zy = 44 - (int)(ph * 28);    // rises from y 44 to 16, stays on screen
    if (ph < 0.35f)      oled.setFont(u8g2_font_5x7_tf);
    else if (ph < 0.7f)  oled.setFont(u8g2_font_6x10_tf);
    else                 oled.setFont(u8g2_font_helvB10_tf);
    oled.drawStr(zx, zy, ph < 0.7f ? "z" : "Z");
  }
}

// Party: happy eyes, a bouncing party hat and falling confetti
void drawParty(unsigned long now) {
  int bounce = (int)(fabsf(sinf(now / 180.0f)) * 3);
  for (int i = 0; i < 2; i++) {                              // happy ^ ^ eyes
    int cx = (i ? 84 : 44), cy = 40 - bounce;
    oled.drawDisc(cx, cy, 14);
    oled.setDrawColor(0); oled.drawDisc(cx, cy + 8, 14); oled.setDrawColor(1);
  }
  int hx = 64 + (int)(sinf(now / 300.0f) * 3), hy = 22 - bounce;   // party hat, wobbling
  oled.drawTriangle(hx - 12, hy, hx + 12, hy, hx, hy - 18);
  oled.setDrawColor(0);                                      // polka dots
  oled.drawDisc(hx - 4, hy - 4, 1); oled.drawDisc(hx + 4, hy - 6, 1); oled.drawDisc(hx, hy - 11, 1); oled.drawPixel(hx + 7, hy - 2);
  oled.setDrawColor(1);
  oled.drawDisc(hx, hy - 19, 2);                             // pom-pom
  for (int c = 0; c < 18; c++) {                             // confetti
    uint32_t h = c * 2654435761u;
    int x = (h >> 8) % 128;
    int y = (int)((now / (8 + (h % 7)) + (h >> 16)) % 70) - 4;
    if ((h & 1) == 0) oled.drawBox(x, y, 2, 2); else oled.drawPixel(x, y);
  }
  oled.setFont(u8g2_font_6x10_tf); centered("PARTY!", 63);
}

// Focus timer: big countdown + progress bar
void drawFocus(unsigned long now) {
  unsigned long total = FOCUS_MINUTES * 60000UL, el = now - modeSince;
  unsigned long left = el < total ? total - el : 0;
  char buf[8];
  oled.setFont(u8g2_font_6x10_tf); centered("FOCUS", 10);
  oled.setFont(u8g2_font_logisoso28_tn);
  snprintf(buf, sizeof(buf), "%02lu:%02lu", left / 60000UL, (left / 1000UL) % 60UL);
  centered(buf, 46);
  oled.drawFrame(10, 54, 108, 7);
  oled.drawBox(12, 56, (int)(104UL * el / total > 104 ? 104 : 104UL * el / total), 3);
}

void drawFocusDone() {
  oled.setFont(u8g2_font_helvB12_tf); centered("Well done!", 28);
  oled.setFont(u8g2_font_6x10_tf);    centered("Take a 5 min break", 48);
}

// Hold progress bar (shows what a hold will do)
void drawHoldBar(unsigned long held) {
  if (held < 400) return;
  const char *label = held < HOLD_PARTY_MS ? "Party..." : (held < HOLD_FOCUS_MS ? "Party! (hold: Focus)" : "Focus!");
  unsigned long goal = held < HOLD_PARTY_MS ? HOLD_PARTY_MS : HOLD_FOCUS_MS;
  oled.setDrawColor(0); oled.drawBox(0, 50, 128, 14); oled.setDrawColor(1);
  oled.drawFrame(0, 50, 128, 14);
  oled.drawBox(2, 52, (int)(124 * (held > goal ? goal : held) / goal), 3);
  oled.setFont(u8g2_font_5x7_tf); oled.drawStr(4, 62, label);
}

// ================================================================
//  Touch: tap / hold
// ================================================================
bool touchDown = false;
unsigned long downAt = 0;

void onTap(unsigned long now) {
  if (mode == SLEEPING || mode == PARTY || mode == FOCUS || mode == FOCUS_DONE) {   // wake up / stop
    setMode(NORMAL); screen = EYES; screenSince = now; beep(15); return;
  }
  screen = (screen + 1) % SCREEN_COUNT; screenSince = now; beep(10);
}

void handleTouch(unsigned long now) {
  bool down = digitalRead(TOUCH_PIN) == HIGH;
  if (down && !touchDown) { touchDown = true; downAt = now; }
  if (!down && touchDown) {
    touchDown = false;
    unsigned long held = now - downAt;
    lastTouch = now;
    if (held < 40) return;                                   // electrical blip
    if (held >= HOLD_FOCUS_MS && mode == NORMAL) { setMode(FOCUS); beep(120); Serial.println("[mode] focus"); }
    else if (held >= HOLD_PARTY_MS && mode == NORMAL) { setMode(PARTY); beep(60); Serial.println("[mode] party"); }
    else onTap(now);
  }
}

// ================================================================
void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT);
#if USE_BUZZER
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
#endif
  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin();
  oled.clearBuffer(); oled.setFont(u8g2_font_6x10_tf);
  centered("DeskBuddy Lite", 28); centered("connecting Wi-Fi...", 44);
  oled.sendBuffer();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  Serial.println(WiFi.status() == WL_CONNECTED ? "[wifi] connected" : "[wifi] NOT connected - check secrets.h");

  configTime(UTC_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com");
  randomSeed(esp_random());
  if (WiFi.status() == WL_CONNECTED) { fetchWeather(); fetchInstagram(); }
  lastTouch = millis();
}

void loop() {
  unsigned long now = millis();
  handleTouch(now);
  if (beepOff && now > beepOff) { digitalWrite(BUZZER_PIN, LOW); beepOff = 0; }

  // modes that end on their own
  if (mode == PARTY && now - modeSince > PARTY_MS) setMode(NORMAL);
  if (mode == FOCUS && now - modeSince >= FOCUS_MINUTES * 60000UL) { setMode(FOCUS_DONE); beep(400); }
  if (mode == FOCUS_DONE && now - modeSince > 8000) setMode(NORMAL);
  if (mode == NORMAL && !touchDown && now - lastTouch > SLEEP_AFTER_MS) { setMode(SLEEPING); screen = EYES; }

  // refresh data now and then (only on the eyes, so a screen never freezes in front of you)
  if (mode == NORMAL && screen == EYES && WiFi.status() == WL_CONNECTED) {
    if (now - lastWeather > WEATHER_EVERY_MS) fetchWeather();
    else if (now - lastInsta > INSTA_EVERY_MS) fetchInstagram();
  }
  static unsigned long lastRetry = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastRetry > 10000) { lastRetry = now; WiFi.reconnect(); }

  // info screens move on by themselves and end back on the eyes
  if (mode == NORMAL && screen != EYES && now - screenSince > SLIDE_MS) {
    screen = (screen + 1) % SCREEN_COUNT; screenSince = now;
  }

  oled.clearBuffer();
  switch (mode) {
    case SLEEPING:   drawSleep(now);     break;
    case PARTY:      drawParty(now);     break;
    case FOCUS:      drawFocus(now);     break;
    case FOCUS_DONE: drawFocusDone();    break;
    default:
      switch (screen) {
        case EYES:      drawEyes(now);  break;
        case TIME_S:    drawTime();     break;
        case DATE_S:    drawDate();     break;
        case WEATHER_S: drawWeather();  break;
        case INSTA_S:   drawInsta();    break;
      }
  }
  if (touchDown && mode == NORMAL) drawHoldBar(now - downAt);
  oled.sendBuffer();
  delay(20);
}
