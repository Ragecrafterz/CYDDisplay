#pragma once
// State shared between the UI loop (core 1) and the network task (core 0).
// Anything both sides touch is guarded by stateLock.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum Source : uint8_t { SRC_NONE = 0, SRC_USB, SRC_WIFI };

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Colour theme slots; the companion app holds the presets and sends all eight colours
enum ThemeSlot : uint8_t { TH_BG, TH_BAR, TH_CARD, TH_LINE, TH_TXT, TH_SUB, TH_DIM, TH_ACC, TH_COUNT };

struct Config {
  char ssid[33] = "";
  char pass[65] = "";
  char city[48] = "";
  float lat = NAN;
  float lon = NAN;
  bool fahrenheit = false;
  bool h12 = false;
  bool invert = true;
  bool touchFlipX = false;
  bool touchFlipY = false;
  uint8_t rot = 1;  // TFT rotation: 1/3 landscape, 0/2 portrait
  int16_t catX = -1;  // cat bubble position, -1 = default corner
  int16_t catY = -1;
  uint8_t catStyle = 0;  // colours: 0 classic, 1 tabby, 2 midnight, 3 calico
  uint8_t catShape = 0;  // body: 0 chibi, 1 chubby, 2 original
  bool albumAccent = true;  // accent colour follows the album art
  uint8_t brightness = 255;
  uint16_t theme[TH_COUNT] = {  // "Midnight"
      rgb(13, 15, 22), rgb(20, 23, 33), rgb(30, 34, 48), rgb(52, 58, 78),
      rgb(242, 243, 247), rgb(196, 200, 212), rgb(138, 144, 162), rgb(30, 215, 96)};
  char spClientId[48] = "";
  char spRefresh[256] = "";
};

struct NowPlaying {
  char title[96] = "";
  char artist[96] = "";
  char album[96] = "";
  uint32_t durMs = 0;
  uint32_t posMs = 0;
  uint32_t posAt = 0;  // millis() when posMs was sampled
  bool playing = false;
  bool valid = false;
  Source src = SRC_NONE;
  char device[40] = "";  // Spotify Connect device it's playing on (WiFi mode)
  uint32_t version = 0;  // bumped on every update
};

struct DayForecast {
  uint8_t dow;  // 0 = Sunday
  uint8_t code;  // WMO weather code
  int16_t tmax;
  int16_t tmin;
};

struct Weather {
  char city[48] = "";
  float temp = 0;
  uint8_t code = 0;
  bool isDay = true;
  uint8_t humidity = 0;
  float wind = 0;
  uint8_t pop = 0;  // today's max rain chance, %
  bool fahrenheit = false;
  DayForecast days[5] = {};
  uint8_t dayCount = 0;
  bool valid = false;
  uint32_t version = 0;
};

// Laptop stats for the big-cat screen (USB only)
struct SysStats {
  uint8_t cpu = 0, ram = 0;
  uint16_t wpm = 0;
  uint32_t at = 0;  // millis() of the last update, 0 = never
};

// Album art handed from the producer (USB link or network task) to the UI as a JPEG.
struct ArtSlot {
  uint8_t* buf = nullptr;
  size_t len = 0;
  bool ready = false;
};

extern Config cfg;
extern NowPlaying np;
extern Weather wx;
extern ArtSlot art;
extern SysStats sys;  // only touched by the UI loop
extern SemaphoreHandle_t stateLock;

extern volatile uint32_t lastUsbMs;
extern volatile uint32_t lastKeyMs;
extern volatile uint16_t pendingTaps;
extern volatile bool wifiUp;
extern volatile bool netReconfigure;
extern volatile bool artWanted;  // UI lost the cover (e.g. rotated): fetch it again
extern volatile uint8_t netCmd;  // CMD_* for the network task to send to Spotify

enum : uint8_t { CMD_NONE = 0, CMD_TOGGLE, CMD_NEXT, CMD_PREV, CMD_PLAY, CMD_RESUME, CMD_PAUSE };

// After a play/pause tap the screen shows the tapped state; updates that were already on their
// way (and still say the old thing) must not flip it back until this time.
extern volatile uint32_t holdPlayUntil;
extern volatile bool holdPlaying;

inline void lockState() { xSemaphoreTake(stateLock, portMAX_DELAY); }
inline void unlockState() { xSemaphoreGive(stateLock); }
inline bool usbActive() {
  uint32_t t = lastUsbMs;
  return t && millis() - t < 6000;
}

void configSave();
void applyBrightness();
void notifyStatus();  // a setting changed on the board: tell the companion
void clockSet(int64_t localEpoch, Source src);
bool clockNow(int64_t& localEpoch);
void deliverArt(uint8_t* buf, size_t len);  // takes ownership of buf
void foldText(char* dst, size_t n, const char* src);  // UTF-8 -> plain ASCII for the built-in fonts
void sendControl(uint8_t cmd);
void logLine(const char* fmt, ...);  // "# ..." debug line over USB; the companion ignores these
void netStart();
