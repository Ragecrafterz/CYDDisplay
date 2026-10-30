// The bongo cat. Slaps the desk when you type, bops along to music, blinks when idle and
// dozes off after two minutes. Three body shapes (chibi, chubby, original) in four colours.
//
// Normally it lives in a chat-head bubble you can drag anywhere; let go and it flies to the
// nearest edge or corner. The bubble is drawn as: a copy of the page underneath (under), the
// cat on top (cs, with a key colour outside the circle), merged into one opaque sprite (comp)
// and pushed in one go, so the round corners always show the real page.
//
// When nothing is playing the music page hands the cat a big box instead (catSetBig): the
// same 56px drawing, scaled up with chunky pixels.
#include "ui.h"

static const int CAT = 56;  // bubble diameter
static const int MARGIN = 4;
static const uint16_t KEY = 0x0001;  // marks pixels outside the bubble

struct CatLook {
  uint16_t fur, ink, line, ear, blush, eye, earL, earR;
  uint8_t marks;  // 0 none, 1 tabby stripes, 2 calico patches
};

static const CatLook LOOKS[] = {
    // classic white
    {rgb(255, 250, 246), rgb(52, 44, 56), rgb(52, 44, 56), rgb(255, 196, 210), rgb(255, 150, 176), rgb(52, 44, 56),
     rgb(255, 250, 246), rgb(255, 250, 246), 0},
    // orange tabby
    {rgb(255, 182, 96), rgb(110, 58, 24), rgb(96, 50, 20), rgb(255, 214, 176), rgb(255, 128, 110), rgb(70, 38, 18),
     rgb(255, 182, 96), rgb(255, 182, 96), 1},
    // midnight black
    {rgb(48, 46, 60), rgb(16, 16, 22), rgb(200, 198, 214), rgb(130, 96, 140), rgb(190, 96, 150), rgb(255, 214, 70),
     rgb(48, 46, 60), rgb(48, 46, 60), 0},
    // calico
    {rgb(255, 250, 246), rgb(52, 44, 56), rgb(52, 44, 56), rgb(255, 196, 210), rgb(255, 150, 176), rgb(52, 44, 56),
     rgb(240, 150, 70), rgb(60, 52, 58), 2},
};

// Body shapes. Coordinates are for the left half; the right half mirrors around x = 28.
struct CatShape {
  int8_t headY, headRx, headRy;      // head/body ellipse centre y and radii
  int8_t earBX, earBY, earTX, earTY, earIX, earIY;  // ear: outer base, tip, inner base
  int8_t eyeDX, eyeY;                // eyes at 28 -/+ eyeDX
  bool bigEyes;                      // sparkly chibi eyes, or plain dots
  int8_t blushDX, blushY, mouthY;
  int8_t pawX, pawUpY, pawR;         // left paw x; raised paw centre y and radius
};

static const CatShape SHAPES[] = {
    // chibi: big round head, big sparkly eyes
    {31, 18, 14, 11, 24, 14, 11, 22, 18, 8, 30, true, 11, 35, 35, 13, 38, 5},
    // chubby: wide and squishy, small ears
    {34, 22, 14, 8, 31, 10, 16, 19, 22, 9, 33, true, 12, 38, 38, 12, 40, 6},
    // original: a soft white mound with dot eyes, like the classic bongo cat
    {40, 23, 19, 9, 30, 12, 19, 19, 25, 8, 35, false, 13, 39, 39, 10, 36, 4},
};

enum Mode : uint8_t { IDLE, TYPING, MUSIC, SLEEP };

static TFT_eSprite cs(&tft), under(&tft), comp(&tft);
static uint16_t keyRaw;  // KEY as stored in the sprite buffer
static bool underDirty = true;
static int16_t cx = 0, cy = 0;  // top-left of the bubble
static Rect pendingOld = {0, 0, 0, 0};  // where the bubble was before it moved; repainted after the redraw
static Rect bigBox = {0, 0, 0, 0};
static uint16_t bigBg = 0;
static bool held = false, flying = false;
static int16_t grabDX, grabDY, flyFromX, flyFromY, flyToX, flyToY;
static uint32_t flyStart = 0, lastMove = 0;
static uint32_t pawLUntil = 0, pawRUntil = 0, lastTap = 0, lastFrame = 0, lastBeat = 0;
static uint32_t nextBlink = 0, blinkUntil = 0, lastPoke = 0, lastPlaying = 0, lastSig = UINT32_MAX;
static bool nextLeft = true;

static bool bigMode() { return !bigBox.empty(); }

Rect catRect() { return uiCatVisible() ? Rect{cx, cy, CAT, CAT} : Rect{0, 0, 0, 0}; }

bool catHit(int16_t x, int16_t y) {
  if (!uiCatVisible()) return false;
  int dx = x - (cx + CAT / 2), dy = y - (cy + CAT / 2);
  return dx * dx + dy * dy <= (CAT / 2 + 8) * (CAT / 2 + 8);  // a little slack for fat fingers
}

void catBackgroundChanged() {
  underDirty = true;
  lastSig = UINT32_MAX;
}

void catSetBig(const Rect& box, uint16_t bg) {
  bigBox = box;
  bigBg = bg;
  lastSig = UINT32_MAX;
}

static int16_t minX() { return MARGIN; }
static int16_t maxX() { return scrW - CAT - MARGIN; }
static int16_t minY() { return hdrH + MARGIN; }
static int16_t maxY() { return scrH - CAT - MARGIN; }

void catLayoutChanged() {
  bool portrait = scrH > scrW;
  if (cfg.catX < 0 || cfg.catY < 0) {
    cx = portrait ? maxX() : minX();  // portrait: top-right beside the art; landscape: under the art
    cy = portrait ? minY() : maxY();
  } else {
    cx = constrain(cfg.catX, minX(), maxX());
    cy = constrain(cfg.catY, minY(), maxY());
  }
  flying = held = false;
  pendingOld = {0, 0, 0, 0};
  catBackgroundChanged();
}

void catInit() {
  for (TFT_eSprite* s : {&cs, &under, &comp}) {
    s->setColorDepth(16);
    s->createSprite(CAT, CAT);
  }
  cs.fillSprite(KEY);
  keyRaw = ((uint16_t*)cs.getPointer())[0];
  nextBlink = millis() + 3000;
}

// ---------- drawing ----------

static void slap() {
  (nextLeft ? pawLUntil : pawRUntil) = millis() + 70;
  nextLeft = !nextLeft;
}

static void paw(const CatLook& k, const CatShape& s, int x, int by, bool down) {
  if (down) {
    cs.fillEllipse(x, 46, s.pawR + 1, 4, k.ink);
    cs.fillEllipse(x, 46, s.pawR, 3, k.fur);
    cs.drawLine(x - s.pawR - 2, 40, x - s.pawR - 4, 38, C_SUB);  // little impact sparks
    cs.drawLine(x + s.pawR + 2, 40, x + s.pawR + 4, 38, C_SUB);
  } else {
    cs.fillCircle(x, s.pawUpY + by, s.pawR, k.ink);
    cs.fillCircle(x, s.pawUpY + by, s.pawR - 1, k.fur);
    cs.fillCircle(x, s.pawUpY + 1 + by, 1, k.blush);  // toe bean
  }
}

static void note(int x, int y, uint16_t c) {
  cs.fillEllipse(x, y, 2, 2, c);
  cs.drawFastVLine(x + 2, y - 6, 6, c);
  cs.drawLine(x + 2, y - 6, x + 5, y - 4, c);
}

enum Eyes : uint8_t { OPEN, CLOSED, HAPPY, SQUINT, SURPRISED };

static void eyes(const CatLook& k, const CatShape& sh, Eyes e, int by) {
  for (int s = -1; s <= 1; s += 2) {
    int ex = 28 + s * sh.eyeDX, ey = sh.eyeY + by;
    switch (e) {
      case OPEN:
        if (sh.bigEyes) {  // big sparkly chibi eyes
          cs.fillEllipse(ex, ey, 3, 4, k.eye);
          if (k.eye != k.ink) cs.fillEllipse(ex, ey, 1, 3, k.ink);  // slit pupil for coloured eyes
          cs.fillCircle(ex + 1, ey - 2, 1, TFT_WHITE);
          cs.drawPixel(ex - 1, ey + 2, TFT_WHITE);
        } else {  // plain dots
          cs.fillRect(ex - 1, ey - 1, 3, 3, k.eye);
        }
        break;
      case CLOSED:
        cs.drawFastHLine(ex - 3, ey + 1, 7, k.line);
        if (sh.bigEyes) cs.drawFastHLine(ex - 2, ey + 2, 5, k.line);
        break;
      case HAPPY:  // ^ ^
        cs.drawLine(ex - 3, ey + 2, ex, ey - 1, k.line);
        cs.drawLine(ex, ey - 1, ex + 3, ey + 2, k.line);
        if (sh.bigEyes) {
          cs.drawLine(ex - 3, ey + 3, ex, ey, k.line);
          cs.drawLine(ex, ey, ex + 3, ey + 3, k.line);
        }
        break;
      case SQUINT: {  // > <
        int d = -s;
        cs.drawLine(ex - 3 * d, ey - 3, ex + 2 * d, ey, k.line);
        cs.drawLine(ex + 2 * d, ey, ex - 3 * d, ey + 3, k.line);
        break;
      }
      case SURPRISED:
        cs.drawCircle(ex, ey, 3, k.line);
        cs.drawPixel(ex, ey, k.line);
        break;
    }
  }
}

static void render(Mode m, bool pl, bool pr, bool bob, bool blink, uint8_t phase, bool big) {
  const CatLook& k = LOOKS[cfg.catStyle % 4];
  const CatShape& s = SHAPES[cfg.catShape % 3];
  const uint16_t DESK = rgb(92, 66, 50), DESK_TOP = rgb(140, 102, 76);
  int by = bob ? 1 : 0;
  uint32_t now = millis();
  bool surprised = lastPoke && now - lastPoke < 350;
  bool grabbed = held && !big;

  cs.fillSprite(big ? bigBg : C_CARD);
  cs.fillRect(0, 45, CAT, CAT - 45, DESK);
  cs.drawFastHLine(0, 45, CAT, DESK_TOP);

  // ears, then the head over their bases
  for (int side = -1; side <= 1; side += 2) {
    auto X = [&](int x) { return side < 0 ? x : 56 - x; };
    uint16_t earFur = side < 0 ? k.earL : k.earR;
    cs.fillTriangle(X(s.earBX - 1), s.earBY + 1 + by, X(s.earTX), s.earTY - 2 + by, X(s.earIX + 1), s.earIY + by, k.ink);
    cs.fillTriangle(X(s.earBX), s.earBY + by, X(s.earTX), s.earTY + by, X(s.earIX), s.earIY + by, earFur);
    cs.fillTriangle(X(s.earBX + 3), s.earBY - 3 + by, X(s.earTX + 1), s.earTY + 4 + by, X(s.earIX - 2), s.earIY + by, k.ear);
  }
  cs.fillEllipse(28, s.headY + by, s.headRx + 1, s.headRy + 1, k.ink);
  cs.fillEllipse(28, s.headY + by, s.headRx, s.headRy, k.fur);
  int top = s.headY - s.headRy + by;
  if (k.marks == 1) {  // tabby stripes on the forehead and cheeks
    uint16_t st = rgb(214, 116, 40);
    cs.fillRect(27, top + 2, 2, 5, st);
    cs.fillRect(22, top + 3, 2, 4, st);
    cs.fillRect(32, top + 3, 2, 4, st);
    for (int side = -1; side <= 1; side += 2) {
      int x = side < 0 ? 28 - s.headRx + 1 : 28 + s.headRx - 4;
      cs.drawFastHLine(x, s.eyeY + by, 4, st);
      cs.drawFastHLine(x, s.eyeY + 3 + by, 4, st);
    }
  } else if (k.marks == 2) {  // calico patches
    cs.fillEllipse(28 - s.headRx / 2, top + 7, 7, 5, k.earL);
    cs.fillEllipse(28 + s.headRx / 2, top + 6, 5, 4, k.earR);
  }
  cs.fillRect(28 - s.headRx + 4, 45, 2 * s.headRx - 8, 2, DESK_TOP);  // chin rests on the desk

  Eyes e = OPEN;
  if (grabbed) e = SQUINT;
  else if (surprised) e = SURPRISED;
  else if (m == SLEEP || blink) e = CLOSED;
  else if (m == MUSIC || m == TYPING) e = HAPPY;
  eyes(k, s, e, by);

  cs.fillEllipse(28 - s.blushDX, s.blushY + by, 3, 2, k.blush);
  cs.fillEllipse(28 + s.blushDX, s.blushY + by, 3, 2, k.blush);
  int my = s.mouthY + by;
  if (surprised) {
    cs.fillEllipse(28, my + 2, 2, 2, k.line);
  } else {  // tiny "w" mouth
    cs.drawLine(25, my, 26, my + 2, k.line);
    cs.drawLine(26, my + 2, 28, my, k.line);
    cs.drawLine(28, my, 30, my + 2, k.line);
    cs.drawLine(30, my + 2, 31, my, k.line);
  }

  paw(k, s, s.pawX, by, pl && !grabbed);
  paw(k, s, 56 - s.pawX, by, pr && !grabbed);

  if (m == MUSIC && !grabbed) {
    uint16_t c = uiAccent();
    note(7, 16 - phase / 3, c);
    note(46, 16 - ((phase + 12) % 25) / 3, c);
  } else if (m == SLEEP) {
    cs.setTextFont(1);
    cs.setTextDatum(TL_DATUM);
    cs.setTextColor(C_SUB);
    cs.drawString("z", 40 + phase / 6, 14 - phase / 3);
    cs.drawString("Z", 44 + phase / 8, 8 - phase / 4);
  }

  if (big) return;  // the big cat has no bubble

  // cut down to a circle, then ring it
  const float r = CAT / 2.0f;
  for (int y = 0; y < CAT; y++) {
    float dy = y + 0.5f - r;
    int half = (int)(sqrtf(max(0.0f, r * r - dy * dy)) + 0.5f);
    int edge = CAT / 2 - half;
    if (edge > 0) {
      cs.drawFastHLine(0, y, edge, KEY);
      cs.drawFastHLine(CAT - edge, y, edge, KEY);
    }
  }
  cs.drawCircle(CAT / 2, CAT / 2, CAT / 2 - 1, grabbed ? C_TXT : uiAccent());
}

static void present() {
  if (underDirty) {
    uiRenderUnder(under, catRect());
    underDirty = false;
  }
  uint16_t* c = (uint16_t*)cs.getPointer();
  uint16_t* u = (uint16_t*)under.getPointer();
  uint16_t* d = (uint16_t*)comp.getPointer();
  for (int i = 0; i < CAT * CAT; i++) d[i] = c[i] == keyRaw ? u[i] : c[i];
  comp.pushSprite(cx, cy);
}

// Scale the 56px frame up with chunky pixels into the big box.
static void presentBig() {
  int k = max(1, min(bigBox.w, bigBox.h) / CAT);
  int w = CAT * k;
  int ox = bigBox.x + (bigBox.w - w) / 2, oy = bigBox.y + (bigBox.h - w) / 2;
  static uint16_t rows[CAT * 4 * 4];  // up to 4x: one source row, k times over
  uint16_t* src = (uint16_t*)cs.getPointer();
  k = min(k, 4);
  for (int y = 0; y < CAT; y++) {
    uint16_t* line = rows;
    for (int x = 0; x < CAT; x++)
      for (int i = 0; i < k; i++) *line++ = src[y * CAT + x];
    for (int j = 1; j < k; j++) memcpy(rows + j * w, rows, w * 2);
    tft.pushImage(ox, oy + y * k, w, k, rows);  // sprite pixels are already in panel byte order
  }
}

// ---------- movement ----------

static void restoreDifference(const Rect& o, const Rect& n) {
  if (!o.overlaps(n)) {
    uiRestore(o);
    return;
  }
  // only repaint the strips the bubble just uncovered
  if (n.y > o.y) uiRestore({o.x, o.y, o.w, (int16_t)(n.y - o.y)});
  else if (n.y < o.y) uiRestore({o.x, (int16_t)(n.y + n.h), o.w, (int16_t)(o.y - n.y)});
  int16_t ty = max(o.y, n.y), th = min(o.y + o.h, n.y + n.h) - ty;
  if (n.x > o.x) uiRestore({o.x, ty, (int16_t)(n.x - o.x), th});
  else if (n.x < o.x) uiRestore({(int16_t)(n.x + n.w), ty, (int16_t)(o.x - n.x), th});
}

// Moves only the position; the next catTick draws the cat at the new spot first and then
// cleans up the spot it left, so the old bubble never shows as a square.
static void moveTo(int nx, int ny) {
  nx = constrain(nx, 0, scrW - CAT);
  ny = constrain(ny, hdrH, scrH - CAT);
  if (nx == cx && ny == cy) return;
  if (pendingOld.empty()) pendingOld = catRect();
  cx = nx;
  cy = ny;
  catBackgroundChanged();
}

void catPoke() {  // startle, then a flurry of slaps
  pendingTaps = 6;
  lastPoke = millis();
  lastSig = UINT32_MAX;
}

void catDragStart(int16_t x, int16_t y) {
  grabDX = x - cx;
  grabDY = y - cy;
  held = true;
  flying = false;
  lastSig = UINT32_MAX;
}

void catDragMove(int16_t x, int16_t y) {
  if (millis() - lastMove < 20) return;
  lastMove = millis();
  moveTo(x - grabDX, y - grabDY);
  catTick(true);
}

void catDragEnd(bool moved) {
  held = false;
  lastSig = UINT32_MAX;
  if (!moved) {  // a tap
    catPoke();
    return;
  }
  // like a chat head: fly to the nearest side, and into a corner if close to one
  flyFromX = cx;
  flyFromY = cy;
  flyToX = cx + CAT / 2 < scrW / 2 ? minX() : maxX();
  flyToY = constrain(cy, minY(), maxY());
  if (flyToY - minY() < 40) flyToY = minY();
  else if (maxY() - flyToY < 40) flyToY = maxY();
  flyStart = millis();
  flying = true;
}

// A small face for the style picker on the settings page
void catDrawSwatch(TFT_eSPI& g, int x, int y, int r, uint8_t style) {
  const CatLook& k = LOOKS[style % 4];
  g.fillTriangle(x - r + 2, y - r / 3, x - r / 2, y - r - 2, x - 1, y - r + 2, k.earL);
  g.fillTriangle(x + r - 2, y - r / 3, x + r / 2, y - r - 2, x + 1, y - r + 2, k.earR);
  g.fillCircle(x, y, r, k.ink);
  g.fillCircle(x, y, r - 1, k.fur);
  if (k.marks == 1) g.fillRect(x - 1, y - r + 2, 2, r / 2, rgb(214, 116, 40));
  if (k.marks == 2) {
    g.fillCircle(x - r / 2, y - r / 3, r / 3, k.earL);
    g.fillCircle(x + r / 2, y - r / 2, r / 4, k.earR);
  }
  g.fillCircle(x - r / 3, y + 1, 2, k.eye);
  g.fillCircle(x + r / 3, y + 1, 2, k.eye);
  g.fillCircle(x - r / 2 - 1, y + r / 3 + 1, 2, k.blush);
  g.fillCircle(x + r / 2 + 1, y + r / 3 + 1, 2, k.blush);
}

// ---------- tick ----------

void catTick(bool force) {
  bool big = bigMode();
  if (!big && !uiCatVisible()) return;
  uint32_t now = millis();
  if (flying && !big) {
    float t = (now - flyStart) / 300.0f;
    if (t >= 1) {
      flying = false;
      moveTo(flyToX, flyToY);
      cfg.catX = cx;
      cfg.catY = cy;
      configSave();
    } else {
      float e = 1 - powf(1 - t, 3);  // ease-out
      moveTo(flyFromX + (flyToX - flyFromX) * e, flyFromY + (flyToY - flyFromY) * e);
    }
    force = true;
  }
  if (!force && now - lastFrame < 30) return;
  lastFrame = now;

  lockState();
  bool playing = np.valid && np.playing;
  unlockState();
  if (playing) lastPlaying = now;
  uint32_t lastAct = max(max((uint32_t)lastKeyMs, lastPoke), lastPlaying);

  Mode m;
  if ((lastKeyMs && now - lastKeyMs < 1500) || (lastPoke && now - lastPoke < 1500)) m = TYPING;
  else if (playing) m = MUSIC;
  else if (now - lastAct > 120000 && !held) m = SLEEP;
  else m = IDLE;

  if (pendingTaps && now - lastTap > 70 && !(lastPoke && now - lastPoke < 350)) {
    pendingTaps = pendingTaps - 1;
    lastTap = now;
    slap();
  }
  bool bob = false;
  if (m == MUSIC) {
    if (now - lastBeat > 480) {  // ~125 bpm bop with a gentle paw
      lastBeat = now;
      slap();
    }
    bob = now - lastBeat < 140;
  }
  if ((int32_t)(now - nextBlink) >= 0) {
    blinkUntil = now + 140;
    nextBlink = now + 2500 + random(3000);
  }
  bool blink = (int32_t)(blinkUntil - now) > 0;
  bool pl = (int32_t)(pawLUntil - now) > 0, pr = (int32_t)(pawRUntil - now) > 0;
  bool surprised = lastPoke && now - lastPoke < 350;
  uint8_t phase = m == MUSIC ? (now / 60) % 25 : m == SLEEP ? (now / 150) % 20 : 0;

  uint32_t sig = m | pl << 2 | pr << 3 | bob << 4 | blink << 5 | held << 6 | surprised << 7 | phase << 8;
  if (!force && sig == lastSig) return;
  lastSig = sig;
  render(m, pl, pr, bob, blink, phase, big);
  if (big) {
    presentBig();
    return;
  }
  present();
  if (!pendingOld.empty()) {
    Rect o = pendingOld;
    pendingOld = {0, 0, 0, 0};
    restoreDifference(o, catRect());
  }
}
