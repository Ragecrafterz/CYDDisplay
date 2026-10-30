#pragma once
#include <TFT_eSPI.h>
#include "state.h"

extern TFT_eSPI tft;

// Theme colours, set from the companion app
#define C_BG (cfg.theme[TH_BG])
#define C_BAR (cfg.theme[TH_BAR])
#define C_CARD (cfg.theme[TH_CARD])
#define C_LINE (cfg.theme[TH_LINE])
#define C_TXT (cfg.theme[TH_TXT])
#define C_SUB (cfg.theme[TH_SUB])
#define C_DIM (cfg.theme[TH_DIM])
#define C_ACC (cfg.theme[TH_ACC])

struct Rect {
  int16_t x, y, w, h;
  bool empty() const { return w <= 0 || h <= 0; }
  bool overlaps(const Rect& o) const {
    return !empty() && !o.empty() && x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
  }
};

extern int16_t scrW, scrH, hdrH;

uint8_t luma(uint16_t c);  // 0..255 brightness of an RGB565 colour

// ui.cpp
void uiInit();
void uiApplyRotation();
void uiThemeChanged();
void uiTick();
void uiTap(int16_t x, int16_t y);
void uiSwipe(int dir);
bool uiDragStart(int16_t x, int16_t y);  // true if the touch grabbed a control (brightness slider)
void uiDragMove(int16_t x, int16_t y);
void uiDragEnd();
bool uiCatVisible();
void uiRedrawAll();
void uiRestore(const Rect& r);  // repaint the page on screen inside r (the cat moved off it)
void uiRenderUnder(TFT_eSprite& spr, const Rect& r);  // paint the page inside r into a sprite
uint16_t uiAccent();
int16_t uiArtSize();
void drawWxIcon(TFT_eSPI& g, int cx, int cy, int s, uint8_t code, bool day, uint16_t bg);

// cat.cpp
void catInit();
void catLayoutChanged();
void catBackgroundChanged();
void catTick(bool force);
Rect catRect();
bool catHit(int16_t x, int16_t y);
void catDragStart(int16_t x, int16_t y);
void catDragMove(int16_t x, int16_t y);
void catDragEnd(bool moved);
void catPoke();
void catDrawSwatch(TFT_eSPI& g, int x, int y, int r, uint8_t style);
void catSetBig(const Rect& box, uint16_t bg);  // empty box: back to the bubble
