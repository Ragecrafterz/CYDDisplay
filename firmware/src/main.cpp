// CYD now-playing + forecast screen with a bongo cat.
// A companion app on the laptop feeds it over USB; without it the board falls back to WiFi.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include "state.h"
#include "ui.h"

// The touch controller has its own SPI bus on the CYD
static const int XPT_IRQ = 36, XPT_MOSI = 32, XPT_MISO = 39, XPT_CLK = 25, XPT_CS = 33;
static const int LED_PINS[] = {4, 16, 17};  // RGB LED, active low
static const int BL_CHANNEL = 0;  // PWM channel for the backlight

TFT_eSPI tft;
static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen ts(XPT_CS, XPT_IRQ);

Config cfg;
NowPlaying np;
Weather wx;
ArtSlot art;
SysStats sys;
SemaphoreHandle_t stateLock;
volatile uint32_t lastUsbMs = 0;
volatile uint32_t lastKeyMs = 0;
volatile uint16_t pendingTaps = 0;
volatile bool wifiUp = false;
volatile bool netReconfigure = false;
volatile uint8_t netCmd = CMD_NONE;
volatile bool artWanted = false;
volatile uint32_t holdPlayUntil = 0;
volatile bool holdPlaying = false;

static SemaphoreHandle_t serialLock;
static SemaphoreHandle_t nvsLock;
static Preferences prefs;

// ---------- config ----------

static void configLoad() {
  if (!prefs.begin("cyd", true)) return;  // first boot: nothing saved yet
  prefs.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
  prefs.getString("pass", cfg.pass, sizeof(cfg.pass));
  prefs.getString("city", cfg.city, sizeof(cfg.city));
  cfg.lat = prefs.getFloat("lat", NAN);
  cfg.lon = prefs.getFloat("lon", NAN);
  cfg.fahrenheit = prefs.getBool("f", false);
  cfg.h12 = prefs.getBool("h12", false);
  cfg.invert = prefs.getBool("inv", true);
  cfg.touchFlipX = prefs.getBool("tfx", false);
  cfg.touchFlipY = prefs.getBool("tfy", false);
  cfg.rot = prefs.getUChar("rot", 1) & 3;
  cfg.catX = prefs.getShort("cx", -1);
  cfg.catY = prefs.getShort("cy", -1);
  cfg.catStyle = prefs.getUChar("cat", 0) & 3;
  cfg.catShape = prefs.getUChar("shp", 0) % 3;
  cfg.albumAccent = prefs.getBool("alb", true);
  cfg.brightness = max<uint8_t>(10, prefs.getUChar("bri", 255));
  if (prefs.getBytesLength("theme") == sizeof(cfg.theme)) prefs.getBytes("theme", cfg.theme, sizeof(cfg.theme));
  prefs.getString("spid", cfg.spClientId, sizeof(cfg.spClientId));
  prefs.getString("sprt", cfg.spRefresh, sizeof(cfg.spRefresh));
  prefs.end();
}

void configSave() {
  xSemaphoreTake(nvsLock, portMAX_DELAY);
  prefs.begin("cyd", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("city", cfg.city);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putBool("f", cfg.fahrenheit);
  prefs.putBool("h12", cfg.h12);
  prefs.putBool("inv", cfg.invert);
  prefs.putBool("tfx", cfg.touchFlipX);
  prefs.putBool("tfy", cfg.touchFlipY);
  prefs.putUChar("rot", cfg.rot);
  prefs.putShort("cx", cfg.catX);
  prefs.putShort("cy", cfg.catY);
  prefs.putUChar("cat", cfg.catStyle);
  prefs.putUChar("shp", cfg.catShape);
  prefs.putBool("alb", cfg.albumAccent);
  prefs.putUChar("bri", cfg.brightness);
  prefs.putBytes("theme", cfg.theme, sizeof(cfg.theme));
  prefs.putString("spid", cfg.spClientId);
  prefs.putString("sprt", cfg.spRefresh);
  prefs.end();
  xSemaphoreGive(nvsLock);
}

void applyBrightness() { ledcWrite(BL_CHANNEL, cfg.brightness); }

// ---------- clock ----------
// Local time (already offset from UTC) counted forward from the last sync.
// The laptop's time wins over NTP while it keeps sending it.

static int64_t clkBase = 0;
static uint32_t clkAt = 0;
static Source clkSrc = SRC_NONE;

void clockSet(int64_t localEpoch, Source src) {
  lockState();
  bool usbFresh = clkSrc == SRC_USB && millis() - clkAt < 120000;
  if (!(src == SRC_WIFI && usbFresh)) {
    clkBase = localEpoch;
    clkAt = millis();
    clkSrc = src;
  }
  unlockState();
}

bool clockNow(int64_t& localEpoch) {
  lockState();
  bool ok = clkSrc != SRC_NONE;
  localEpoch = clkBase + (int64_t)((millis() - clkAt) / 1000);
  unlockState();
  return ok;
}

// ---------- shared helpers ----------

void deliverArt(uint8_t* buf, size_t len) {
  lockState();
  free(art.buf);
  art.buf = buf;
  art.len = len;
  art.ready = true;
  unlockState();
}

void foldText(char* dst, size_t n, const char* src) {
  // Latin-1 letters U+00C0..U+00FF mapped to their plain base letter
  static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
  size_t o = 0;
  const uint8_t* s = (const uint8_t*)src;
  while (*s && o + 1 < n) {
    uint32_t cp;
    int len;
    if (*s < 0x80) { cp = *s; len = 1; }
    else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; len = 2; }
    else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; len = 3; }
    else { cp = *s & 0x07; len = 4; }
    for (int i = 1; i < len; i++) {
      if ((s[i] & 0xC0) != 0x80) { len = i; cp = '?'; break; }
      cp = (cp << 6) | (s[i] & 0x3F);
    }
    s += len;
    char c = 0;
    if (cp >= 32 && cp < 127) c = (char)cp;
    else if (cp >= 0xC0 && cp <= 0xFF) c = latin1[cp - 0xC0];
    else if (cp == 0x2018 || cp == 0x2019) c = '\'';
    else if (cp == 0x201C || cp == 0x201D) c = '"';
    else if (cp == 0x2013 || cp == 0x2014) c = '-';
    else if (cp >= 0x80) c = (o && dst[o - 1] == '?') ? 0 : '?';  // collapse runs of unknown glyphs
    if (c) dst[o++] = c;
  }
  dst[o] = 0;
}

// ---------- USB link ----------
// Laptop -> board: one JSON object per line. {"t":"art","len":N} is followed by N raw JPEG bytes.
// Board -> laptop: {"t":"cmd"...} for the play controls and {"t":"status"...} replies.

void logLine(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  xSemaphoreTake(serialLock, portMAX_DELAY);
  Serial.print("# ");
  Serial.println(buf);
  xSemaphoreGive(serialLock);
}

static void sendJson(JsonDocument& d) {
  xSemaphoreTake(serialLock, portMAX_DELAY);
  serializeJson(d, Serial);
  Serial.write('\n');
  xSemaphoreGive(serialLock);
}

static void sendStatus(bool boot) {
  JsonDocument d;
  d["t"] = "status";
  d["boot"] = boot;
  d["ssid"] = cfg.ssid;
  d["wifi"] = (bool)wifiUp;
  d["ip"] = wifiUp ? WiFi.localIP().toString() : String();
  d["city"] = cfg.city;
  d["sp"] = cfg.spRefresh[0] != 0;
  d["f"] = cfg.fahrenheit;
  d["h12"] = cfg.h12;
  d["inv"] = cfg.invert;
  d["tfx"] = cfg.touchFlipX;
  d["tfy"] = cfg.touchFlipY;
  d["rot"] = cfg.rot;
  d["cat"] = cfg.catStyle;
  d["shape"] = cfg.catShape;
  d["alb"] = cfg.albumAccent;
  d["bri"] = cfg.brightness;
  d["art"] = uiArtSize();  // the companion sizes album art to fit exactly
  d["heap"] = ESP.getFreeHeap();
  sendJson(d);
}

static volatile bool statusDirty = false;
void notifyStatus() { statusDirty = true; }

void sendControl(uint8_t cmd) {
  if (usbActive()) {
    JsonDocument d;
    d["t"] = "cmd";
    static const char* names[] = {"", "toggle", "next", "prev", "play", "resume", "pause"};
    d["c"] = names[cmd];
    sendJson(d);
  } else {
    netCmd = cmd;
  }
}

static bool setStr(char* dst, size_t n, JsonVariantConst v) {
  if (!v.is<const char*>() || !strcmp(dst, v.as<const char*>())) return false;
  strlcpy(dst, v.as<const char*>(), n);
  return true;
}

static bool setBool(bool& dst, JsonVariantConst v) {
  if (!v.is<bool>() || dst == v.as<bool>()) return false;
  dst = v.as<bool>();
  return true;
}

static void applyCfg(JsonDocument& d) {
  bool net = false;
  net |= setStr(cfg.ssid, sizeof(cfg.ssid), d["ssid"]);
  net |= setStr(cfg.pass, sizeof(cfg.pass), d["pass"]);
  net |= setStr(cfg.city, sizeof(cfg.city), d["city"]);
  net |= setStr(cfg.spClientId, sizeof(cfg.spClientId), d["spid"]);
  net |= setStr(cfg.spRefresh, sizeof(cfg.spRefresh), d["sprt"]);
  net |= setBool(cfg.fahrenheit, d["f"]);
  if (d["lat"].is<float>() && d["lon"].is<float>() && (d["lat"].as<float>() != cfg.lat || d["lon"].as<float>() != cfg.lon)) {
    cfg.lat = d["lat"];
    cfg.lon = d["lon"];
    net = true;
  }
  bool ui = setBool(cfg.h12, d["h12"]);
  if (setBool(cfg.invert, d["inv"])) {
    tft.invertDisplay(cfg.invert);
    ui = true;
  }
  ui |= setBool(cfg.touchFlipX, d["tfx"]);
  ui |= setBool(cfg.touchFlipY, d["tfy"]);
  bool rotated = d["rot"].is<int>() && (d["rot"].as<int>() & 3) != cfg.rot;
  if (rotated) {
    cfg.rot = d["rot"].as<int>() & 3;
    cfg.catX = cfg.catY = -1;  // the cat goes back to its default corner for the new shape
    uiApplyRotation();
  }
  if (d["cat"].is<int>() && (d["cat"].as<int>() & 3) != cfg.catStyle) {
    cfg.catStyle = d["cat"].as<int>() & 3;
    catBackgroundChanged();
    ui = true;
  }
  if (d["shape"].is<int>() && d["shape"].as<int>() % 3 != cfg.catShape) {
    cfg.catShape = d["shape"].as<int>() % 3;
    catBackgroundChanged();
    ui = true;
  }
  if (d["bri"].is<int>() && constrain(d["bri"].as<int>(), 10, 255) != cfg.brightness) {
    cfg.brightness = constrain(d["bri"].as<int>(), 10, 255);
    applyBrightness();
    ui = true;
  }
  bool themed = setBool(cfg.albumAccent, d["alb"]);
  JsonArrayConst th = d["theme"];
  if (th.size() == TH_COUNT) {
    for (int i = 0; i < TH_COUNT; i++) {
      uint16_t c = th[i] | cfg.theme[i];
      if (c != cfg.theme[i]) {
        cfg.theme[i] = c;
        themed = true;
      }
    }
  }
  if (themed) {
    uiThemeChanged();
    catBackgroundChanged();
    ui = true;
  }
  if (net || ui || rotated) configSave();
  if (net) netReconfigure = true;
  if (ui) uiRedrawAll();
  sendStatus(false);
}

static void applyWeather(JsonDocument& d) {
  Weather w;
  strlcpy(w.city, d["city"] | "", sizeof(w.city));
  w.temp = d["tmp"] | 0.0f;
  w.code = d["code"] | 0;
  w.isDay = (d["day"] | 1) != 0;
  w.humidity = d["hum"] | 0;
  w.wind = d["wind"] | 0.0f;
  w.pop = d["pop"] | 0;
  w.fahrenheit = d["f"] | false;
  for (JsonArray day : d["d"].as<JsonArray>()) {
    if (w.dayCount >= 5) break;
    DayForecast& f = w.days[w.dayCount++];
    f.dow = day[0];
    f.code = day[1];
    f.tmax = lroundf(day[2].as<float>());
    f.tmin = lroundf(day[3].as<float>());
  }
  w.valid = true;
  lockState();
  w.version = wx.version + 1;
  wx = w;
  unlockState();
}

static uint8_t* binBuf = nullptr;
static size_t binLen = 0, binGot = 0, binSkip = 0;
static uint32_t binStart = 0;

static void handleLine(char* line) {
  JsonDocument d;
  if (deserializeJson(d, line)) return;
  const char* t = d["t"] | "";
  lastUsbMs = millis();
  if (!strcmp(t, "now")) {
    lockState();
    foldText(np.title, sizeof(np.title), d["title"] | "");
    foldText(np.artist, sizeof(np.artist), d["artist"] | "");
    foldText(np.album, sizeof(np.album), d["album"] | "");
    np.durMs = d["dur"] | 0;
    np.posMs = d["pos"] | 0;
    np.posAt = millis();
    np.playing = d["play"] | false;
    if ((int32_t)(holdPlayUntil - millis()) > 0) np.playing = holdPlaying;  // a tap is still on its way
    np.valid = np.title[0] != 0;
    np.src = SRC_USB;
    np.version++;
    unlockState();
  } else if (!strcmp(t, "art")) {
    size_t len = d["len"] | 0;
    binBuf = len && len <= 60000 ? (uint8_t*)malloc(len) : nullptr;
    if (binBuf) {
      binLen = len;
      binGot = 0;
      binStart = millis();
    } else {
      binSkip = len;  // still have to swallow the bytes
    }
  } else if (!strcmp(t, "time")) {
    clockSet(d["e"].as<int64_t>(), SRC_USB);
  } else if (!strcmp(t, "keys")) {
    uint32_t n = pendingTaps + (d["n"] | 0);
    pendingTaps = n > 8 ? 8 : n;
    lastKeyMs = millis();
  } else if (!strcmp(t, "sys")) {
    sys.cpu = d["cpu"] | 0;
    sys.ram = d["ram"] | 0;
    sys.wpm = d["wpm"] | 0;
    sys.at = millis() | 1;
  } else if (!strcmp(t, "wx")) {
    applyWeather(d);
  } else if (!strcmp(t, "cfg")) {
    applyCfg(d);
  } else if (!strcmp(t, "status")) {
    sendStatus(false);
  }
}

static void linkPoll() {
  static char line[1024];
  static size_t lineLen = 0;
  while (Serial.available()) {
    if (binBuf) {
      binGot += Serial.read(binBuf + binGot, binLen - binGot);
      if (binGot >= binLen) {
        deliverArt(binBuf, binLen);
        binBuf = nullptr;
      }
      continue;
    }
    int c = Serial.read();
    if (binSkip) {
      binSkip--;
      continue;
    }
    if (c == '\n') {
      line[lineLen] = 0;
      if (lineLen && line[0] == '{') handleLine(line);
      lineLen = 0;
    } else if (c != '\r') {
      if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
      else lineLen = 0;  // overlong garbage, drop it
    }
  }
  if (binBuf && millis() - binStart > 3000) {  // transfer stalled
    free(binBuf);
    binBuf = nullptr;
  }
}

// ---------- touch ----------

static bool touchRead(int16_t& x, int16_t& y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < 200) return false;
  // calibrated in the landscape (rotation 1) frame, then turned to match the screen
  int lx = constrain(map(p.x, 200, 3700, 0, 319), 0, 319);
  int ly = constrain(map(p.y, 240, 3800, 0, 239), 0, 239);
  int tx, ty;
  switch (cfg.rot) {
    case 1: tx = lx; ty = ly; break;
    case 3: tx = 319 - lx; ty = 239 - ly; break;
    case 0: tx = 239 - ly; ty = lx; break;
    default: tx = ly; ty = 319 - lx; break;
  }
  if (cfg.touchFlipX) tx = scrW - 1 - tx;
  if (cfg.touchFlipY) ty = scrH - 1 - ty;
  x = tx;
  y = ty;
  return true;
}

static void touchPoll() {
  enum { NONE, CAT_DRAG, SLIDER } static grab = NONE;
  static bool down = false;
  static int16_t x0, y0, lx, ly;
  static uint32_t t0, lastSeen;
  int16_t x, y;
  if (touchRead(x, y)) {
    if (!down) {
      down = true;
      x0 = x;
      y0 = y;
      t0 = millis();
      grab = NONE;
      if (catHit(x, y)) {
        grab = CAT_DRAG;
        catDragStart(x, y);
      } else if (uiDragStart(x, y)) {
        grab = SLIDER;
      }
    } else if (grab == CAT_DRAG) {
      catDragMove(x, y);
    } else if (grab == SLIDER) {
      uiDragMove(x, y);
    }
    lx = x;
    ly = y;
    lastSeen = millis();
    return;
  }
  if (!down || millis() - lastSeen < 60) return;  // resistive panels drop out briefly mid-press
  down = false;
  int dx = lx - x0, dy = ly - y0;
  if (grab == CAT_DRAG) {
    catDragEnd(abs(dx) > 8 || abs(dy) > 8);
    return;
  }
  if (grab == SLIDER) {
    uiDragEnd();
    return;
  }
  if (abs(dx) > 50 && abs(dx) > 2 * abs(dy)) uiSwipe(dx < 0 ? 1 : -1);
  else if (abs(dx) < 25 && abs(dy) < 25 && lastSeen - t0 < 800) uiTap(x0, y0);
}

// ---------- main ----------

void setup() {
  for (int pin : LED_PINS) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  Serial.setRxBufferSize(8192);
  Serial.begin(460800);
  stateLock = xSemaphoreCreateMutex();
  serialLock = xSemaphoreCreateMutex();
  nvsLock = xSemaphoreCreateMutex();
  configLoad();

  tft.init();
  tft.invertDisplay(cfg.invert);
  ledcSetup(BL_CHANNEL, 5000, 8);  // dimmable backlight instead of TFT_eSPI's plain on/off
  ledcAttachPin(TFT_BL, BL_CHANNEL);
  applyBrightness();
  touchSpi.begin(XPT_CLK, XPT_MISO, XPT_MOSI, XPT_CS);
  ts.begin(touchSpi);
  ts.setRotation(1);

  uiInit();
  netStart();
  sendStatus(true);
}

void loop() {
  linkPoll();
  touchPoll();
  uiTick();
  if (statusDirty) {
    statusDirty = false;
    sendStatus(false);
  }
  delay(1);
}
