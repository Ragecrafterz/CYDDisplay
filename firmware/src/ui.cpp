// Screen: a clock bar along the top (15% of the height), then two swipeable pages:
//   0  now playing: album art, track info, progress, controls
//   1  current weather and a 5-day forecast
// Landscape and portrait each get their own layout. Colours come from the theme, and the
// music page can take its accent from the album art.
//
// The cat bubble (cat.cpp) floats on top. Page pieces never paint over it: when a piece
// overlaps the bubble it is drawn around it, and the cat repaints its own copy of the page
// underneath (uiRenderUnder) so its round corners always show the right pixels.
// All page drawing goes through G, which is the screen or the cat's background sprite.
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <time.h>
#include "ui.h"

int16_t scrW = 320, scrH = 240, hdrH = 36;

struct Layout {
  bool portrait;
  Rect art, title, artist, album, prog, ctrl, src, dots;
  Rect big, stats;  // idle screen: big cat and laptop stats
  int16_t ctrlY, prevX, playX, nextX;
};
static Layout L;

static TFT_eSPI* G = &tft;
static TFT_eSprite hdr(&tft);
static TFT_eSprite marq(&tft);

static int page = 0;  // 0 music, 1 weather, 2 settings
static int lastMainPage = 0;  // where "Done" on the settings page goes back to
static bool pageDirty = true;
static uint32_t lastHdr = 0;

static uint16_t accent, pageBg;
static uint32_t accentResetAt = 0;

// music page
static NowPlaying cur;
static uint32_t shownVersion = UINT32_MAX;
static char shownTitle[96] = "\x01";
static char shownArtist[96] = "";
static bool shownPlaying = false, shownValid = false;
static Source shownSrc = SRC_NONE;
static uint8_t* artKeep = nullptr;
static size_t artKeepLen = 0;
static bool dText, dArt, dCtrl, dProg, dSrc;
static uint32_t lastProg = 0;
static char marqText[96] = "";
static int titleW = 0, marqOff = 0;
static uint32_t marqAt = 0, marqPauseUntil = 0;

// weather page
static uint32_t shownWx = UINT32_MAX;

static const char* SHAPE_NAMES[] = {"Chibi", "Chubby", "Original"};

static bool onScreen() { return G == &tft; }
// Nothing playing: the music page becomes a big bongo cat with the laptop's stats
static bool idleScreen() { return page == 0 && !shownValid; }

// pushImage isn't virtual in TFT_eSPI: called through G on a sprite it would write straight
// to the panel (and wedge the SPI bus), so pick the right class explicitly.
static void pushImg(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) {
  if (onScreen()) tft.pushImage(x, y, w, h, data);
  else static_cast<TFT_eSprite*>(G)->pushImage(x, y, w, h, data);
}
static uint16_t bg() { return page == 0 ? pageBg : C_BG; }
uint16_t uiAccent() { return accent; }
int16_t uiArtSize() { return L.art.w; }

uint8_t luma(uint16_t c) {
  int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
  return (r * 77 + g * 150 + b * 29) >> 8;
}

// ---------- layout ----------

static void computeSettings();

static void computeLayout() {
  scrW = tft.width();
  scrH = tft.height();
  L.portrait = scrH > scrW;
  if (!L.portrait) {
    hdrH = 36;
    L.art = {10, 44, 136, 136};
    L.title = {158, 44, 154, 26};
    L.artist = {158, 72, 154, 20};
    L.album = {158, 94, 154, 18};
    L.prog = {152, 116, 168, 34};
    L.ctrl = {152, 152, 168, 54};
    L.ctrlY = 179;
    L.prevX = 192;
    L.playX = 238;
    L.nextX = 284;
    L.src = {152, 208, 168, 18};
    L.dots = {148, 226, 24, 10};
    L.big = {8, 42, 168, 168};
    L.stats = {184, 42, 136, 180};
  } else {
    hdrH = 48;
    L.art = {48, 54, 132, 132};
    L.title = {10, 190, 220, 26};
    L.artist = {10, 218, 220, 20};
    L.album = {10, 238, 220, 18};
    L.prog = {0, 256, 240, 22};
    L.ctrl = {0, 278, 240, 42};
    L.ctrlY = 298;
    L.prevX = 66;
    L.playX = 120;
    L.nextX = 174;
    L.src = {0, 0, 0, 0};  // no room in portrait
    L.dots = {108, 49, 24, 5};
    L.big = {36, 58, 168, 168};
    L.stats = {0, 228, 240, 92};
  }
  computeSettings();
}

// ---------- clipping (screen only) ----------

static Rect clips[4];
static int clipDepth = 0;

static Rect intersect(const Rect& a, const Rect& b) {
  int16_t x = max(a.x, b.x), y = max(a.y, b.y);
  int16_t r = min(a.x + a.w, b.x + b.w), btm = min(a.y + a.h, b.y + b.h);
  return {x, y, (int16_t)(r - x), (int16_t)(btm - y)};
}

static Rect currentClip() { return clipDepth ? clips[clipDepth - 1] : Rect{0, 0, scrW, scrH}; }

static bool pushClip(const Rect& r) {
  Rect c = intersect(r, currentClip());
  if (c.empty()) return false;
  clips[clipDepth++] = c;
  tft.setViewport(c.x, c.y, c.w, c.h, false);
  return true;
}

static void popClip() {
  clipDepth--;
  Rect c = currentClip();
  if (clipDepth) tft.setViewport(c.x, c.y, c.w, c.h, false);
  else tft.resetViewport();
}

// Draw a piece of the page on screen without touching the cat bubble: split the piece's
// rectangle around the bubble, draw into each part, then let the cat refresh its backdrop.
template <typename F>
static void paint(const Rect& r, F draw) {
  if (!onScreen()) {
    draw();
    return;
  }
  Rect c = catRect();
  if (!r.overlaps(c)) {
    draw();
    return;
  }
  int16_t top = max(r.y, c.y), bottom = min(r.y + r.h, c.y + c.h);
  Rect parts[4] = {
      {r.x, r.y, r.w, (int16_t)(c.y - r.y)},
      {r.x, (int16_t)(c.y + c.h), r.w, (int16_t)(r.y + r.h - c.y - c.h)},
      {r.x, top, (int16_t)(c.x - r.x), (int16_t)(bottom - top)},
      {(int16_t)(c.x + c.w), top, (int16_t)(r.x + r.w - c.x - c.w), (int16_t)(bottom - top)},
  };
  for (const Rect& p : parts) {
    if (p.empty()) continue;
    if (pushClip(p)) {
      draw();
      popClip();
    }
  }
  catBackgroundChanged();
}

// ---------- helpers ----------

static uint16_t blend(uint16_t a, uint16_t b, uint8_t amt) {  // amt/255 of b over a
  int ar = (a >> 11) << 3, ag = ((a >> 5) & 63) << 2, ab = (a & 31) << 3;
  int br = (b >> 11) << 3, bgc = ((b >> 5) & 63) << 2, bb = (b & 31) << 3;
  return rgb(ar + (br - ar) * amt / 255, ag + (bgc - ag) * amt / 255, ab + (bb - ab) * amt / 255);
}

static void setAccent(uint16_t a) {
  uint16_t newBg = cfg.albumAccent ? blend(C_BG, a, luma(C_BG) > 128 ? 40 : 26) : C_BG;
  if (a == accent && newBg == pageBg) return;
  accent = a;
  pageBg = newBg;
  pageDirty = true;
}

static String fitText(TFT_eSPI& g, const char* s, int maxW) {
  String t(s);
  if (g.textWidth(t) <= maxW) return t;
  while (t.length() && g.textWidth(t + "...") > maxW) t.remove(t.length() - 1);
  return t + "...";
}

static void drawDegree(TFT_eSPI& g, int x, int y, int r, uint16_t c) {
  g.drawCircle(x, y, r, c);
  if (r > 2) g.drawCircle(x, y, r - 1, c);
}

static void roundCorners(const Rect& r, int rad, uint16_t c) {
  for (int i = 0; i < rad; i++) {
    float dy = rad - i - 0.5f;
    int dx = rad - (int)(sqrtf(rad * rad - dy * dy) + 0.5f);
    if (dx <= 0) continue;
    G->drawFastHLine(r.x, r.y + i, dx, c);
    G->drawFastHLine(r.x + r.w - dx, r.y + i, dx, c);
    G->drawFastHLine(r.x, r.y + r.h - 1 - i, dx, c);
    G->drawFastHLine(r.x + r.w - dx, r.y + r.h - 1 - i, dx, c);
  }
}

static const char* wxText(uint8_t c) {
  switch (c) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm with hail";
    default: return "--";
  }
}

// Weather icon drawn from primitives, centred on (cx, cy) inside an s x s box
void drawWxIcon(TFT_eSPI& g, int cx, int cy, int s, uint8_t code, bool day, uint16_t bgc) {
  bool light = luma(bgc) > 150;  // darker clouds on light themes so they stay visible
  const uint16_t SUN = rgb(255, 190, 30), MOON = light ? rgb(150, 156, 180) : rgb(222, 226, 240),
                 CLOUD = light ? rgb(160, 168, 186) : rgb(214, 218, 230),
                 GREY = light ? rgb(110, 116, 134) : rgb(128, 134, 150),
                 RAIN = rgb(70, 150, 255), SNOW = light ? rgb(120, 170, 230) : TFT_WHITE, BOLT = rgb(255, 200, 0);
  auto sun = [&](int x, int y, int r) {
    for (int a = 0; a < 8; a++) {
      float t = a * PI / 4;
      int r1 = r + 2 + s / 30, r2 = r + 2 + s / 8;
      g.drawLine(x + cosf(t) * r1, y + sinf(t) * r1, x + cosf(t) * r2, y + sinf(t) * r2, SUN);
      g.drawLine(x + cosf(t) * r1 + 1, y + sinf(t) * r1, x + cosf(t) * r2 + 1, y + sinf(t) * r2, SUN);
    }
    g.fillCircle(x, y, r, SUN);
  };
  auto moon = [&](int x, int y, int r) {
    g.fillCircle(x, y, r, MOON);
    g.fillCircle(x + r / 2, y - r / 3, r * 3 / 4, bgc);
  };
  auto cloud = [&](int x, int y, int w, uint16_t c) {  // (x, y) is roughly the cloud's centre
    int h = w / 2;
    y += h / 6;
    g.fillCircle(x - w / 5, y - h / 6, h / 3, c);
    g.fillCircle(x + w / 8, y - h / 4, h * 2 / 5, c);
    g.fillRoundRect(x - w / 2, y - h / 6, w, h / 2, h / 4, c);
  };
  auto sky = [&](int x, int y, int r) { day ? sun(x, y, r) : moon(x, y, r + r / 4); };

  if (code == 0) {
    sky(cx, cy, s / 5);
  } else if (code <= 2) {
    sky(cx - s / 7, cy - s / 7, s / 6);
    cloud(cx + s / 10, cy + s / 8, s * 7 / 10, CLOUD);
  } else if (code == 3) {
    cloud(cx - s / 8, cy - s / 10, s * 11 / 20, GREY);
    cloud(cx + s / 12, cy + s / 10, s * 7 / 10, CLOUD);
  } else if (code == 45 || code == 48) {
    cloud(cx, cy - s / 8, s * 7 / 10, GREY);
    for (int i = 0; i < 3; i++) g.fillRoundRect(cx - s * 2 / 5 + i * s / 10, cy + s / 6 + i * s / 9, s * 3 / 5, max(2, s / 24), 1, GREY);
  } else if (code >= 95) {
    cloud(cx, cy - s / 6, s * 3 / 4, GREY);
    int k = max(1, s / 20);
    g.fillTriangle(cx + 2 * k, cy, cx - 2 * k, cy + 5 * k, cx + k, cy + 5 * k, BOLT);
    g.fillTriangle(cx - k, cy + 4 * k, cx + 2 * k, cy + 4 * k, cx - k, cy + 9 * k, BOLT);
  } else {
    bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    cloud(cx, cy - s / 6, s * 3 / 4, CLOUD);
    for (int i = 0; i < 3; i++) {
      int x = cx - s / 5 + i * s / 5, y = cy + s / 5;
      if (snow) {
        g.fillCircle(x, y, max(1, s / 22), SNOW);
        g.fillCircle(x - s / 10, y + s / 7, max(1, s / 22), SNOW);
      } else {
        g.drawLine(x, y - s / 20, x - s / 12, y + s / 5, RAIN);
        g.drawLine(x + 1, y - s / 20, x + 1 - s / 12, y + s / 5, RAIN);
      }
    }
  }
}

// ---------- header ----------

static void drawHeader() {
  hdr.fillSprite(C_BAR);
  int64_t now;
  bool ok = clockNow(now);
  char tb[8] = "--:--", db[24] = "";
  const char* ampm = "";
  if (ok) {
    time_t tt = (time_t)now;
    struct tm tmv;
    gmtime_r(&tt, &tmv);
    int h = tmv.tm_hour;
    if (cfg.h12) {
      ampm = h < 12 ? "AM" : "PM";
      h %= 12;
      if (!h) h = 12;
      snprintf(tb, sizeof(tb), "%d:%02d", h, tmv.tm_min);
    } else {
      snprintf(tb, sizeof(tb), "%02d:%02d", h, tmv.tm_min);
    }
    strftime(db, sizeof(db), L.portrait ? "%A %d %b" : "%a %d %b", &tmv);
  }

  int timeY = L.portrait ? 18 : 17;
  hdr.setTextDatum(ML_DATUM);
  hdr.setFreeFont(L.portrait ? &FreeSansBold18pt7b : &FreeSansBold12pt7b);
  hdr.setTextColor(C_TXT);
  hdr.drawString(tb, 10, timeY);
  int x = 10 + hdr.textWidth(tb) + 5;
  hdr.setFreeFont(&FreeSans9pt7b);
  if (*ampm) {
    hdr.setTextColor(C_SUB);
    hdr.drawString(ampm, x, timeY + (L.portrait ? 4 : 0));
    x += hdr.textWidth(ampm) + 8;
  }
  hdr.setTextColor(C_DIM);
  if (L.portrait) {
    hdr.setTextFont(2);
    hdr.drawString(db, 11, 39);
  } else {
    hdr.drawString(db, x + 2, timeY);
  }

  // settings gear, top-right
  {
    int gx = scrW - 17, gy = 18;
    uint16_t gc = page == 2 ? accent : C_SUB;
    for (int a = 0; a < 8; a++) {
      float t = a * PI / 4;
      hdr.fillCircle(gx + cosf(t) * 7, gy + sinf(t) * 7, 2, gc);
    }
    hdr.fillCircle(gx, gy, 6, gc);
    hdr.fillCircle(gx, gy, 3, C_BAR);
  }

  // connection indicator: next to the gear in landscape, under it in portrait
  int rx = L.portrait ? scrW - 10 : scrW - 36;
  int linkY = L.portrait ? 38 : 18;
  hdr.setTextFont(2);
  hdr.setTextDatum(MC_DATUM);
  int linkW;
  if (usbActive()) {
    linkW = hdr.textWidth("USB") + 12;
    hdr.fillRoundRect(rx - linkW, linkY - 9, linkW, 18, 9, C_CARD);
    hdr.setTextColor(accent);
    hdr.drawString("USB", rx - linkW / 2, linkY);
  } else if (wifiUp) {
    linkW = 14;
    for (int i = 0; i < 3; i++) hdr.fillRoundRect(rx - 14 + i * 5, linkY + 6 - (i + 1) * 4, 3, (i + 1) * 4, 1, C_SUB);
  } else {
    hdr.setTextColor(C_DIM);
    hdr.setTextDatum(MR_DATUM);
    hdr.drawString("offline", rx, linkY);
    linkW = hdr.textWidth("offline");
  }
  rx = L.portrait ? scrW - 36 : rx - linkW - 10;

  // current weather
  lockState();
  bool wv = wx.valid;
  float temp = wx.temp;
  uint8_t code = wx.code;
  bool isDay = wx.isDay;
  unlockState();
  if (wv) {
    int wy = L.portrait ? 16 : 18;
    char t[8];
    snprintf(t, sizeof(t), "%d", (int)lroundf(temp));
    hdr.setFreeFont(&FreeSansBold9pt7b);
    hdr.setTextColor(C_TXT);
    hdr.setTextDatum(MR_DATUM);
    drawDegree(hdr, rx - 3, wy - 5, 2, C_TXT);
    hdr.drawString(t, rx - 8, wy);
    rx -= hdr.textWidth(t) + 8 + 16;
    drawWxIcon(hdr, rx, wy, 24, code, isDay, C_BAR);
  }
  hdr.drawFastHLine(0, hdrH - 1, scrW, C_LINE);
  hdr.pushSprite(0, 0);
}

// ---------- music page pieces ----------

static uint32_t accR, accG, accB, accW;

// The cover is decoded once into artPix (RGB565, art box sized) and blitted from there,
// so repainting bits of it while the cat moves around costs a copy, not a JPEG decode.
static uint16_t* artPix = nullptr;
static bool artPixOk = false;

static bool jpgToCache(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  const Rect& a = L.art;
  int x0 = max<int>(x, a.x), x1 = min<int>(x + w, a.x + a.w);
  int y0 = max<int>(y, a.y), y1 = min<int>(y + h, a.y + a.h);
  if (x0 >= x1 || y0 >= y1) return true;
  for (int yy = y0; yy < y1; yy++) {
    memcpy(artPix + (yy - a.y) * a.w + (x0 - a.x), bmp + (yy - y) * w + (x0 - x), (x1 - x0) * 2);
  }
  for (int yy = y0; yy < y1; yy += 3) {
    for (int xx = x0; xx < x1; xx += 3) {
      uint16_t p = bmp[(yy - y) * w + (xx - x)];
      int r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
      int mx = max(r, max(g, b)), mn = min(r, min(g, b));
      uint32_t wt = (mx - mn) * (mx > 50) + 2;  // favour colourful, not-too-dark pixels
      accR += r * wt;
      accG += g * wt;
      accB += b * wt;
      accW += wt;
    }
  }
  return true;
}

static uint16_t accentFromSums() {
  if (!accW) return C_ACC;
  uint32_t r = accR / accW, g = accG / accW, b = accB / accW;
  uint32_t mx = max(r, max(g, b)), mn = min(r, min(g, b));
  if (mx < 24 || mx - mn < 24) return C_ACC;  // greyscale cover: keep the theme's accent
  // a bright, saturated version of the cover's main hue
  float k = 240.0f / mx;
  int rr = r * k, gg = g * k, bb = b * k;
  int lo = min(rr, min(gg, bb));
  rr -= lo / 3;
  gg -= lo / 3;
  bb -= lo / 3;
  uint16_t c = rgb(constrain(rr, 0, 255), constrain(gg, 0, 255), constrain(bb, 0, 255));
  if (luma(C_BG) > 150) c = blend(c, 0, 70);  // light themes need a darker accent for contrast
  return c;
}

static void decodeArt() {
  const Rect& a = L.art;
  uint16_t w = 0, h = 0;
  if (!artKeep || TJpgDec.getJpgSize(&w, &h, artKeep, artKeepLen) != JDR_OK || !w) return;
  if (!artPix) artPix = (uint16_t*)malloc(a.w * a.h * 2);
  if (!artPix) return;
  for (int i = 0; i < a.w * a.h; i++) artPix[i] = C_CARD;
  // biggest decode that still covers the box; anything extra is cropped evenly
  uint8_t scale = 1;
  while (scale < 8 && w / (scale * 2) >= a.w && h / (scale * 2) >= a.h) scale *= 2;
  int dw = w / scale, dh = h / scale;
  accR = accG = accB = accW = 0;
  TJpgDec.setJpgScale(scale);
  if (TJpgDec.drawJpg(a.x + (a.w - dw) / 2, a.y + (a.h - dh) / 2, artKeep, artKeepLen) != JDR_OK) return;  // e.g. progressive
  artPixOk = true;
  // the JPEG isn't needed once decoded, and the network task needs that memory for HTTPS
  free(artKeep);
  artKeep = nullptr;
  setAccent(cfg.albumAccent ? accentFromSums() : C_ACC);
}

static void drawPlaceholderArt() {
  const Rect& a = L.art;
  G->fillRect(a.x, a.y, a.w, a.h, bg());
  G->fillRoundRect(a.x, a.y, a.w, a.h, 10, C_CARD);
  int cx = a.x + a.w / 2, cy = a.y + a.h / 2;
  G->fillCircle(cx - 10, cy + 20, 12, C_LINE);
  G->fillRect(cx - 1, cy - 30, 5, 50, C_LINE);
  G->fillTriangle(cx + 4, cy - 30, cx + 26, cy - 16, cx + 4, cy - 12, C_LINE);
}

static void drawArt() {
  const Rect& a = L.art;
  if (!artPixOk) decodeArt();
  if (!artPixOk) {
    drawPlaceholderArt();
    return;
  }
  G->setSwapBytes(true);
  pushImg(a.x, a.y, a.w, a.h, artPix);
  G->setSwapBytes(false);
  roundCorners(a, 10, bg());
}

static void drawTitle() {
  marq.fillSprite(bg());
  marq.setTextColor(C_TXT);
  if (titleW <= L.title.w) {
    marq.setTextDatum(L.portrait ? TC_DATUM : TL_DATUM);
    marq.drawString(marqText, L.portrait ? L.title.w / 2 : 0, 2);
  } else {
    marq.setTextDatum(TL_DATUM);
    marq.drawString(marqText, -marqOff, 2);
    marq.drawString(marqText, -marqOff + titleW + 40, 2);
  }
  if (onScreen()) {
    marq.pushSprite(L.title.x, L.title.y);
  } else {
    G->setSwapBytes(false);  // sprite to sprite: copy the raw pixels as they are
    pushImg(L.title.x, L.title.y, L.title.w, L.title.h, (uint16_t*)marq.getPointer());
  }
}

static void drawTrackText() {
  drawTitle();
  Rect r = {L.artist.x, L.artist.y, L.artist.w, (int16_t)(L.album.y + L.album.h - L.artist.y)};
  G->fillRect(r.x, r.y, r.w, r.h, bg());
  uint8_t datum = L.portrait ? TC_DATUM : TL_DATUM;
  int x = L.portrait ? L.artist.x + L.artist.w / 2 : L.artist.x;
  G->setTextDatum(datum);
  G->setFreeFont(&FreeSans9pt7b);
  G->setTextColor(C_SUB);
  G->drawString(fitText(*G, cur.valid ? cur.artist : "Play something on Spotify", L.artist.w), x, L.artist.y + 2);
  if (cur.valid && cur.album[0]) {
    G->setTextFont(2);
    G->setTextColor(C_DIM, bg());
    G->drawString(fitText(*G, cur.album, L.album.w), x, L.album.y + 1);
  }
}

static void drawControls() {
  const Rect& r = L.ctrl;
  G->fillRect(r.x, r.y, r.w, r.h, bg());
  int y = L.ctrlY;
  G->fillRect(L.prevX - 9, y - 8, 3, 16, C_TXT);
  G->fillTriangle(L.prevX + 7, y - 8, L.prevX + 7, y + 8, L.prevX - 6, y, C_TXT);
  G->fillRect(L.nextX + 7, y - 8, 3, 16, C_TXT);
  G->fillTriangle(L.nextX - 6, y - 8, L.nextX - 6, y + 8, L.nextX + 7, y, C_TXT);
  G->fillCircle(L.playX, y, 20, accent);
  uint16_t ink = luma(accent) > 140 ? rgb(14, 16, 22) : TFT_WHITE;
  if (cur.playing) {
    G->fillRoundRect(L.playX - 7, y - 8, 5, 16, 1, ink);
    G->fillRoundRect(L.playX + 2, y - 8, 5, 16, 1, ink);
  } else {
    G->fillTriangle(L.playX - 5, y - 9, L.playX - 5, y + 9, L.playX + 9, y, ink);
  }
}

static void fmtTime(char* b, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  snprintf(b, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void drawProgress() {
  const Rect& r = L.prog;
  G->fillRect(r.x, r.y, r.w, r.h, bg());
  if (!cur.valid || !cur.durMs) return;
  uint32_t pos = cur.posMs + (cur.playing ? millis() - cur.posAt : 0);
  if (pos > cur.durMs) pos = cur.durMs;
  char a[12], b[12];
  fmtTime(a, sizeof(a), pos);
  fmtTime(b, sizeof(b), cur.durMs);
  int bx, bw, by;
  G->setTextFont(2);
  G->setTextColor(C_DIM, bg());
  if (L.portrait) {
    bx = 50;
    bw = 140;
    by = r.y + 10;
    G->setTextDatum(ML_DATUM);
    G->drawString(a, 10, by + 1);
    G->setTextDatum(MR_DATUM);
    G->drawString(b, 230, by + 1);
  } else {
    bx = 158;
    bw = 154;
    by = r.y + 9;
    G->setTextDatum(TL_DATUM);
    G->drawString(a, bx, by + 8);
    G->setTextDatum(TR_DATUM);
    G->drawString(b, bx + bw, by + 8);
  }
  int w = (uint64_t)bw * pos / cur.durMs;
  G->fillRoundRect(bx, by - 1, bw, 4, 2, C_LINE);
  if (w > 0) G->fillRoundRect(bx, by - 1, max(w, 4), 4, 2, accent);
  G->fillCircle(bx + w, by + 1, 4, C_TXT);
}

static void drawSource() {
  const Rect& r = L.src;
  if (r.empty()) return;
  G->fillRect(r.x, r.y, r.w, r.h, bg());
  if (!cur.valid) return;
  G->fillCircle(r.x + 10, r.y + 9, 3, cur.playing ? accent : C_DIM);
  G->setTextFont(2);
  G->setTextDatum(ML_DATUM);
  G->setTextColor(C_DIM, bg());
  char b[64];
  if (cur.src == SRC_USB) strlcpy(b, cur.playing ? "Playing on laptop" : "Paused on laptop", sizeof(b));
  else snprintf(b, sizeof(b), "%s on %s", cur.playing ? "Playing" : "Paused", cur.device[0] ? cur.device : "Spotify");
  G->drawString(fitText(*G, b, r.w - 22), r.x + 18, r.y + 9);
}

static void drawDots() {
  const Rect& r = L.dots;
  G->fillRect(r.x, r.y, r.w, r.h, bg());
  int cy = r.y + r.h / 2, rad = L.portrait ? 2 : 3;
  for (int i = 0; i < 2; i++) G->fillCircle(r.x + 6 + i * 12, cy, rad, i == page ? C_TXT : C_LINE);
}

// ---------- idle screen (nothing playing) ----------

static uint32_t shownSys = 0;

// Three stat rows next to (landscape) or under (portrait) the big cat. With the laptop
// connected: its CPU, RAM and typing speed. On its own: WiFi and the weather.
struct StatRow {
  const char* label;
  char value[40];
  int pct;    // bar fill 0..100, or -1 for no bar
  int degAt;  // draw a degree sign after this many characters, or -1
};

static void setRow(StatRow& r, const char* label, const char* value, int pct, int degAt) {
  r.label = label;
  strlcpy(r.value, value, sizeof(r.value));
  r.pct = pct;
  r.degAt = degAt;
}

static void statRows(StatRow rows[3]) {
  if (sys.at && usbActive()) {
    setRow(rows[0], "CPU", "", sys.cpu, -1);
    setRow(rows[1], "RAM", "", sys.ram, -1);
    setRow(rows[2], "Typing", "", min<int>(sys.wpm, 120) * 100 / 120, -1);
    snprintf(rows[0].value, 40, "%u%%", sys.cpu);
    snprintf(rows[1].value, 40, "%u%%", sys.ram);
    snprintf(rows[2].value, 40, L.portrait ? "%u" : "%u wpm", sys.wpm);
    if (L.portrait) rows[2].label = "wpm";
    return;
  }
  Weather w;
  lockState();
  w = wx;
  unlockState();
  int rssi = wifiUp ? WiFi.RSSI() : -100;
  int signal = constrain(2 * (rssi + 100), 0, 100);
  setRow(rows[0], "WiFi", "", wifiUp ? signal : -1, -1);
  if (L.portrait) snprintf(rows[0].value, 40, wifiUp ? "%d%%" : "off", signal);
  else strlcpy(rows[0].value, wifiUp ? cfg.ssid : cfg.ssid[0] ? "connecting..." : "not set up", 40);
  setRow(rows[1], "Now", "--", -1, -1);
  setRow(rows[2], L.portrait ? "high/low" : "Today", "--", -1, -1);
  if (L.portrait) rows[1].label = "now";
  if (w.valid) {
    int n = snprintf(rows[1].value, 40, "%d", (int)lroundf(w.temp));
    rows[1].degAt = n;
    if (!L.portrait) snprintf(rows[1].value + n, 40 - n, "  %s", wxText(w.code));
    else strlcat(rows[1].value, " ", 40);
    if (w.dayCount) {
      if (L.portrait) snprintf(rows[2].value, 40, "%d/%d", w.days[0].tmax, w.days[0].tmin);
      else snprintf(rows[2].value, 40, "%d / %d, rain %u%%", w.days[0].tmax, w.days[0].tmin, w.pop);
    }
  }
}

// Draw text with a little degree circle after the first `degAt` characters
static void degreeAfter(const StatRow& r, int textLeft, int top) {
  if (r.degAt < 0) return;
  char head[16];
  strlcpy(head, r.value, min<int>(sizeof(head), r.degAt + 1));
  drawDegree(*G, textLeft + G->textWidth(head) + 4, top + 2, 2, C_TXT);
}

static void drawStats() {
  StatRow rows[3];
  statRows(rows);
  if (!L.portrait) {
    Rect r = {L.stats.x, 98, L.stats.w, 124};
    G->fillRect(r.x, r.y, r.w, r.h, bg());
    for (int i = 0; i < 3; i++) {
      int y = 102 + i * 38, x = r.x + 4, w = r.w - 16;
      G->setTextFont(2);
      G->setTextDatum(TL_DATUM);
      G->setTextColor(C_DIM, bg());
      G->drawString(rows[i].label, x, y);
      int labelW = G->textWidth(rows[i].label) + 8;
      String v = fitText(*G, rows[i].value, w - labelW);
      G->setTextDatum(TR_DATUM);
      G->setTextColor(C_TXT, bg());
      G->drawString(v, x + w, y);
      if (v == rows[i].value) degreeAfter(rows[i], x + w - G->textWidth(v), y);
      if (rows[i].pct >= 0) {
        G->fillRoundRect(x, y + 20, w, 5, 2, C_LINE);
        if (rows[i].pct > 0) G->fillRoundRect(x, y + 20, max(5, w * min(rows[i].pct, 100) / 100), 5, 2, accent);
      }
    }
  } else {
    Rect r = {0, 256, scrW, 64};
    G->fillRect(r.x, r.y, r.w, r.h, bg());
    for (int i = 0; i < 3; i++) {
      int cx = 40 + i * 80;
      G->setTextDatum(MC_DATUM);
      G->setFreeFont(&FreeSansBold12pt7b);
      G->setTextColor(C_TXT);
      G->drawString(rows[i].value, cx, 276);
      degreeAfter(rows[i], cx - G->textWidth(rows[i].value) / 2, 266);
      G->setTextFont(2);
      G->setTextColor(C_DIM, bg());
      G->drawString(rows[i].label, cx, 302);
    }
  }
}
// Something can be started from here: the laptop (opens Spotify if needed) or, on its own,
// a linked Spotify account over WiFi.
static bool canStartPlayback() { return usbActive() || (cfg.spRefresh[0] && wifiUp); }
static bool shownCanPlay = false;

static Rect playButton() {
  return L.portrait ? Rect{60, 227, 120, 26} : Rect{(int16_t)(L.stats.x + 4), 68, 120, 26};
}

static void drawPlayPrompt() {
  shownCanPlay = canStartPlayback();
  Rect b = playButton();
  if (L.portrait) {
    G->fillRect(0, b.y - 2, scrW, b.h + 4, bg());
  } else {
    G->fillRect(L.stats.x, 64, L.stats.w, 34, bg());
  }
  if (shownCanPlay) {
    G->fillRoundRect(b.x, b.y, b.w, b.h, b.h / 2, accent);
    uint16_t ink = luma(accent) > 140 ? rgb(14, 16, 22) : TFT_WHITE;
    int ty = b.y + b.h / 2;
    G->fillTriangle(b.x + 16, ty - 6, b.x + 16, ty + 6, b.x + 26, ty, ink);
    G->setTextFont(2);
    G->setTextDatum(ML_DATUM);
    G->setTextColor(ink, accent);
    G->drawString("Play Spotify", b.x + 34, ty);
  } else if (L.portrait) {
    G->setFreeFont(&FreeSansBold9pt7b);
    G->setTextDatum(MC_DATUM);
    G->setTextColor(C_TXT);
    G->drawString("Nothing playing", scrW / 2, 240);
  } else {
    G->setTextFont(2);
    G->setTextDatum(TL_DATUM);
    G->setTextColor(C_DIM, bg());
    G->drawString("Play some music and", L.stats.x + 4, 68);
    G->drawString("it shows up here", L.stats.x + 4, 84);
  }
}

static void drawIdle(const Rect& area) {
  G->fillRect(area.x, area.y, area.w, area.h, bg());
  if (!L.portrait) {
    G->setFreeFont(&FreeSansBold9pt7b);
    G->setTextColor(C_TXT);
    G->setTextDatum(TL_DATUM);
    G->drawString("Nothing playing", L.stats.x + 4, 46);
  }
  drawPlayPrompt();
  drawStats();
  drawDots();
  // the cat paints itself into L.big
}

static void drawMusic(const Rect& area) {
  if (idleScreen()) {
    drawIdle(area);
    return;
  }
  G->fillRect(area.x, area.y, area.w, area.h, bg());
  if (L.art.overlaps(area)) drawArt();
  if (L.title.overlaps(area) || L.artist.overlaps(area) || L.album.overlaps(area)) drawTrackText();
  if (L.prog.overlaps(area)) drawProgress();
  if (L.ctrl.overlaps(area)) drawControls();
  if (L.src.overlaps(area)) drawSource();
  if (L.dots.overlaps(area)) drawDots();
}

static void marqueeTick() {
  uint32_t now = millis();
  if (titleW <= L.title.w || (int32_t)(now - marqPauseUntil) < 0 || now - marqAt < 30) return;
  marqAt = now;
  if (++marqOff >= titleW + 40) {
    marqOff = 0;
    marqPauseUntil = now + 2500;
  }
  paint(L.title, drawTitle);
}

// Pull new track info / art from the shared state, whichever page is showing.
static void syncMusic() {
  lockState();
  if (np.src == SRC_USB && np.valid && !usbActive()) {  // laptop went away
    np.valid = false;
    np.playing = false;
    np.version++;
  }
  bool changed = np.version != shownVersion;
  if (changed) {
    cur = np;
    shownVersion = np.version;
  }
  uint8_t* newArt = nullptr;
  size_t newLen = 0;
  if (art.ready) {
    newArt = art.buf;
    newLen = art.len;
    art.buf = nullptr;
    art.ready = false;
  }
  unlockState();

  if (changed) {
    if (strcmp(cur.title, shownTitle) || strcmp(cur.artist, shownArtist) || cur.valid != shownValid) {
      strlcpy(shownTitle, cur.title, sizeof(shownTitle));
      strlcpy(shownArtist, cur.artist, sizeof(shownArtist));
      if (cur.valid != shownValid && page == 0) pageDirty = true;  // switch between music and the big cat
      shownValid = cur.valid;
      free(artKeep);  // new track: old art is wrong; new art follows the track info
      artKeep = nullptr;
      artPixOk = false;
      strlcpy(marqText, cur.valid ? cur.title : "Nothing playing", sizeof(marqText));
      titleW = marq.textWidth(marqText);
      marqOff = 0;
      marqPauseUntil = millis() + 2500;
      accentResetAt = millis() + 1500;  // back to the theme colour unless art shows up
      dText = dArt = dSrc = true;
    }
    if (cur.playing != shownPlaying || cur.src != shownSrc) {
      shownPlaying = cur.playing;
      shownSrc = cur.src;
      dCtrl = dSrc = true;
    }
    dProg = true;
  }
  if (newArt) {
    free(artKeep);
    artKeep = newArt;
    artKeepLen = newLen;
    artPixOk = false;
    accentResetAt = 0;
    dArt = true;
  }
  if (accentResetAt && (int32_t)(millis() - accentResetAt) >= 0) {
    accentResetAt = 0;
    setAccent(C_ACC);
  }
}

// ---------- weather page ----------

static void weatherLandscape(const Weather& w) {
  char b[40];
  G->setTextFont(2);
  G->setTextDatum(TL_DATUM);
  G->setTextColor(C_DIM, C_BG);
  G->drawString(fitText(*G, w.city, 180), 12, 42);

  drawWxIcon(*G, 44, 86, 60, w.code, w.isDay, C_BG);
  snprintf(b, sizeof(b), "%d", (int)lroundf(w.temp));
  G->setFreeFont(&FreeSansBold24pt7b);
  G->setTextColor(C_TXT);
  G->drawString(b, 84, 60);
  int x = 84 + G->textWidth(b) + 5;
  drawDegree(*G, x, 66, 4, C_TXT);
  G->setFreeFont(&FreeSansBold12pt7b);
  G->drawString(w.fahrenheit ? "F" : "C", x + 7, 61);
  G->setFreeFont(&FreeSans9pt7b);
  G->setTextColor(C_SUB);
  G->drawString(fitText(*G, wxText(w.code), 110), 86, 96);

  G->fillRoundRect(200, 44, 112, 66, 10, C_CARD);
  G->setTextFont(2);
  const char* labels[] = {"Rain", "Humidity", "Wind"};
  char vals[3][16];
  snprintf(vals[0], 16, "%u%%", w.pop);
  snprintf(vals[1], 16, "%u%%", w.humidity);
  snprintf(vals[2], 16, "%d %s", (int)lroundf(w.wind), w.fahrenheit ? "mph" : "km/h");
  for (int i = 0; i < 3; i++) {
    int y = 56 + i * 20;
    G->setTextDatum(ML_DATUM);
    G->setTextColor(C_DIM, C_CARD);
    G->drawString(labels[i], 210, y);
    G->setTextDatum(MR_DATUM);
    G->setTextColor(C_TXT, C_CARD);
    G->drawString(vals[i], 302, y);
  }

  static const char* names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (int i = 0; i < w.dayCount; i++) {
    const DayForecast& f = w.days[i];
    int x0 = 10 + i * 61, y0 = 118;
    G->fillRoundRect(x0, y0, 57, 60, 10, C_CARD);
    if (i == 0) G->drawRoundRect(x0, y0, 57, 60, 10, C_LINE);
    G->setTextDatum(MC_DATUM);
    G->setTextColor(i == 0 ? C_TXT : C_DIM, C_CARD);
    G->drawString(i == 0 ? "Today" : names[f.dow % 7], x0 + 28, y0 + 11);
    drawWxIcon(*G, x0 + 28, y0 + 30, 22, f.code, true, C_CARD);
    snprintf(b, sizeof(b), "%d", f.tmax);
    G->setTextDatum(ML_DATUM);
    G->setTextColor(C_TXT, C_CARD);
    G->drawString(b, x0 + 8, y0 + 49);
    snprintf(b, sizeof(b), "%d", f.tmin);
    G->setTextDatum(MR_DATUM);
    G->setTextColor(C_DIM, C_CARD);
    G->drawString(b, x0 + 49, y0 + 49);
  }
}

static void weatherPortrait(const Weather& w) {
  char b[40];
  G->setFreeFont(&FreeSansBold9pt7b);
  G->setTextDatum(TL_DATUM);
  G->setTextColor(C_SUB);
  G->drawString(fitText(*G, w.city, 160), 12, 56);

  drawWxIcon(*G, 46, 112, 64, w.code, w.isDay, C_BG);
  snprintf(b, sizeof(b), "%d", (int)lroundf(w.temp));
  G->setFreeFont(&FreeSansBold24pt7b);
  G->setTextColor(C_TXT);
  G->drawString(b, 90, 86);
  int x = 90 + G->textWidth(b) + 5;
  drawDegree(*G, x, 92, 4, C_TXT);
  G->setFreeFont(&FreeSansBold12pt7b);
  G->drawString(w.fahrenheit ? "F" : "C", x + 7, 87);
  G->setFreeFont(&FreeSans9pt7b);
  G->setTextColor(C_SUB);
  G->drawString(fitText(*G, wxText(w.code), 140), 92, 124);

  G->fillRoundRect(10, 148, 220, 36, 10, C_CARD);
  const char* labels[] = {"rain", "humidity", "wind"};
  char vals[3][16];
  snprintf(vals[0], 16, "%u%%", w.pop);
  snprintf(vals[1], 16, "%u%%", w.humidity);
  snprintf(vals[2], 16, "%d %s", (int)lroundf(w.wind), w.fahrenheit ? "mph" : "km/h");
  for (int i = 0; i < 3; i++) {
    int cx = 47 + i * 73;
    G->setTextDatum(MC_DATUM);
    G->setTextFont(2);
    G->setTextColor(C_TXT, C_CARD);
    G->drawString(vals[i], cx, 160);
    G->setTextFont(1);
    G->setTextColor(C_DIM, C_CARD);
    G->drawString(labels[i], cx, 176);
  }

  static const char* names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  G->fillRoundRect(10, 192, 220, 124, 10, C_CARD);
  for (int i = 0; i < w.dayCount; i++) {
    const DayForecast& f = w.days[i];
    int yc = 204 + i * 24;
    G->setTextFont(2);
    G->setTextDatum(ML_DATUM);
    G->setTextColor(i == 0 ? C_TXT : C_SUB, C_CARD);
    G->drawString(i == 0 ? "Today" : names[f.dow % 7], 20, yc);
    drawWxIcon(*G, 124, yc, 20, f.code, true, C_CARD);
    snprintf(b, sizeof(b), "%d", f.tmax);
    G->setTextDatum(MR_DATUM);
    G->setTextColor(C_TXT, C_CARD);
    G->drawString(b, 186, yc);
    snprintf(b, sizeof(b), "%d", f.tmin);
    G->setTextColor(C_DIM, C_CARD);
    G->drawString(b, 220, yc);
    if (i < w.dayCount - 1) G->drawFastHLine(20, yc + 12, 200, C_LINE);
  }
}

static void drawWeather(const Rect& area) {
  Weather w;
  lockState();
  w = wx;
  unlockState();
  G->fillRect(area.x, area.y, area.w, area.h, C_BG);
  if (!w.valid) {
    int cy = hdrH + (scrH - hdrH) / 2 - 20;
    G->setTextDatum(MC_DATUM);
    G->setFreeFont(&FreeSansBold9pt7b);
    G->setTextColor(C_TXT);
    G->drawString("No forecast yet", scrW / 2, cy);
    G->setTextFont(2);
    G->setTextColor(C_DIM, C_BG);
    G->drawString("Pick a city in the companion app", scrW / 2, cy + 24);
  } else if (L.portrait) {
    weatherPortrait(w);
  } else {
    weatherLandscape(w);
  }
  drawDots();
}

// ---------- settings page ----------

struct SettingsLayout {
  Rect done, slider, shape, seg, rot;
  int16_t swY, swX[4], swR, labelBri, labelCat;
};
static SettingsLayout S;
static uint32_t lastSliderDraw = 0;

static void computeSettings() {
  int y0 = hdrH + 6;
  S.done = {(int16_t)(scrW - 76), (int16_t)y0, 64, 26};
  S.labelBri = y0 + 40;
  S.slider = {20, (int16_t)(y0 + 58), (int16_t)(scrW - 40), 20};
  S.labelCat = y0 + 84;
  S.swR = 15;
  S.swY = y0 + 120;
  int step = L.portrait ? 56 : 52;
  for (int i = 0; i < 4; i++) S.swX[i] = (L.portrait ? 36 : 38) + i * step;
  if (L.portrait) {
    S.shape = {12, (int16_t)(y0 + 148), (int16_t)(scrW - 24), 28};
    S.seg = {12, (int16_t)(y0 + 186), (int16_t)(scrW - 24), 30};
    S.rot = {12, (int16_t)(y0 + 224), (int16_t)(scrW - 24), 30};
  } else {
    S.shape = {222, (int16_t)(S.swY - 14), (int16_t)(scrW - 234), 28};
    S.seg = {12, (int16_t)(y0 + 152), 140, 30};
    S.rot = {166, (int16_t)(y0 + 152), (int16_t)(scrW - 178), 30};
  }
}

static bool inRect(const Rect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void drawSlider() {
  const Rect& s = S.slider;
  G->fillRect(0, s.y - 2, scrW, s.h + 4, C_BG);
  int cy = s.y + s.h / 2;
  int kx = s.x + (int)(cfg.brightness - 10) * s.w / 245;
  G->fillRoundRect(s.x, cy - 3, s.w, 6, 3, C_LINE);
  G->fillRoundRect(s.x, cy - 3, max(kx - s.x, 6), 6, 3, accent);
  G->fillCircle(kx, cy, 9, C_TXT);
  char b[8];
  snprintf(b, sizeof(b), "%d%%", (cfg.brightness * 100 + 127) / 255);
  G->fillRect(scrW - 60, S.labelBri, 48, 16, C_BG);
  G->setTextFont(2);
  G->setTextDatum(TR_DATUM);
  G->setTextColor(C_SUB, C_BG);
  G->drawString(b, scrW - 12, S.labelBri);
}

static void drawSwatches() {
  G->fillRect(0, S.swY - S.swR - 6, scrW, 2 * S.swR + 12, C_BG);
  for (int i = 0; i < 4; i++) {
    if (i == cfg.catStyle) G->fillCircle(S.swX[i], S.swY, S.swR + 5, accent);
    catDrawSwatch(*G, S.swX[i], S.swY, S.swR, i);
  }
}

static void button(const Rect& r, const char* text, bool on) {
  G->fillRoundRect(r.x, r.y, r.w, r.h, r.h / 2, on ? accent : C_CARD);
  G->setTextFont(2);
  G->setTextDatum(MC_DATUM);
  uint16_t fg = on ? (luma(accent) > 140 ? rgb(14, 16, 22) : TFT_WHITE) : C_TXT;
  G->setTextColor(fg, on ? accent : C_CARD);
  G->drawString(text, r.x + r.w / 2, r.y + r.h / 2);
}

static void drawSeg() {
  const Rect& s = S.seg;
  G->fillRect(s.x, s.y, s.w, s.h, C_BG);
  G->fillRoundRect(s.x, s.y, s.w, s.h, s.h / 2, C_CARD);
  button({s.x, s.y, (int16_t)(s.w / 2), s.h}, "24-hour", !cfg.h12);
  button({(int16_t)(s.x + s.w / 2), s.y, (int16_t)(s.w / 2), s.h}, "12-hour", cfg.h12);
}

static void drawSettings(const Rect& area) {
  G->fillRect(area.x, area.y, area.w, area.h, C_BG);
  G->setFreeFont(&FreeSansBold12pt7b);
  G->setTextDatum(ML_DATUM);
  G->setTextColor(C_TXT);
  G->drawString("Settings", 12, S.done.y + S.done.h / 2);
  button(S.done, "Done", true);
  G->setTextFont(2);
  G->setTextDatum(TL_DATUM);
  G->setTextColor(C_DIM, C_BG);
  G->drawString("Brightness", 12, S.labelBri);
  G->drawString("Cat", 12, S.labelCat);
  drawSlider();
  drawSwatches();
  drawSeg();
  button(S.shape, SHAPE_NAMES[cfg.catShape % 3], false);
  button(S.rot, "Rotate screen", false);
}

static void setBrightnessFromX(int x) {
  const Rect& s = S.slider;
  int v = 10 + (long)constrain(x - s.x, 0, s.w) * 245 / s.w;
  if (v == cfg.brightness) return;
  cfg.brightness = v;
  applyBrightness();
  if (millis() - lastSliderDraw > 40) {
    lastSliderDraw = millis();
    drawSlider();
  }
}

static void settingsTap(int x, int y) {
  if (inRect(S.done, x, y)) {
    page = lastMainPage;
    pageDirty = true;
    return;
  }
  for (int i = 0; i < 4; i++) {
    int dx = x - S.swX[i], dy = y - S.swY;
    if (dx * dx + dy * dy <= (S.swR + 10) * (S.swR + 10) && cfg.catStyle != i) {
      cfg.catStyle = i;
      configSave();
      notifyStatus();
      drawSwatches();
      catBackgroundChanged();
      return;
    }
  }
  if (inRect(S.shape, x, y)) {
    cfg.catShape = (cfg.catShape + 1) % 3;
    configSave();
    notifyStatus();
    button(S.shape, SHAPE_NAMES[cfg.catShape], false);
    catBackgroundChanged();
    return;
  }
  if (inRect(S.seg, x, y)) {
    bool h12 = x >= S.seg.x + S.seg.w / 2;
    if (h12 != cfg.h12) {
      cfg.h12 = h12;
      configSave();
      notifyStatus();
      drawSeg();
      lastHdr = 0;
    }
  } else if (inRect(S.rot, x, y)) {
    static const uint8_t next[] = {3, 0, 1, 2};  // landscape -> portrait -> flipped -> ...
    cfg.rot = next[cfg.rot & 3];
    cfg.catX = cfg.catY = -1;
    configSave();
    notifyStatus();
    uiApplyRotation();
  }
}

static void drawPage(const Rect& area) {
  if (page == 0) drawMusic(area);
  else if (page == 1) drawWeather(area);
  else drawSettings(area);
}

// ---------- public ----------

void uiRestore(const Rect& r) {
  if (r.empty() || pageDirty) return;  // a full redraw is coming anyway
  if (pushClip(r)) {
    drawPage(r);
    popClip();
  }
}

void uiRenderUnder(TFT_eSprite& spr, const Rect& r) {
  if (pageDirty) {
    spr.fillSprite(bg());
    return;
  }
  G = &spr;
  spr.setViewport(-r.x, -r.y, scrW, scrH, true);  // draw in screen coordinates
  drawPage(r);
  spr.resetViewport();
  G = &tft;
}

void uiThemeChanged() {
  accent = C_ACC;
  pageBg = C_BG;
  if (artPixOk) setAccent(cfg.albumAccent ? accentFromSums() : C_ACC);  // the cover's colour sums are kept
  pageDirty = true;
  lastHdr = 0;
}

void uiApplyRotation() {
  tft.setRotation(cfg.rot);
  computeLayout();
  hdr.deleteSprite();
  hdr.setColorDepth(16);
  hdr.createSprite(scrW, hdrH);
  free(artPix);  // the art box size may have changed
  artPix = nullptr;
  artPixOk = false;
  artWanted = true;  // the JPEG is gone: ask for the cover again at the new size
  marq.deleteSprite();
  marq.setColorDepth(16);
  marq.createSprite(L.title.w, L.title.h);
  marq.setFreeFont(&FreeSansBold12pt7b);
  titleW = marq.textWidth(marqText);
  catLayoutChanged();
  tft.fillScreen(C_BG);
  pageDirty = true;
  lastHdr = 0;
}

void uiInit() {
  accent = C_ACC;
  pageBg = C_BG;
  TJpgDec.setCallback(jpgToCache);
  catInit();
  uiApplyRotation();
}

void uiRedrawAll() { pageDirty = true; }

void uiSwipe(int dir) {
  if (page == 2) return;
  page = (page + dir + 2) % 2;
  pageDirty = true;
}

bool uiCatVisible() { return page != 2 && !idleScreen(); }

bool uiDragStart(int16_t x, int16_t y) {
  if (page != 2) return false;
  const Rect& s = S.slider;
  if (y < s.y - 14 || y > s.y + s.h + 14) return false;
  setBrightnessFromX(x);
  return true;
}

void uiDragMove(int16_t x, int16_t y) { setBrightnessFromX(x); }

void uiDragEnd() {
  drawSlider();
  configSave();
  notifyStatus();
}

void uiTap(int16_t x, int16_t y) {
  if (y < hdrH && x > scrW - 40) {  // gear
    if (page == 2) {
      page = lastMainPage;
    } else {
      lastMainPage = page;
      page = 2;
    }
    pageDirty = true;
    return;
  }
  if (page == 2) {
    settingsTap(x, y);
    return;
  }
  if (page != 0) return;
  if (idleScreen()) {
    Rect b = playButton();
    if (shownCanPlay && x >= b.x - 6 && x < b.x + b.w + 6 && y >= b.y - 8 && y < b.y + b.h + 8) sendControl(CMD_PLAY);
    else if (x >= L.big.x && x < L.big.x + L.big.w && y >= L.big.y && y < L.big.y + L.big.h) catPoke();
    return;
  }
  auto near = [&](int cx, int r) { return (x - cx) * (x - cx) + (y - L.ctrlY) * (y - L.ctrlY) <= r * r; };
  if (near(L.playX, 28)) {
    // flip right away; the next update from the laptop/Spotify confirms it
    // decide play or pause from what the screen shows, show it right away, and hold it until
    // the laptop/Spotify have caught up
    lockState();
    bool valid = np.valid, want = !np.playing;
    if (valid) {
      np.posMs += np.playing ? millis() - np.posAt : 0;
      np.posAt = millis();
      np.playing = want;
      np.version++;
    }
    unlockState();
    if (!valid) return;
    holdPlaying = want;
    holdPlayUntil = millis() + 3000;
    sendControl(want ? CMD_RESUME : CMD_PAUSE);
  } else if (near(L.prevX, 24)) {
    sendControl(CMD_PREV);
  } else if (near(L.nextX, 24)) {
    sendControl(CMD_NEXT);
  }
}

void uiTick() {
  syncMusic();
  uint32_t now = millis();
  if (pageDirty) {
    pageDirty = false;
    if (page == 0) {
      dText = dCtrl = dProg = dSrc = dArt = false;
    } else {
      shownWx = wx.version;
    }
    catSetBig(idleScreen() ? L.big : Rect{0, 0, 0, 0}, bg());
    Rect content = {0, hdrH, scrW, (int16_t)(scrH - hdrH)};
    paint(content, [&] { drawPage(content); });
    lastProg = now;
    lastHdr = 0;
    catBackgroundChanged();
  }
  if (!lastHdr || now - lastHdr >= 1000) {
    drawHeader();
    lastHdr = now ? now : 1;
  }
  if (idleScreen()) {
    if (sys.at != shownSys || now - lastProg >= 2000) {  // stats come every ~2 s; also catches the laptop leaving
      shownSys = sys.at;
      lastProg = now;
      drawStats();
      if (canStartPlayback() != shownCanPlay) drawPlayPrompt();
    }
  } else if (page == 0) {
    if (dArt) { dArt = false; paint(L.art, drawArt); }
    if (dText) {
      dText = false;
      paint({L.title.x, L.title.y, L.title.w, (int16_t)(L.album.y + L.album.h - L.title.y)}, drawTrackText);
    }
    if (dCtrl) { dCtrl = false; paint(L.ctrl, drawControls); }
    if (dSrc) { dSrc = false; paint(L.src, drawSource); }
    if (dProg || now - lastProg >= 1000) {
      dProg = false;
      lastProg = now;
      paint(L.prog, drawProgress);
    }
    marqueeTick();
  } else if (page == 1) {
    lockState();
    uint32_t v = wx.version;
    unlockState();
    if (v != shownWx) {
      shownWx = v;
      Rect content = {0, hdrH, scrW, (int16_t)(scrH - hdrH)};
      paint(content, [&] { drawWeather(content); });
    }
  }
  catTick(false);
}
