// =============================================
// BTCWallet_Display.h
// Brilho + Controle RGB LED Traseiro
// =============================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

#ifndef C_BG
  #define C_BG      0x0000
#endif
#ifndef C_WHITE
  #define C_WHITE   0xFFFF
#endif
#ifndef C_GOLD
  #define C_GOLD    0xFEA0
#endif
#ifndef C_DGRAY
  #define C_DGRAY   0x39E7
#endif
#ifndef C_PANEL
  #define C_PANEL   0x1082
#endif
#ifndef C_SURFACE
  #define C_SURFACE 0x18C3
#endif
#ifndef C_SURFACE2
  #define C_SURFACE2 0x2104
#endif

// ── Backlight ─────────────────────────────────
#define BL_PIN        21
#define BL_LEDC_FREQ 5000
#define BL_LEDC_BITS  8
#define BL_DEFAULT    200

// ── RGB LED Pins ──────────────────────────────
#define LED_R_PIN     4
#define LED_G_PIN    16
#define LED_B_PIN    17

// Global states
static uint8_t g_brightness = BL_DEFAULT;
static bool    g_rgbEnabled = true;
static uint8_t g_ledColorPos = 0;   // 0=Red, 85=Green, 170=Blue

// ───────────────────────────────────────────────
inline void displayInit() {
  ledcSetup(0, BL_LEDC_FREQ, BL_LEDC_BITS);   // canal 0
  ledcAttachPin(BL_PIN, 0);
  ledcWrite(0, BL_DEFAULT);
  g_brightness = BL_DEFAULT;
}

inline void setBrightness(uint8_t val) {
  g_brightness = val;
  ledcWrite(0, val);
}

// ───────────────────────────────────────────────
inline void setLedColor(uint8_t pos) {
  g_ledColorPos = pos;

  // Se o LED estiver desligado, apenas salva a posição sem acender
  if (!g_rgbEnabled) return;

  uint8_t r = 0, g = 0, b = 0;

  if (pos < 85) {           // Red → Green
    r = 255 - pos * 3;
    g = pos * 3;
  } else if (pos < 170) {   // Green → Blue
    uint8_t p = pos - 85;
    g = 255 - p * 3;
    b = p * 3;
  } else {                  // Blue → Red
    uint8_t p = pos - 170;
    r = p * 3;
    b = 255 - p * 3;
  }

  ledcWrite(1, r);
  ledcWrite(2, g);
  ledcWrite(3, b);
}

inline void rgbLedInit() {
  ledcSetup(1, 5000, 8);  ledcAttachPin(LED_R_PIN, 1);
  ledcSetup(2, 5000, 8);  ledcAttachPin(LED_G_PIN, 2);
  ledcSetup(3, 5000, 8);  ledcAttachPin(LED_B_PIN, 3);
  setLedColor(0);        // Start with Red
}

inline void setRGBEnabled(bool enabled) {
  g_rgbEnabled = enabled;
  if (!enabled) {
    ledcWrite(1, 0);
    ledcWrite(2, 0);
    ledcWrite(3, 0);
  } else {
    setLedColor(g_ledColorPos);
  }
}

// ───────────────────────────────────────────────
// Draw brightness bar (used in Settings only now)
// ───────────────────────────────────────────────
inline void drawBrightnessBar(TFT_eSPI& tft, bool big = false) {
  int screenW = tft.width();
  int screenH = tft.height();
  int w = big ? min(280, screenW - 40) : screenW - 8;
  int x = big ? (screenW - w) / 2 : 4;
  int y = big ? 130 : screenH - 15;
  int h = big ? 28 : 14;

  if (big) {
    tft.fillRoundRect(x - 6, y - 14, w + 12, h + 32, 12, C_PANEL);
    tft.drawRoundRect(x - 6, y - 14, w + 12, h + 32, 12, C_SURFACE2);
  } else {
    tft.fillRect(x - 4, y - 4, w + 8, h + 30, C_BG);
  }

  tft.fillCircle(x + 10, y + h/2, 7, C_GOLD);
  tft.fillCircle(x + 13, y + h/2 - 2, 5, C_BG);

  int sx = x + w - 10;
  tft.fillCircle(sx, y + h/2, 5, C_GOLD);
  tft.drawFastHLine(sx-9, y+h/2, 4, C_GOLD);
  tft.drawFastHLine(sx+6, y+h/2, 4, C_GOLD);
  tft.drawFastVLine(sx, y+h/2-9, 4, C_GOLD);
  tft.drawFastVLine(sx, y+h/2+6, 4, C_GOLD);

  int trackX = x + 22;
  int trackW = w - 44;
  int trackY = y + (h - 8) / 2;
  tft.fillRoundRect(trackX, trackY, trackW, 8, 4, C_SURFACE2);
  tft.drawRoundRect(trackX, trackY, trackW, 8, 4, C_DGRAY);

  int fillW = (long)g_brightness * trackW / 255;
  if (fillW > 0) {
    tft.fillRoundRect(trackX, trackY, fillW, 8, 4, C_GOLD);
  }

  int knobX = constrain(trackX + fillW, trackX + 6, trackX + trackW - 6);
  int knobR = big ? 9 : 5;
  tft.fillCircle(knobX, y + h / 2, knobR, C_WHITE);
  tft.drawCircle(knobX, y + h / 2, knobR, C_GOLD);
}

// ───────────────────────────────────────────────
// Touch handler for brightness
// ───────────────────────────────────────────────
inline bool touchBrightnessBar(TFT_eSPI& tft, int x, int y, bool big = false) {
  int screenW = tft.width();
  int screenH = tft.height();
  int barY = big ? 130 : screenH - 15;
  int barH = big ? 40 : 20;
  if (y < barY-5 || y > barY + barH) return false;

  int barW = big ? min(280, screenW - 40) : screenW - 40;
  int barX = big ? (screenW - barW) / 2 : 4;
  int trackX = barX + 22;
  int trackW = barW - 44;
  int relX = constrain(x - trackX, 0, trackW);
  uint8_t newB = (uint8_t)((long)relX * 255 / trackW);
  if (newB < 10) newB = 10;
  setBrightness(newB);
  return true;
}
