// Standalone mode: WiFi, NTP, Open-Meteo forecast and the Spotify Web API.
// Runs as its own task on core 0 so slow HTTP calls never freeze the screen.
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "state.h"

static int32_t utcOffset = 0;
static bool offsetKnown = false;

static String spToken;
static uint32_t spTokenExp = 0;
static bool spDead = false;  // refresh token rejected: wait for a new one from the companion
static char spTrackId[64] = "";

static uint8_t dayOfWeek(const char* ymd) {  // "2026-10-30" -> 0 = Sunday
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = atoi(ymd), m = atoi(ymd + 5), d = atoi(ymd + 8);
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static bool fetchWeather() {
  char url[512];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=auto&forecast_days=5%s",
           cfg.lat, cfg.lon, cfg.fahrenheit ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  if (!http.begin(url)) return false;
  if (http.GET() != 200) {
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return false;

  Weather w;
  strlcpy(w.city, cfg.city, sizeof(w.city));
  JsonObject c = doc["current"];
  w.temp = c["temperature_2m"] | 0.0f;
  w.code = c["weather_code"] | 0;
  w.isDay = (c["is_day"] | 1) != 0;
  w.humidity = c["relative_humidity_2m"] | 0;
  w.wind = c["wind_speed_10m"] | 0.0f;
  w.fahrenheit = cfg.fahrenheit;
  JsonObject d = doc["daily"];
  JsonArray days = d["time"];
  for (size_t i = 0; i < days.size() && i < 5; i++) {
    DayForecast& f = w.days[w.dayCount++];
    f.dow = dayOfWeek(days[i] | "2000-01-01");
    f.code = d["weather_code"][i] | 0;
    f.tmax = lroundf(d["temperature_2m_max"][i] | 0.0f);
    f.tmin = lroundf(d["temperature_2m_min"][i] | 0.0f);
  }
  w.pop = d["precipitation_probability_max"][0] | 0;
  w.valid = true;
  utcOffset = doc["utc_offset_seconds"] | 0;
  offsetKnown = true;

  lockState();
  w.version = wx.version + 1;
  wx = w;
  unlockState();
  return true;
}

// ---------- Spotify ----------

static bool spEnsureToken() {
  if (spToken.length() && (int32_t)(spTokenExp - millis()) > 0) return true;
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  if (!http.begin(cli, "https://accounts.spotify.com/api/token")) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = String("grant_type=refresh_token&refresh_token=") + cfg.spRefresh + "&client_id=" + cfg.spClientId;
  int code = http.POST(body);
  if (code != 200) {
    http.end();
    logLine("token refresh failed: HTTP %d", code);
    if (code == 400 || code == 401) spDead = true;
    return false;
  }
  JsonDocument doc;
  deserializeJson(doc, http.getString());
  http.end();
  spToken = doc["access_token"] | "";
  spTokenExp = millis() + ((doc["expires_in"] | 3600) - 60) * 1000UL;
  // Spotify may rotate the refresh token; the old one stops working, so keep the new one
  const char* rt = doc["refresh_token"];
  if (rt && strcmp(rt, cfg.spRefresh)) {
    strlcpy(cfg.spRefresh, rt, sizeof(cfg.spRefresh));
    configSave();
  }
  return spToken.length() > 0;
}

static void downloadArt(const char* url) {
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  if (!http.begin(cli, url)) return;
  if (http.GET() != 200) {
    http.end();
    return;
  }
  int len = http.getSize();
  uint8_t* buf = len > 0 && len <= 90000 ? (uint8_t*)malloc(len) : nullptr;
  if (!buf) {
    http.end();
    return;
  }
  WiFiClient* s = http.getStreamPtr();
  int got = 0;
  uint32_t t0 = millis();
  while (got < len && millis() - t0 < 8000) {
    int avail = s->available();
    if (avail > 0) got += s->readBytes(buf + got, min(avail, len - got));
    else vTaskDelay(pdMS_TO_TICKS(5));
  }
  http.end();
  if (got < len) {
    free(buf);
    return;
  }
  deliverArt(buf, len);
}

// Every api.spotify.com call goes over one kept-alive HTTPS connection: a fresh TLS handshake
// costs the ESP32 1-2 s, a reused connection a couple of hundred ms.
static WiFiClientSecure apiCli;
static HTTPClient apiHttp;

static int spApi(const char* method, const char* url, const String& body = "", String* resp = nullptr) {
  static bool init = false;
  if (!init) {
    apiCli.setInsecure();
    apiHttp.setReuse(true);
    apiHttp.setTimeout(6000);
    init = true;
  }
  if (!apiHttp.begin(apiCli, url)) return -1;
  apiHttp.addHeader("Authorization", "Bearer " + spToken);
  if (body.length()) apiHttp.addHeader("Content-Type", "application/json");
  apiHttp.addHeader("Content-Length", String(body.length()));  // Spotify wants it even when 0
  int code = apiHttp.sendRequest(method, (uint8_t*)body.c_str(), body.length());
  String text = code > 0 ? apiHttp.getString() : String();  // read it all so the connection can be reused
  apiHttp.end();
  if (code < 0) apiCli.stop();  // broken connection: start clean next time
  if (resp) *resp = text;
  return code;
}

static void spotifyPoll() {
  if (!spEnsureToken()) return;
  String body;
  int code = spApi("GET", "https://api.spotify.com/v1/me/player?additional_types=episode", "", &body);
  if (code != 200 && code != 204) logLine("poll: HTTP %d", code);
  if (code == 204) {  // no active device: nothing playing anywhere
    lockState();
    if (np.src != SRC_USB && np.valid) {
      np.valid = false;
      np.playing = false;
      np.src = SRC_WIFI;
      np.version++;
    }
    unlockState();
    spTrackId[0] = 0;
    return;
  }
  if (code == 401) spTokenExp = 0;
  if (code != 200) return;

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["device"]["name"] = true;
  filter["device"]["type"] = true;
  JsonObject fi = filter["item"].to<JsonObject>();
  fi["id"] = true;
  fi["name"] = true;
  fi["duration_ms"] = true;
  fi["artists"][0]["name"] = true;
  fi["album"]["name"] = true;
  fi["album"]["images"][0]["url"] = true;
  fi["album"]["images"][0]["width"] = true;
  fi["show"]["name"] = true;  // podcast episodes
  fi["images"][0]["url"] = true;
  fi["images"][0]["width"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err || doc["item"].isNull()) return;

  JsonObject item = doc["item"];
  char artists[160] = "";
  for (JsonObject a : item["artists"].as<JsonArray>()) {
    if (artists[0]) strlcat(artists, ", ", sizeof(artists));
    strlcat(artists, a["name"] | "", sizeof(artists));
  }
  if (!artists[0]) strlcpy(artists, item["show"]["name"] | "", sizeof(artists));
  const char* album = item["album"]["name"] | (item["show"]["name"] | "");

  // Prefer the ~300px image: it decodes to 150px at 1/2 scale
  JsonArray imgs = item["album"]["images"].isNull() ? item["images"].as<JsonArray>() : item["album"]["images"].as<JsonArray>();
  const char* imgUrl = "";
  int best = INT32_MAX;
  for (JsonObject im : imgs) {
    int diff = abs((im["width"] | 0) - 300);
    if (diff < best) {
      best = diff;
      imgUrl = im["url"] | "";
    }
  }

  lockState();
  foldText(np.title, sizeof(np.title), item["name"] | "");
  foldText(np.artist, sizeof(np.artist), artists);
  foldText(np.album, sizeof(np.album), album);
  np.durMs = item["duration_ms"] | 0;
  np.posMs = doc["progress_ms"] | 0;
  np.posAt = millis();
  np.playing = doc["is_playing"] | false;
  if ((int32_t)(holdPlayUntil - millis()) > 0) np.playing = holdPlaying;  // a tap is still on its way
  np.valid = np.title[0] != 0;
  np.src = SRC_WIFI;
  foldText(np.device, sizeof(np.device), doc["device"]["name"] | "");
  np.version++;
  unlockState();

  const char* id = item["id"] | (item["name"] | "");
  if (strcmp(id, spTrackId)) {
    strlcpy(spTrackId, id, sizeof(spTrackId));
    if (imgUrl[0]) downloadArt(imgUrl);
  }
}

// Start playing from nothing: pick one of the account's devices (the desktop app first, then a
// phone, then anything; a browser web player only when nothing else is open, even if Spotify
// calls it the active one) and start playback there.
static void spotifyStart() {
  String body;
  if (spApi("GET", "https://api.spotify.com/v1/me/player/devices", "", &body) != 200) return;
  JsonDocument doc;
  deserializeJson(doc, body);
  String pick, pickName;
  int score = -1;
  logLine("play: %u device(s)", doc["devices"].size());
  for (JsonObject d : doc["devices"].as<JsonArray>()) {
    if (d["is_restricted"] | false) continue;
    const char* type = d["type"] | "";
    String name = d["name"] | "";
    name.toLowerCase();
    bool web = name.indexOf("web player") >= 0;  // a browser tab also reports itself as a "Computer"
    int s = web ? 0 : !strcmp(type, "Computer") ? 4 : !strcmp(type, "Smartphone") ? 3 : 2;
    if (d["is_active"] | false) s += 1;  // breaks ties, never beats a better kind of device
    logLine("  %s | %s | active=%d restricted=%d score=%d", d["name"] | "?", type, (int)(d["is_active"] | false),
            (int)(d["is_restricted"] | false), s);
    if (s > score) {
      score = s;
      pick = d["id"] | "";
      pickName = d["name"] | "";
    }
  }
  if (!pick.length()) return;  // Spotify isn't open anywhere
  int code = spApi("PUT", "https://api.spotify.com/v1/me/player", "{\"device_ids\":[\"" + pick + "\"],\"play\":true}");
  logLine("play: picked %s -> HTTP %d", pickName.c_str(), code);
}

static void spotifyControl(uint8_t cmd) {
  if (!spEnsureToken()) return;
  if (cmd == CMD_PLAY) {
    spotifyStart();
    return;
  }
  const char* method = "PUT";
  const char* url;
  if (cmd == CMD_NEXT) method = "POST", url = "https://api.spotify.com/v1/me/player/next";
  else if (cmd == CMD_PREV) method = "POST", url = "https://api.spotify.com/v1/me/player/previous";
  else if (cmd == CMD_PAUSE) url = "https://api.spotify.com/v1/me/player/pause";
  else url = "https://api.spotify.com/v1/me/player/play";  // CMD_RESUME
  uint32_t t0 = millis();
  int code = spApi(method, url);
  logLine("control %d -> HTTP %d in %u ms", cmd, code, (unsigned)(millis() - t0));
}

// ---------- task ----------

static void netTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  bool started = false;
  uint32_t startAt = 0, nextWx = 0, lastClk = 0, nextSp = 0;
  for (;;) {
    if (netReconfigure) {
      netReconfigure = false;
      WiFi.disconnect();
      started = false;
      nextWx = 0;
      spToken = "";
      spTokenExp = 0;
      spDead = false;
      spTrackId[0] = 0;
    }
    if (!cfg.ssid[0]) {
      wifiUp = false;
      netCmd = CMD_NONE;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!started) {
      WiFi.begin(cfg.ssid, cfg.pass);
      started = true;
      startAt = millis();
    }
    wifiUp = WiFi.status() == WL_CONNECTED;
    if (!wifiUp) {
      if (millis() - startAt > 30000) {  // kick it and try again
        WiFi.disconnect();
        started = false;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    uint32_t now = millis();
    if (!isnan(cfg.lat) && (!nextWx || (int32_t)(now - nextWx) >= 0)) {
      nextWx = now + (fetchWeather() ? 15 * 60000 : 60000);
      if (!nextWx) nextWx = 1;
    }
    time_t t = time(nullptr);
    if (offsetKnown && t > 1700000000 && (!lastClk || now - lastClk > 60000)) {
      clockSet((int64_t)t + utcOffset, SRC_WIFI);
      lastClk = now;
    }
    if (artWanted) {
      artWanted = false;
      spTrackId[0] = 0;  // next poll downloads the cover again
    }
    if (cfg.spRefresh[0] && !spDead && !usbActive()) {
      uint8_t cmd = netCmd;
      if (cmd) {
        netCmd = CMD_NONE;
        spotifyControl(cmd);
        nextSp = millis() + 900;  // give Spotify a moment, then show what really happened
      }
      bool playing;
      lockState();
      playing = np.playing;
      unlockState();
      if (!nextSp || (int32_t)(millis() - nextSp) >= 0) {
        spotifyPoll();
        nextSp = millis() + (playing ? 3000 : 6000);
      }
    } else {
      netCmd = CMD_NONE;
    }
    vTaskDelay(pdMS_TO_TICKS(20));  // pick up taps quickly
  }
}

void netStart() {
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}
