/*
Bitcoin HD Wallet - ESP32-2432S028R (CYD)
Touch VSPI: CLK=25, MOSI=32, MISO=39, CS=33
ORIENTAÇÃO: Portrait 240×320 (rotation 0)
*/
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Bitcoin.h>
#include <Hash.h>
#include <Conversion.h>
#include "BTCWallet_SD.h"
#include "BTCWallet_QRCode.h"
#include "BTCWallet_Display.h"
#include "BTCWallet_BIP39.h"

// Touch — VSPI
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39

// Cores
#define C_BG       0x0000
#define C_ORANGE   0xFD20
#define C_GOLD     0xFEA0
#define C_WHITE    0xFFFF
#define C_GRAY     0x7BEF
#define C_DGRAY    0x39E7
#define C_PANEL    0x1082
#define C_GREEN    0x07E0
#define C_RED      0xF800
#define C_BLUE     0x041F
#define C_TEAL     0x07FF
#define C_YELLOW   0xFFE0
#define C_PURPLE   0x801F
#define C_SURFACE  0x18C3
#define C_SURFACE2 0x2104
#define C_LINE     0x31A6
#define C_TEXTDIM  0xA514
#define C_SOFT     0x632C
#define C_CREAM    0xFF7A

// ── Dimensões Portrait ────────────────────────
static constexpr int SW = 240;   // Screen Width
static constexpr int SH = 320;   // Screen Height

// ── Calibração touch Portrait (CYD rotation 0) ──
// Baseada nos seus dados reais:
// canto sup-dir: RAW X=346  Y=3518  → MAP X=239 Y=0
// canto inf-dir: RAW X=398  Y=528   → MAP X=238 Y=319
// canto inf-esq: RAW X=3628 Y=392   → MAP X=0   Y=319
// canto sup-esq: RAW X=3610 Y=3572  → MAP X=0   Y=0
// Calibração medida nos 4 cantos reais do hardware:
// sup-dir(24): RAW X=3375 Y=460  → tela X=239 Y=0
// inf-dir(cfg): RAW X=3691 Y=3658 → tela X=239 Y=319
// inf-esq(cfg): RAW X=515  Y=3657 → tela X=0   Y=319
// sup-esq(12):  RAW X=630  Y=399  → tela X=0   Y=0
static constexpr int TX_MIN = 515;   // borda esquerda
static constexpr int TX_MAX = 3691;  // borda direita
static constexpr int TY_TOP = 399;   // borda superior (Y RAW baixo = topo)
static constexpr int TY_BOT = 3658;  // borda inferior (Y RAW alto = base)

static int mapTouchX(int rawX) {
  return constrain(map(rawX, TX_MIN, TX_MAX, 0, SW - 1), 0, SW - 1);
}
static int mapTouchY(int rawY) {
  // Y invertido: RAW alto = base da tela, RAW baixo = topo
  return constrain(map(rawY, TY_BOT, TY_TOP, SH - 1, 0), 0, SH - 1);
}

// ─────────────────────────────────────────────
uint16_t darken(uint16_t c) {
  uint8_t r = (c >> 11) & 0x1F;
  uint8_t g = (c >> 5)  & 0x3F;
  uint8_t b =  c        & 0x1F;
  r = r > 6 ? r - 6 : 0;
  g = g > 12 ? g - 12 : 0;
  b = b > 6 ? b - 6 : 0;
  return (r << 11) | (g << 5) | b;
}

uint16_t lighten(uint16_t c, uint8_t amount = 4) {
  uint8_t r = (c >> 11) & 0x1F;
  uint8_t g = (c >> 5)  & 0x3F;
  uint8_t b =  c        & 0x1F;
  r = min(31, r + amount);
  g = min(63, g + amount * 2);
  b = min(31, b + amount);
  return (r << 11) | (g << 5) | b;
}

SPIClass touchSPI(VSPI);
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

enum Screen {
  SCR_HOME, SCR_CHOOSE_WORDS, SCR_SHOW_SEED,
  SCR_RECOVER_INTRO, SCR_KEYBOARD, SCR_PASSPHRASE,
  SCR_ACCOUNT,       // ← NOVO: seleção do account de derivação
  SCR_ADDRESSES,
  SCR_GENKEY, SCR_QRCODE, SCR_SETTINGS
};
Screen currentScreen = SCR_HOME;
bool g_isRecovery = false;
int g_numWords = 12;
String g_mnemonic = "";
String g_passphrase = "";

enum KeyboardTarget { KB_PASSPHRASE, KB_RECOVER_WORD, KB_GENKEY, KB_ACCOUNT };
KeyboardTarget g_kbTarget = KB_PASSPHRASE;
Screen g_kbPrevScreen = SCR_HOME;

String g_keyPassword = "";
int g_recoverWordIdx = 0;
String g_recoverWords[24];
String g_currentInput = "";
static const char* g_suggestions[BIP39_SUGGEST_MAX];
static uint8_t     g_suggestCount = 0;
static bool        g_suggestVisible = false;
String g_addrP2PKH = "";
String g_addrP2SH  = "";
String g_addrBech32 = "";

enum KbLayout { KB_LOWER, KB_UPPER, KB_NUMBERS };
KbLayout g_kbLayout = KB_LOWER;
int g_addrScroll = 0;
int g_seedTab  = 0;  // 0 = palavras 1-12, 1 = palavras 13-24 (só para 24 palavras)
int g_seedZoom = 1;  // 1 = textSize(1) palavras completas | 2 = textSize(2) ampliado

// ── Account / Derivação ───────────────────────────────────────────────────────
// Usuário escolhe na tela SCR_ACCOUNT antes de derivar.
// Paths usados: m/44'/0'/0'/0/X  m/49'/0'/0'/0/X  m/84'/0'/0'/0/X
// onde X = g_account (0..99)
static int g_account = 0;              // account selecionado
static String g_wifP2PKH   = "";       // WIF BIP44 — salvo no SD
static String g_wifP2SH    = "";       // WIF BIP49 — salvo no SD
static String g_wifBech32  = "";       // WIF BIP84 — salvo no SD

// Forward declarations
void drawHeader(const char* t, uint16_t bg);
void drawBtn(int x, int y, int w, int h, const char* lbl, uint16_t bg, uint16_t fg);
void drawSuggBtn(int x, int y, int w, int h, const char* lbl, uint16_t bg);
void flashBtn(int x, int y, int w, int h, const char* lbl, uint16_t bg, uint16_t fg);
void drawBackArrow(uint16_t col = C_GOLD);
void drawBootAnimation();
void drawHome();
void drawChooseWords();
void drawShowSeed();
void drawRecoverIntro();
void drawKeyboard();
void drawPassphraseScreen();
void drawAccountScreen();       // ← NOVO
void drawAddresses();
void drawGenKey();
void drawSettings();
void showQRCode(int addrIdx);
void handleTouch(int x, int y);
void doGenerate();
void doDerive();
void kbHandleKey(const char* key);
void kbRedrawInput();
void kbUpdateSuggestions();
void kbDrawSuggestions();
void kbClearSuggestions();
static String normalizeBip39Word(String word);
static int bip39Index(const String& word);
static bool validateRecoveryMnemonic(String& errorMsg);
static bool commitRecoveryWord(const String& typedWord);
static void showKeyboardMessage(const char* title, uint16_t color,
                                const String& line1, const String& line2 = "");
static void redrawSettingsBar();
static bool isPointInRect(int px, int py, int x, int y, int w, int h);
static bool isBrightnessTouchPoint(int x, int y);

// ─────────────────────────────────────────────────────────────────────────────
// Boot Animation — Portrait 240×320
// Logo Bitcoin no centro, trilhas de circuito nas bordas
// ─────────────────────────────────────────────────────────────────────────────
static void animLine(int x0, int y0, int x1, int y1, int x2, int y2,
                     uint16_t col, int spd = 4) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = max(abs(dx), abs(dy)) / max(spd, 1);
  for (int i = 0; i <= steps; i++) {
    int px = x0 + dx * i / max(steps, 1);
    int py = y0 + dy * i / max(steps, 1);
    tft.drawPixel(px, py, col);
    tft.drawPixel(px + 1, py, col);
    tft.drawPixel(px, py + 1, col);
  }
  delay(spd);
  dx = x2 - x1; dy = y2 - y1;
  steps = max(abs(dx), abs(dy)) / max(spd, 1);
  for (int i = 0; i <= steps; i++) {
    int px = x1 + dx * i / max(steps, 1);
    int py = y1 + dy * i / max(steps, 1);
    tft.drawPixel(px, py, col);
    tft.drawPixel(px + 1, py, col);
    tft.drawPixel(px, py + 1, col);
  }
  delay(spd);
}

static void animDot(int x, int y, uint16_t col) {
  tft.fillCircle(x, y, 3, col);
  delay(8);
}

void drawBootAnimation() {
  // Centro da tela portrait: 120, 150
  const int CX = SW / 2;       // 120
  const int CY = SH / 2 - 10;  // 150
  const uint16_t TRAIL = C_WHITE;
  const uint16_t DIM   = 0x8400;
  const uint16_t RING  = C_BG;

  tft.fillScreen(C_BG);  //MUDA A COR DO PLANO FUNDO DO BOOT INICIAL

  // ── Fase 1: trilhas das bordas convergem para o centro ────────────────────

  // Borda esquerda
  animLine(0, 60,  40, 60,  40, 100,  TRAIL, 3);
  animDot(2, 60, DIM);
  animLine(0, 90,  25, 90,  25, 115,  DIM, 3);
  animDot(2, 90, DIM);

  animLine(0, 200, 40, 200, 40, 175,  TRAIL, 3);
  animDot(2, 200, DIM);
  animLine(0, 230, 28, 230, 28, 200,  DIM, 3);
  animDot(2, 230, DIM);

  // Borda direita
  animLine(SW, 55,  SW-40, 55,  SW-40, 100,  TRAIL, 3);
  animDot(SW-3, 55, DIM);
  animLine(SW, 85,  SW-28, 85,  SW-28, 115,  DIM, 3);
  animDot(SW-3, 85, DIM);

  animLine(SW, 205, SW-40, 205, SW-40, 175,  TRAIL, 3);
  animDot(SW-3, 205, DIM);
  animLine(SW, 235, SW-28, 235, SW-28, 200,  DIM, 3);
  animDot(SW-3, 235, DIM);

  // Borda superior
  animLine(50,  0,  50,  40,  85,  40,   TRAIL, 3);
  animDot(50, 2, DIM);
  animLine(120, 0,  120, 50,  105, 50,   DIM, 3);
  animDot(120, 2, DIM);
  animLine(185, 0,  185, 40,  155, 40,   TRAIL, 3);
  animDot(185, 2, DIM);

  // Borda inferior
  animLine(45,  SH,  45,  SH-45, 80,  SH-45,  TRAIL, 3);
  animDot(45, SH-3, DIM);
  animLine(120, SH, 120, SH-50, 105, SH-50,  DIM, 3);
  animDot(120, SH-3, DIM);
  animLine(190, SH, 190, SH-45, 158, SH-45,  TRAIL, 3);
  animDot(190, SH-3, DIM);

  // ── Fase 2: terminais piscam ──────────────────────────────────────────────
  int pts[][2] = {
    {40,100},{40,175},{SW-40,100},{SW-40,175},
    {85,40},{155,40},{80,SH-45},{158,SH-45}
  };
  for (int p = 0; p < 3; p++) {
    for (auto& pt : pts)
      tft.fillCircle(pt[0], pt[1], 3, (p % 2 == 0) ? TRAIL : DIM);
    delay(110);
  }

  // ── Fase 3: anel externo cresce ──────────────────────────────────────────
  for (int r = 10; r <= 58; r += 2) {
    tft.drawCircle(CX, CY, r, (r % 8 < 4) ? TRAIL : DIM);
    delay(6);
  }

  // ── Fase 4: logo BTC aparece ─────────────────────────────────────────────
  for (int r = 54; r >= 2; r -= 2) { tft.fillCircle(CX, CY, r, C_BG); delay(2); }
  tft.fillCircle(CX, CY, 58, C_BG);
  tft.drawCircle(CX, CY, 52, DIM);
  tft.drawCircle(CX, CY, 53, TRAIL);
  tft.drawCircle(CX, CY, 54, TRAIL);
  tft.drawCircle(CX, CY, 55, TRAIL);
  tft.drawCircle(CX, CY, 56, TRAIL);
  tft.drawCircle(CX, CY, 57, TRAIL);
  tft.drawCircle(CX, CY, 58, DIM);

  tft.setTextSize(5);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(CX - 42, CY - 20);
  tft.print("BTC");

  // ── Fase 5: título aparece letra a letra ─────────────────────────────────
  delay(200);
  tft.setTextColor(TRAIL, C_BG);
  tft.setTextSize(2);
  const char* title = " BTC HD Wallet";
  int titX = (SW - (int)strlen(title) * 12) / 2;
  for (int i = 0; title[i]; i++) {
    tft.setCursor(titX + i * 12, CY + 75);
    tft.print(title[i]);
    delay(65);
  }

  // ── Fase 6: pulsa anel 2x ────────────────────────────────────────────────
  for (int p = 0; p < 2; p++) {
    for (int r = 58; r <= 63; r++) { tft.drawCircle(CX, CY, r, TRAIL); delay(15); }
    for (int r = 63; r >= 58; r--) { tft.drawCircle(CX, CY, r, C_BG);  delay(200); }
  }

}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println(">>> SETUP INICIADO - Portrait 240x320");

  tft.init();
  tft.setRotation(0);   // ← Portrait
  tft.fillScreen(C_BG);

  displayInit();
  rgbLedInit();

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSPI);
  touch.setRotation(0); // ← Portrait

  Serial.println(">>> TFT & TOUCH INICIALIZADOS");

  drawBootAnimation();
  drawHome();
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  static bool touchHeld = false;
  static unsigned long lastTouch = 0;

  if (!touch.touched()) { touchHeld = false; return; }

  TS_Point p = touch.getPoint();
  int tx = mapTouchX(p.x);
  int ty = mapTouchY(p.y);

  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 500) {
    Serial.printf("RAW: X=%d Y=%d -> MAP: X=%d Y=%d\n", p.x, p.y, tx, ty);
    lastDebug = millis();
  }

  bool allowRepeat = (currentScreen == SCR_SETTINGS) && isBrightnessTouchPoint(tx, ty);
  if (touchHeld && !allowRepeat) return;

  unsigned long debounceMs = allowRepeat ? 35 : 140;
  if (millis() - lastTouch < debounceMs) return;
  lastTouch = millis();
  touchHeld = true;

  handleTouch(tx, ty);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers de desenho (adaptados para 240 px de largura)
// ─────────────────────────────────────────────────────────────────────────────
void drawHeader(const char* t, uint16_t bg) {
  tft.fillScreen(C_BG);
  // Decoração de fundo
  tft.fillCircle(210, 8, 60, darken(darken(bg)));
  tft.fillCircle(18, SH - 30, 44, C_SURFACE);
  tft.fillCircle(220, SH - 22, 30, C_SURFACE2);

  for (int x = 8; x < SW; x += 24)
    tft.drawFastHLine(x, 46, 10, C_SURFACE2);

  tft.fillRect(0, 0, SW, 44, bg);
  uint16_t lineCol = (bg == C_WHITE || bg == C_YELLOW) ? C_GOLD : lighten(bg, 2);
  tft.fillRect(0, 40, SW, 4, lineCol);

  String title = String(t);
  title.trim();
  uint16_t titleFg = (bg == C_WHITE || bg == C_YELLOW) ? C_BG : C_WHITE;
  int tw = title.length() * 12;
  int tx2 = max(44, (SW - tw) / 2);

  tft.setTextColor(titleFg, bg);
  tft.setTextSize(2);
  tft.setCursor(tx2, 12);
  tft.print(title);

//  uint16_t brandFg = (bg == C_WHITE || bg == C_WHITE) ? C_PANEL : lighten(bg, 4); // DISNECESSARIO
//  tft.setTextColor(brandFg, bg);
//  tft.setTextSize(1);
//  tft.setCursor(194, 6);
//  tft.print("BTC");
}

void drawCard(int x, int y, int w, int h, uint16_t fill = C_SURFACE, uint16_t border = C_LINE) {
  int radius = (h >= 46) ? 10 : 8;
  tft.fillRoundRect(x + 2, y + 3, w, h, radius, C_SURFACE2);
  tft.fillRoundRect(x, y, w, h, radius, fill);
  tft.drawRoundRect(x, y, w, h, radius, border);
  if (w > 24) tft.drawFastHLine(x + 10, y + 6, w - 20, lighten(fill, 2));
}

void drawBadge(int x, int y, int w, const char* lbl, uint16_t bg, uint16_t fg) {
  tft.fillRoundRect(x, y, w, 16, 8, bg);
  tft.drawRoundRect(x, y, w, 16, 8, lighten(bg, 2));
  tft.setTextColor(fg, bg);
  tft.setTextSize(1);
  int tw = strlen(lbl) * 6;
  tft.setCursor(x + (w - tw) / 2, y + 4);
  tft.print(lbl);
}

void drawFooterHint(const char* txt) {
  drawCard(6, SH - 18, SW - 12, 14, C_SURFACE, C_SURFACE2);
  tft.setTextColor(C_TEXTDIM, C_SURFACE);
  tft.setTextSize(1);
  int tw = strlen(txt) * 6;
  int tx2 = max(10, (SW - tw) / 2);
  tft.setCursor(tx2, SH - 14);
  tft.print(txt);
}

void drawBtn(int x, int y, int w, int h, const char* lbl, uint16_t bg, uint16_t fg) {
  int radius = (h >= 46) ? 10 : 8;
  uint16_t border = (bg == C_WHITE) ? C_GOLD : lighten(bg, 3);
  tft.fillRoundRect(x + 2, y + 3, w, h, radius, darken(darken(bg)));
  tft.fillRoundRect(x, y, w, h, radius, bg);
  tft.drawRoundRect(x, y, w, h, radius, border);
  if (w > 26) tft.drawFastHLine(x + 9, y + 5, w - 18, lighten(bg, 3));
  tft.setTextColor(fg, bg);
  uint8_t sz = (strlen(lbl) <= 4 && h >= 30) ? 2 : 1;
  tft.setTextSize(sz);
  int tw = strlen(lbl) * 6 * sz;
  int th = 8 * sz;
  tft.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  tft.print(lbl);
}

void drawSuggBtn(int x, int y, int w, int h, const char* lbl, uint16_t bg) {
  int radius = 7;
  uint16_t border = lighten(bg, 3);
  tft.fillRoundRect(x, y + 2, w, h, radius, darken(darken(bg)));
  tft.fillRoundRect(x, y,     w, h, radius, bg);
  tft.drawRoundRect(x, y,     w, h, radius, border);
  if (w > 20) tft.drawFastHLine(x + 6, y + 4, w - 12, lighten(bg, 3));
  int len = strlen(lbl);
  uint8_t sz = ((len * 12) <= w - 8) ? 2 : 1;
  int tw = len * 6 * sz;
  int th = 8 * sz;
  tft.setTextColor(C_BG, bg);
  tft.setTextSize(sz);
  tft.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  tft.print(lbl);
}

void drawPressedBtn(int x, int y, int w, int h, const char* lbl, uint16_t bg, uint16_t fg) {
  uint16_t pressedBg = darken(bg);
  int radius = (h >= 46) ? 10 : 8;
  uint16_t border = (pressedBg == C_WHITE) ? C_GOLD : lighten(pressedBg, 2);
  tft.fillRoundRect(x + 1, y + 1, w, h, radius, darken(darken(pressedBg)));
  tft.fillRoundRect(x, y, w, h, radius, pressedBg);
  tft.drawRoundRect(x, y, w, h, radius, border);
  if (w > 26) tft.drawFastHLine(x + 9, y + 4, w - 18, lighten(pressedBg, 2));
  tft.setTextColor(fg, pressedBg);
  uint8_t sz = (strlen(lbl) <= 4 && h >= 30) ? 2 : 1;
  tft.setTextSize(sz);
  int tw = strlen(lbl) * 6 * sz;
  int th = 8 * sz;
  tft.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  tft.print(lbl);
}

void flashBtn(int x, int y, int w, int h, const char* lbl, uint16_t bg, uint16_t fg) {
  drawPressedBtn(x, y, w, h, lbl, bg, fg);
  delay(35);
  drawBtn(x, y, w, h, lbl, bg, fg);
}

static bool isPointInRect(int px, int py, int x, int y, int w, int h) {
  return (px >= x && px <= (x + w) && py >= y && py <= (y + h));
}

static bool isBrightnessTouchPoint(int x, int y) {
  // Barra de brilho em portrait: y≈130, largura 200px centrada
  return isPointInRect(x, y, 20, 120, 200, 45);
}

void drawBackArrow(uint16_t col) {
  drawCard(5, 5, 30, 30, C_BG, C_SURFACE);
  tft.fillTriangle(11, 20, 22, 12, 22, 28, col);
  tft.drawLine(11, 20, 26, 20, C_WHITE);
  tft.drawLine(11, 20, 22, 12, C_WHITE);
  tft.drawLine(11, 20, 22, 28, C_WHITE);
}

// ─────────────────────────────────────────────────────────────────────────────
// HOME — Metro UI  (Windows Phone style)
// Tela 240×320 | Header preto y=0..50 | Tiles y=56..316
//
//  Tile 12 WORDS  : x=4   y=56  w=113 h=113  azul   #0078D7
//  Tile 24 WORDS  : x=123 y=56  w=113 h=113  azul e #005A9E
//  Tile CONFIG    : x=4   y=175 w=232 h=60   laranja #D83B01
//  Tile RECOVER   : x=4   y=241 w=113 h=75   verde  #107C10
//  Tile CHAVE.KEY : x=123 y=241 w=113 h=75   roxo   #5C2D91
//  Gap entre tiles: 4px (fundo preto aparece como grade)
// ─────────────────────────────────────────────────────────────────────────────

// Cores Metro UI (RGB565)
#define METRO_BLUE    0x03DA   // #0078D7
#define METRO_DBLUE   0x02D3   // #005A9E
#define METRO_ORANGE  0xD9C0   // #D83B01
#define METRO_GREEN   0x13E2   // #107C10
#define METRO_PURPLE  0x5972   // #5C2D91
#define METRO_WHITE   0xFFFF
// Coordenadas dos tiles — usadas no handleTouch
#define MT_12_X    4
#define MT_12_Y   56
#define MT_24_X  123
#define MT_24_Y   56
#define MT_TW    113
#define MT_TH    113
#define MT_CFG_X   4
#define MT_CFG_Y 175
#define MT_CFG_W 232
#define MT_CFG_H  60
#define MT_REC_X   4
#define MT_REC_Y 241
#define MT_KEY_X 123
#define MT_KEY_Y 241
#define MT_BW    113
#define MT_BH     75

// Desenha um tile Metro: fill colorido + label(s) no canto inf-esq
static void drawMetroTile(int x, int y, int w, int h, uint16_t col,
                          const char* line1, const char* line2 = "") {
  tft.fillRect(x, y, w, h, col);
  tft.setTextColor(C_WHITE, col);

  bool isBigNum = (strlen(line1) <= 2 && isdigit(line1[0]));
  if (isBigNum) {
    // Número grande + label pequena abaixo
    tft.setTextSize(4);
    tft.setCursor(x + 12, y + h - 54);
    tft.print(line1);
    if (line2[0]) {
      tft.setTextSize(2);
      tft.setCursor(x + 12, y + h - 22);
      tft.print(line2);
    }
  } else if (line2[0]) {
    // Duas linhas normais
    tft.setTextSize(2);
    tft.setCursor(x + 12, y + h - 36);
    tft.print(line1);
    tft.setCursor(x + 12, y + h - 18);
    tft.print(line2);
  } else {
    // Uma linha centralizada verticalmente
    tft.setTextSize(2);
    tft.setCursor(x + 12, y + (h - 16) / 2);
    tft.print(line1);
  }
}

// ── drawHome ─────────────────────────────────────────────────────────────────
void drawHome() {
  currentScreen = SCR_HOME;

  // Fundo preto (aparece como grade entre os tiles)
  tft.fillScreen(C_WHITE);

  // Header
  tft.setTextColor(C_BG, C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 20);
  tft.print("   BTC HD WALLET");

  // Tiles linha 1
  drawMetroTile(MT_12_X,  MT_12_Y,  MT_TW, MT_TH, METRO_BLUE,   "12", "WORDS");
  drawMetroTile(MT_24_X,  MT_24_Y,  MT_TW, MT_TH, METRO_DBLUE,  "24", "WORDS");

  // Tile CONFIG (largura total)
  drawMetroTile(MT_CFG_X, MT_CFG_Y, MT_CFG_W, MT_CFG_H, METRO_ORANGE, "CONFIG", "");

  // Tiles linha 2
  drawMetroTile(MT_REC_X, MT_REC_Y, MT_BW, MT_BH, METRO_GREEN, "", "RECOVER");

  bool keyOk = !g_keyPassword.isEmpty();
  drawMetroTile(MT_KEY_X, MT_KEY_Y, MT_BW, MT_BH,
                keyOk ? METRO_GREEN : METRO_PURPLE,
                "CHAVE", keyOk ? ".KEY +" : ".KEY");
}

// ─────────────────────────────────────────────────────────────────────────────
// CHOOSE WORDS
// ─────────────────────────────────────────────────────────────────────────────
void drawChooseWords() {
  currentScreen = SCR_CHOOSE_WORDS;
  const char* title = g_isRecovery ? "RECOVER" : "GERAR PALAVRAS";
  drawHeader(title, g_isRecovery ? C_BLUE : C_ORANGE);
  drawBackArrow();

  drawCard(8, 52, SW - 16, 30, C_SURFACE, C_SURFACE2);
  tft.setTextColor(C_WHITE, C_SURFACE);
  tft.setTextSize(1);
  tft.setCursor(18, 63);
  tft.print("ESCOLHA 12 OU 24 PALAVRAS");

  drawBtn(14, 96,  SW - 28, 52, "12 PALAVRAS", C_WHITE, C_BG);
  drawBtn(14, 158, SW - 28, 52, "24 PALAVRAS", C_PURPLE, C_WHITE);

  tft.setTextColor(C_TEXTDIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(12, 228);
  tft.print("RECUPERE AS SUAS SEEDS");
}

// ─────────────────────────────────────────────────────────────────────────────
// doGenerate
// ─────────────────────────────────────────────────────────────────────────────
void doGenerate() {
  tft.fillScreen(C_BG);
  drawHeader("GERANDO...", C_ORANGE);

  tft.setTextColor(C_GRAY, C_BG);
  tft.setTextSize(1);
  tft.setCursor(8, 52);
  tft.print("COLETANDO ENTROPIA...");

  uint8_t entropy[32];
  int entropyBytes = (g_numWords == 12) ? 16 : 32;
  esp_fill_random(entropy, entropyBytes);

  const char* mn = generateMnemonic(entropy, entropyBytes);
  g_mnemonic = String(mn);

  if (g_numWords == 12) {
    int wordCount = 0, cutPos = -1;
    for (int i = 0; i < (int)g_mnemonic.length(); i++) {
      if (g_mnemonic[i] == ' ') {
        wordCount++;
        if (wordCount == 12) { cutPos = i; break; }
      }
    }
    if (cutPos > 0) g_mnemonic = g_mnemonic.substring(0, cutPos);
  }

  g_passphrase = "";
  g_seedTab  = 0;
  g_seedZoom = 1;
  Serial.println(">>> MNEMONIC GERADO (" + String(g_numWords) + " PALAVRAS):");
  Serial.println(g_mnemonic);

  drawShowSeed();
}

// ─────────────────────────────────────────────────────────────────────────────
// SHOW SEED — Portrait
// 12 palavras: sem abas, 2 colunas, textSize(2)
// 24 palavras: 2 abas (1-12 / 13-24), 2 colunas, textSize(2)
// ─────────────────────────────────────────────────────────────────────────────
void drawShowSeed() {
  currentScreen = SCR_SHOW_SEED;
  drawHeader(" ANOTE SUA SEED", C_WHITE);
  drawBackArrow(C_GOLD);

  // Parseia as palavras
  String words[24];
  int cnt = 0;
  String tmp = g_mnemonic + " ";
  while (tmp.length() > 0 && cnt < 24) {
    int sp = tmp.indexOf(' ');
    if (sp < 0) { words[cnt++] = tmp; break; }
    words[cnt++] = tmp.substring(0, sp);
    tmp = tmp.substring(sp + 1);
  }
  if (cnt > g_numWords) cnt = g_numWords;

  // ── Abas (só para 24 palavras) ───────────────────────────────────────────
  int cardY = 52;
  if (g_numWords == 24) {
    cardY = 70;  // card desce para dar espaço às abas
    int tabW = (SW - 8) / 2;
    const char* tabLbl[2] = {"PALAVRAS 1-12", "PALAVRAS 13-24"};
    uint16_t tabActive   = C_GOLD;
    uint16_t tabInactive = darken(darken(C_GOLD));
    for (int i = 0; i < 2; i++) {
      uint16_t bg = (g_seedTab == i) ? tabActive : tabInactive;
      tft.fillRoundRect(4 + i * (tabW + 4), 50, tabW, 18, 8, bg);
      uint16_t fg = (g_seedTab == i) ? C_BG : C_WHITE;
      tft.setTextColor(fg, bg); tft.setTextSize(1);
      int tw = strlen(tabLbl[i]) * 6;
      tft.setCursor(4 + i * (tabW + 4) + (tabW - tw) / 2, 56);
      tft.print(tabLbl[i]);
    }
  }

  // ── Card de palavras ─────────────────────────────────────────────────────
  // cardH reserva 32px para a barra de zoom entre o card e o botão CONTINUAR
  int cardH  = SH - cardY - 48 - 32;
  drawCard(6, cardY, SW - 12, cardH, C_SURFACE, C_GOLD);

  int startIdx = (g_numWords == 24 && g_seedTab == 1) ? 12 : 0;
  int startY   = cardY + 10;

  if (g_seedZoom == 1) {
    // ── Zoom 1: textSize(1) = 6×8px, 2 colunas, palavras completas (≤8ch) ──
    // Col esq: num x=10 (2ch=12px) | palavra x=26 (8ch=48px) → fim 74px
    // Col dir: num x=124           | palavra x=140            → fim 188px
    tft.setTextSize(1);
    int lineH = 22;
    for (int i = 0; i < 12; i++) {
      int wordIdx = startIdx + i;
      if (wordIdx >= cnt) break;
      int col   = (i < 6) ? 0 : 1;
      int linha = (i < 6) ? i : i - 6;
      int ky    = startY + linha * lineH;
      int kxNum = (col == 0) ?  10 : 124;
      int kxWrd = (col == 0) ?  26 : 140;
      char numBuf[4];
      snprintf(numBuf, sizeof(numBuf), "%2d", wordIdx + 1);
      tft.setTextColor(C_GOLD, C_SURFACE);
      tft.setCursor(kxNum, ky); tft.print(numBuf);
      tft.setTextColor(C_WHITE, C_SURFACE);
      String w = words[wordIdx];
      if (w.length() > 8) w = w.substring(0, 8);  // BIP39 max = 8, nunca trunca
      tft.setCursor(kxWrd, ky); tft.print(w);
    }
  } else {
    // ── Zoom 2: textSize(2) = 12×16px, 2 colunas, palavras até 7ch ──────────
    // Disponível: 228px | por coluna: 114px
    // num: 2ch×12=24px + gap 4px → palavra: 86px → 7ch (84px) ✓
    // Col esq: num x=6 | palavra x=34 → fim 118px
    // Col dir: num x=120 | palavra x=148 → fim 232px  margem dir=8px ✓
    tft.setTextSize(2);
    int lineH = 20;
    for (int i = 0; i < 12; i++) {
      int wordIdx = startIdx + i;
      if (wordIdx >= cnt) break;
      int col   = (i < 6) ? 0 : 1;
      int linha = (i < 6) ? i : i - 6;
      int ky    = startY + linha * lineH;
      int kxNum = (col == 0) ?   6 : 120;
      int kxWrd = (col == 0) ?  34 : 148;
      char numBuf[4];
      snprintf(numBuf, sizeof(numBuf), "%2d", wordIdx + 1);
      tft.setTextColor(C_GOLD, C_SURFACE);
      tft.setCursor(kxNum, ky); tft.print(numBuf);
      tft.setTextColor(C_WHITE, C_SURFACE);
      String w = words[wordIdx];
      if (w.length() > 7) w = w.substring(0, 7);  // 7×12px=84px por coluna
      tft.setCursor(kxWrd, ky); tft.print(w);
    }
  }

  // ── Barra de Zoom ─────────────────────────────────────────────────────────
  // Posicionada entre o card e o botão CONTINUAR
  // Exemplo 12w: card termina em y=52+188=240, zoomBarY=244, CONTINUAR em y=278
  int zoomBarY = cardY + cardH + 4;
  tft.fillRoundRect(6, zoomBarY, SW - 12, 26, 6, C_SURFACE);
  tft.drawRoundRect(6, zoomBarY, SW - 12, 26, 6, C_LINE);

  // Botão A- (desabilitado quando já no mínimo)
  uint16_t bgMinus = (g_seedZoom <= 1) ? C_DGRAY    : C_SURFACE2;
  uint16_t fgMinus = (g_seedZoom <= 1) ? C_GRAY     : C_WHITE;
  drawBtn(8, zoomBarY + 2, 54, 22, "A-", bgMinus, fgMinus);

  // Indicador de nível: 2 bolinhas, ativa = dourado grande
  int dotCY = zoomBarY + 13;
  for (int d = 0; d < 2; d++) {
    bool active = (d + 1 == g_seedZoom);
    int  dotX   = SW / 2 - 8 + d * 16;
    tft.fillCircle(dotX, dotCY, active ? 5 : 3, active ? C_GOLD : C_DGRAY);
  }

  // Botão A+ (desabilitado quando já no máximo)
  uint16_t bgPlus = (g_seedZoom >= 2) ? C_DGRAY    : C_SURFACE2;
  uint16_t fgPlus = (g_seedZoom >= 2) ? C_GRAY     : C_WHITE;
  drawBtn(SW - 64, zoomBarY + 2, 54, 22, "A+", bgPlus, fgPlus);

  drawBtn(12, SH - 42, SW - 24, 36, "CONTINUAR", C_GOLD, C_BG);
}

// ─────────────────────────────────────────────────────────────────────────────
// GENKEY
// ─────────────────────────────────────────────────────────────────────────────
void drawGenKey() {
  currentScreen = SCR_GENKEY;
  drawHeader("  GERAR A CHAVE", C_ORANGE);
  drawBackArrow();

  drawCard(6, 52, SW - 12, 72, C_SURFACE, g_keyPassword.isEmpty() ? C_RED : C_GREEN);
  if (g_keyPassword.isEmpty()) {
    tft.setTextColor(C_WHITE, C_SURFACE); tft.setTextSize(1);
    tft.setCursor(14, 66);  tft.print("DEFINA UMA SENHA ANTES DE");
    tft.setCursor(14, 80);  tft.print("GERAR O ARQUIVO CHAVE.KEY");
    tft.setCursor(14, 96);  tft.print("NO CARTAO SD.");
  } else {
    drawBadge(160, 60, 64, "PRONTA", C_GREEN, C_BG);
    tft.setTextColor(C_GREEN, C_SURFACE); tft.setTextSize(1);
    tft.setCursor(14, 68); tft.print("SENHA ATIVA EM MEMORIA");
    tft.setTextColor(C_WHITE, C_SURFACE); tft.setTextSize(2);
    tft.setCursor(14, 86);
    String mask = "";
    for (int i = 0; i < (int)g_keyPassword.length() && i < 14; i++) mask += "*";
    tft.print(mask);
  }

  drawBtn(12, 134, SW - 24, 40, "DIGITAR SENHA",       C_WHITE, C_BG);
  drawBtn(12, 182, SW - 24, 40, "GERAR E SALVAR .KEY", C_TEAL,  C_BG);

  tft.setTextColor(C_TEXTDIM, C_BG); tft.setTextSize(1);
  tft.setCursor(10, 236);
  tft.print("USE A MESMA SENHA NO PC.");
}

void doSaveKeyFile() {
  if (g_keyPassword.isEmpty()) {
    tft.fillScreen(C_BG);
    drawHeader("   ERRO", C_RED);
    tft.setTextColor(C_WHITE, C_BG); tft.setTextSize(1);
    tft.setCursor(8, 60); tft.print("CONFIGURE UMA SENHA PRIMEIRO!");
    tft.setCursor(8, 76); tft.print("SEM A SENHA AS SEEDS NAO SERA SALVA");
    tft.setCursor(8, 92); tft.print("NO SD A SENHA SERVE PARA CRYPTOGRAFAR");
    tft.setCursor(8, 108); tft.print("E PROTEGER AS SEEDS NO SD");
    tft.setCursor(8, 124); tft.print("CRYPTOGRAFANDO O ARQUIVO .enc");
    tft.setCursor(8, 140); tft.print("GERANDO O ARQUIVO chave.key");
    delay(5000); drawGenKey(); return;
  }

  tft.fillScreen(C_BG);
  drawHeader("   SALVANDO...", C_TEAL);
  tft.setTextColor(C_YELLOW, C_BG); tft.setTextSize(1);
  tft.setCursor(8, 60); tft.print("GERANDO SALT E VERIFICADOR...");
  tft.setCursor(8, 76); tft.print("CODIFICANDO EM BASE64...");
  tft.setCursor(8, 92); tft.print("GRAVANDO CHAVE.KEY NO SD...");

  bool saved = saveKeyFile(g_keyPassword);

  touchSPI.begin(25, 39, 32, 33);
  touch.begin(touchSPI);
  touch.setRotation(0);

  tft.setCursor(8, 116);
  if (saved) {
    tft.setTextColor(C_WHITE, C_BG);  tft.print("CHAVE.KEY SALVO!");
    tft.setCursor(8, 132); tft.setTextColor(C_WHITE, C_BG); tft.print("/CHAVE.KEY NO SD");
    tft.setCursor(8, 152); tft.setTextColor(C_WHITE,  C_BG); tft.print("USE NO PC PARA");
    tft.setCursor(8, 166); tft.print("DESCRIPTOGRAFAR XXXXXX.enc");
  } else {
    tft.setTextColor(C_WHITE, C_BG); tft.print("FALHA! VERIFIQUE O SD.");
  }
  delay(5000);
  drawGenKey();
}

// ─────────────────────────────────────────────────────────────────────────────
// RECOVER INTRO
// ─────────────────────────────────────────────────────────────────────────────
void drawRecoverIntro() {
  currentScreen = SCR_RECOVER_INTRO;
  drawHeader("   RESTAURAR", C_BLUE);
  drawBackArrow();

  drawCard(8, 52, SW - 16, 68, C_SURFACE, C_BLUE);
//  drawBadge(148, 60, 80, g_numWords == 24 ? "24 WORDS" : "12 WORDS", C_BLUE, C_WHITE);
  tft.setTextColor(C_WHITE, C_SURFACE); tft.setTextSize(1);
  tft.setCursor(16, 74);
  tft.printf("DIGITE AS %d PALAVRAS DA SEED.", g_numWords);
  tft.setCursor(16, 90);
  tft.print("USE 4 PRIMEIRAS LETRAS.");

  g_recoverWordIdx = 0;
  for (int i = 0; i < 24; i++) g_recoverWords[i] = "";
  g_currentInput = "";

  drawBtn(30, 136, SW - 60, 52, "INICIAR", C_WHITE, C_BG);

  tft.setTextColor(C_TEXTDIM, C_BG); tft.setTextSize(1);
  tft.setCursor(8, 230);
  tft.print("VOLTE PARA CANCELAR.");
}

static String normalizeBip39Word(String word) {
  word.trim();
  word.toLowerCase();
  return word;
}

static int bip39Index(const String& word) {
  String lower = normalizeBip39Word(word);
  for (uint16_t i = 0; i < BIP39_WORD_COUNT; i++) {
    if (strcmp(BIP39_WORDS[i], lower.c_str()) == 0) return (int)i;
  }
  return -1;
}

static void showKeyboardMessage(const char* title, uint16_t color,
                                const String& line1, const String& line2) {
  tft.fillScreen(C_BG);
  drawHeader(title, color);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setTextSize(1);
  tft.setCursor(8, 70);
  tft.print(line1);
  if (!line2.isEmpty()) {
    tft.setCursor(8, 86);
    tft.print(line2);
  }
  delay(1800);
}

static bool validateRecoveryMnemonic(String& errorMsg) {
  if (g_numWords != 12 && g_numWords != 24) {
    errorMsg = "QUANTIDADES DE PALAVRAS INVALIDAS.";
    return false;
  }

  const int entropyBits = (g_numWords / 3) * 32;
  const int checksumBits = g_numWords / 3;
  const int totalBits = g_numWords * 11;
  const int totalBytes = (totalBits + 7) / 8;
  const int entropyBytes = entropyBits / 8;
  uint8_t packed[33] = {0};
  int bitPos = 0;

  for (int i = 0; i < g_numWords; i++) {
    g_recoverWords[i] = normalizeBip39Word(g_recoverWords[i]);
    int idx = bip39Index(g_recoverWords[i]);
    if (idx < 0) {
      errorMsg = "PALAVRA INVALIDA NA SEED.";
      return false;
    }

    for (int bit = 10; bit >= 0; bit--) {
      if (idx & (1 << bit)) {
        packed[bitPos / 8] |= (uint8_t)(1 << (7 - (bitPos % 8)));
      }
      bitPos++;
    }
  }

  uint8_t hash[32] = {0};
  mbedtls_sha256(packed, entropyBytes, hash, 0);

  for (int i = 0; i < checksumBits; i++) {
    int combinedBitPos = entropyBits + i;
    uint8_t mnemonicBit = (packed[combinedBitPos / 8] >> (7 - (combinedBitPos % 8))) & 0x01;
    uint8_t hashBit = (hash[0] >> (7 - i)) & 0x01;
    if (mnemonicBit != hashBit) {
      errorMsg = "CHECKSUM BIP39 INVALIDO.";
      return false;
    }
  }

  for (int i = totalBytes; i < 33; i++) packed[i] = 0;
  return true;
}

static bool commitRecoveryWord(const String& typedWord) {
  String normalized = normalizeBip39Word(typedWord);
  if (!bip39Exact(normalized)) {
    showKeyboardMessage("PALAVRA INVALIDA", C_RED,
                        "Use apenas palavras BIP39.",
                        "Confira a digitacao da palavra.");
    drawKeyboard();
    return false;
  }

  g_recoverWords[g_recoverWordIdx] = normalized;
  g_currentInput = "";
  g_suggestCount = 0;
  g_suggestVisible = false;
  g_recoverWordIdx++;

  if (g_recoverWordIdx >= g_numWords) {
    String validationError;
    if (!validateRecoveryMnemonic(validationError)) {
      showKeyboardMessage("SEED INVALIDA", C_RED,
                          validationError,
                          "Digite as palavras novamente.");
      drawRecoverIntro();
      return false;
    }

    g_mnemonic = "";
    for (int i = 0; i < g_numWords; i++) {
      if (i > 0) g_mnemonic += " ";
      g_mnemonic += g_recoverWords[i];
    }
    g_seedTab = 0;
    drawPassphraseScreen();
    return true;
  }

  drawKeyboard();
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// KEYBOARD — Portrait 240×320  (versão melhorada)
//
// Layout:
//   Header    y=0..44    (drawHeader padrão)
//   Painel    y=46..103  (card alto com contexto + texto + contador)
//   Sugestões y=106..136 (card dedicado, visível só em KB_RECOVER_WORD)
//   Teclas    y=140..272 (3 linhas × 38px + 3px gap)
//   Barra inf y=276..316 (ABC/123 | ESPAÇO | ← DEL | OK/ENTER)
// ─────────────────────────────────────────────────────────────────────────────
#define KB_PANEL_Y      46    // topo do painel de input
#define KB_PANEL_H      58    // altura do painel
#define SUGG_Y         108    // topo da área de sugestões
#define SUGG_H          26    // altura de cada botão sugestão
#define SUGG_GAP         3
#define SUGG_AREA_H     30    // altura total da faixa de sugestões
#define KB_KEYS_Y      138
#define KB_KEY_H        44
#define KB_KEY_W        23
#define KB_KEY_GAP       1
#define KB_ROW_GAP       2
#define KB_BAR_Y       276
#define KB_BAR_H        42
// Linha 2: [CAPS ⬆] [z x c v b n m] [DEL]
#define KB_R2_CAPS_X     2
#define KB_R2_CAPS_W    32
#define KB_R2_LTR_X     36
#define KB_R2_DEL_X    203
#define KB_R2_DEL_W     35
// Barra inferior: [NUMS] [ESPACO] [OK]
#define KB_BAR_NUMS_X    2
#define KB_BAR_NUMS_W   52
#define KB_BAR_SPC_X    56
#define KB_BAR_SPC_W   130
#define KB_BAR_OK_X    188
#define KB_BAR_OK_W     50

// ── Painel de input ──────────────────────────────────────────────────────────
void kbRedrawInput() {
  String preview = g_currentInput;
  preview.trim();

  // Cor de destaque depende do contexto
  uint16_t accentCol = C_PURPLE;
  if      (g_kbTarget == KB_GENKEY)      accentCol = C_TEAL;
  else if (g_kbTarget == KB_RECOVER_WORD) accentCol = C_WHITE;

  // Card principal do painel
  tft.fillRoundRect(4, KB_PANEL_Y, SW - 8, KB_PANEL_H, 10, C_SURFACE);
  tft.drawRoundRect(4, KB_PANEL_Y, SW - 8, KB_PANEL_H, 10, accentCol);
  // Linha de destaque no topo do card
  tft.fillRoundRect(4, KB_PANEL_Y, SW - 8, 4, 4, accentCol);

  // ── Rótulo de contexto (canto sup-esq, dentro do card) ───────────────────
  const char* ctxLbl = "";
  if      (g_kbTarget == KB_PASSPHRASE)   ctxLbl = "PASSPHRASE";
  else if (g_kbTarget == KB_GENKEY)        ctxLbl = "SENHA";
  else {
    // Para recover: "PALAVRA X / Y"
    static char ctxBuf[20];
    snprintf(ctxBuf, sizeof(ctxBuf), "PAL. %d/%d", g_recoverWordIdx + 1, g_numWords);
    ctxLbl = ctxBuf;
  }
  tft.setTextColor(accentCol, C_SURFACE);
  tft.setTextSize(1);
  tft.setCursor(14, KB_PANEL_Y + 8);
  tft.print(ctxLbl);

  // ── Contador de caracteres (canto sup-dir) ────────────────────────────────
  if (preview.length() > 0) {
    char cntBuf[8];
    snprintf(cntBuf, sizeof(cntBuf), "%d", (int)preview.length());
    int cntW = strlen(cntBuf) * 6;
    tft.setTextColor(C_TEXTDIM, C_SURFACE);
    tft.setTextSize(1);
    tft.setCursor(SW - 14 - cntW, KB_PANEL_Y + 8);
    tft.print(cntBuf);
  }

  // ── Texto digitado (linha principal) ─────────────────────────────────────
  tft.setTextColor(C_WHITE, C_SURFACE);
  if (preview.isEmpty()) {
    // Placeholder em tom apagado
    tft.setTextColor(C_TEXTDIM, C_SURFACE);
    tft.setTextSize(1);
    tft.setCursor(14, KB_PANEL_Y + 36);
    if      (g_kbTarget == KB_PASSPHRASE) tft.print("Digite a passphrase...");
    else if (g_kbTarget == KB_GENKEY)     tft.print("Digite a senha...");
    else                                   tft.print("Digite a palavra BIP39...");
  } else {
    tft.setTextSize(2);
    // Trunca pelo início se muito longo (max ~16 chars em textSize 2)
    String display = preview;
    if (display.length() > 16) display = String((char)0xBB) + display.substring(display.length() - 14);
    // Cursor piscante  "▌"  simulado com underscore
    display += "_";
    int tw = display.length() * 12;
    // Centraliza horizontalmente
    int tx2 = max(14, (SW - tw) / 2);
    tft.setCursor(tx2, KB_PANEL_Y + 30);
    tft.print(display);
  }
}

// ── Área de sugestões BIP39 ──────────────────────────────────────────────────
void kbClearSuggestions() {
  g_suggestCount   = 0;
  g_suggestVisible = false;
  tft.fillRect(0, SUGG_Y - 2, SW, SUGG_AREA_H + 4, C_BG);
}

void kbDrawSuggestions() {
  tft.fillRect(0, SUGG_Y - 2, SW, SUGG_AREA_H + 4, C_BG);
  if (g_suggestCount == 0) return;
  g_suggestVisible = true;
  int n = g_suggestCount;
  int totalGap = SUGG_GAP * (n + 1);
  int btnW = (SW - totalGap) / n;
  for (int i = 0; i < n; i++) {
    int bx = SUGG_GAP + i * (btnW + SUGG_GAP);
    uint16_t bg = (n == 1) ? C_GREEN : C_TEAL;
    drawSuggBtn(bx, SUGG_Y, btnW, SUGG_H, g_suggestions[i], bg);
  }
}

void kbUpdateSuggestions() {
  if (g_kbTarget != KB_RECOVER_WORD) { kbClearSuggestions(); return; }
  String lower = g_currentInput;
  lower.trim(); lower.toLowerCase();
  if (lower.length() < 4) { kbClearSuggestions(); return; }
  g_suggestCount = bip39Search(lower, g_suggestions, BIP39_SUGGEST_MAX);
  if (g_suggestCount == 0) { kbClearSuggestions(); return; }
  if (g_suggestCount == 1) {
    g_currentInput = String(g_suggestions[0]);
    g_suggestCount = 0; g_suggestVisible = false;
    tft.fillRect(0, SUGG_Y - 2, SW, SUGG_AREA_H + 4, C_BG);
    kbRedrawInput(); return;
  }
  kbDrawSuggestions();
}

static const char* KB_ROWS_LOWER[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm", ""};
static const char* KB_ROWS_UPPER[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", ""};
static const char* KB_ROWS_NUM[]   = {"1234567890", "-/:;()&@#", ".,?!'%^+=", ""};

// ── WP flat key ────────────────────────────────────────────────────────────
static void drawKey(int kx, int ky, int kw, int kh, const char* lbl,
                    uint16_t bg, uint16_t fg) {
  tft.fillRect(kx, ky, kw, kh, bg);
  uint8_t sz = (strlen(lbl) == 1 || kw >= 48) ? 2 : 1;
  tft.setTextColor(fg, bg);
  tft.setTextSize(sz);
  int tw = strlen(lbl) * 6 * sz, th = 8 * sz;
  tft.setCursor(kx + (kw - tw) / 2, ky + (kh - th) / 2);
  tft.print(lbl);
}

// ── Sinking press (WP feel) ─────────────────────────────────────────────────
static void pressKey(int kx, int ky, int kw, int kh, const char* lbl,
                     uint16_t bg, uint16_t fg) {
  tft.fillRect(kx, ky, kw, kh, C_BG);
  uint16_t pb = darken(darken(bg));
  tft.fillRect(kx, ky + 2, kw, kh - 2, pb);
  uint8_t sz = (strlen(lbl) == 1 || kw >= 48) ? 2 : 1;
  tft.setTextColor(fg, pb);
  tft.setTextSize(sz);
  int tw = strlen(lbl) * 6 * sz, th = 8 * sz;
  tft.setCursor(kx + (kw - tw) / 2, ky + 2 + (kh - 2 - th) / 2);
  tft.print(lbl);
  delay(50);
  drawKey(kx, ky, kw, kh, lbl, bg, fg);
}

// ── Cor das teclas de letra (fonte única de verdade) ───────────────────────
static uint16_t kbKeyColor() {
  if (g_kbLayout == KB_NUMBERS)      return C_SURFACE2;
  if (g_kbTarget == KB_GENKEY)       return C_TEAL;
  if (g_kbTarget == KB_RECOVER_WORD) return C_SURFACE2;
  return C_PURPLE;
}

// ── Botão CAPS com seta ⬆ desenhada ───────────────────────────────────────
// active=true → maiúsculas ativas (seta branca cheia)
// active=false → minúsculas (seta cinza/outline)
static void drawCapsKey(int kx, int ky, int kw, int kh, bool active) {
  uint16_t bg = C_SURFACE2;
  tft.fillRect(kx, ky, kw, kh, bg);
  // Triângulo central (seta para cima)
  int cx  = kx + kw / 2;
  int cy  = ky + kh / 2 - 2;
  uint16_t arrowCol = active ? C_WHITE : C_GRAY;
  // Seta: triângulo + haste
  tft.fillTriangle(cx, cy - 7, cx - 7, cy + 2, cx + 7, cy + 2, arrowCol);
  tft.fillRect(cx - 3, cy + 2, 6, 5, arrowCol);
  // Borda dourada quando ativo (indica maiúsculas ligadas)
  if (active) tft.drawRect(kx, ky, kw, kh, C_YELLOW);
}

static void pressCapsKey(int kx, int ky, int kw, int kh, bool active) {
  tft.fillRect(kx, ky, kw, kh, C_BG);
  uint16_t pb = darken(darken(C_SURFACE2));
  tft.fillRect(kx, ky + 2, kw, kh - 2, pb);
  int cx = kx + kw / 2, cy = ky + 2 + (kh - 2) / 2 - 2;
  uint16_t arrowCol = active ? C_WHITE : C_GRAY;
  tft.fillTriangle(cx, cy - 7, cx - 7, cy + 2, cx + 7, cy + 2, arrowCol);
  tft.fillRect(cx - 3, cy + 2, 6, 5, arrowCol);
  delay(50);
  drawCapsKey(kx, ky, kw, kh, active);
}

void drawKeyboard() {
  currentScreen = SCR_KEYBOARD;

  if      (g_kbTarget == KB_PASSPHRASE) drawHeader("PASSPHRASE", C_PURPLE);
  else if (g_kbTarget == KB_GENKEY)     drawHeader("SENHA",      C_TEAL);
  else {
    char buf[32];
    snprintf(buf, sizeof(buf), "PALAVRA %d/%d", g_recoverWordIdx + 1, g_numWords);
    drawHeader(buf, C_WHITE);
  }
  drawBackArrow();
  kbRedrawInput();

  if (g_suggestVisible && g_suggestCount > 0 && g_kbTarget == KB_RECOVER_WORD)
    kbDrawSuggestions();
  else
    tft.fillRect(0, SUGG_Y - 2, SW, SUGG_AREA_H + 4, C_BG);

  tft.fillRect(0, KB_KEYS_Y - 2, SW, SH - KB_KEYS_Y + 2, C_BG);

  uint16_t keyBg = kbKeyColor();
  const char** rows = (g_kbLayout == KB_LOWER) ? KB_ROWS_LOWER :
                      (g_kbLayout == KB_UPPER) ? KB_ROWS_UPPER : KB_ROWS_NUM;

  // Linhas 0 e 1 — centradas
  for (int row = 0; row < 2; row++) {
    const char* r  = rows[row];
    int len    = strlen(r);
    int totalW = len * KB_KEY_W + (len - 1) * KB_KEY_GAP;
    int xOff   = (SW - totalW) / 2;
    int ky     = KB_KEYS_Y + row * (KB_KEY_H + KB_ROW_GAP);
    for (int c = 0; c < len; c++)  {
      char lbl[2] = {r[c], 0};
      drawKey(xOff + c * (KB_KEY_W + KB_KEY_GAP), ky, KB_KEY_W, KB_KEY_H, lbl, keyBg, C_WHITE);
    }
  }

  // Linha 2: [⬆ CAPS] [z x c v b n m] [DEL]
  // CAPS inativo (cinza) em modo números; ativo (branco+borda) em maiúsculas
  int ky2 = KB_KEYS_Y + 2 * (KB_KEY_H + KB_ROW_GAP);
  drawCapsKey(KB_R2_CAPS_X, ky2, KB_R2_CAPS_W, KB_KEY_H, g_kbLayout == KB_UPPER);

  const char* r2 = rows[2];
  int r2len = strlen(r2);
  for (int c = 0; c < r2len; c++) {
    char lbl[2] = {r2[c], 0};
    drawKey(KB_R2_LTR_X + c * (KB_KEY_W + KB_KEY_GAP), ky2, KB_KEY_W, KB_KEY_H, lbl, keyBg, C_WHITE);
  }
  drawKey(KB_R2_DEL_X, ky2, KB_R2_DEL_W, KB_KEY_H, "DEL", C_RED, C_WHITE);

  // Barra inferior: [123 / ABC] [ESPACO] [OK]
  const char* numsLbl = (g_kbLayout == KB_NUMBERS) ? "ABC" : "123";
  uint16_t okCol = (g_kbTarget == KB_PASSPHRASE || g_kbTarget == KB_GENKEY) ? C_GREEN
                   : (g_currentInput.isEmpty() ? C_DGRAY : C_GREEN);
  drawKey(KB_BAR_NUMS_X, KB_BAR_Y, KB_BAR_NUMS_W, KB_BAR_H, numsLbl,  C_SURFACE2, C_YELLOW);
  drawKey(KB_BAR_SPC_X,  KB_BAR_Y, KB_BAR_SPC_W,  KB_BAR_H, "ESPACO", C_PANEL,    C_WHITE);
  drawKey(KB_BAR_OK_X,   KB_BAR_Y, KB_BAR_OK_W,   KB_BAR_H, "OK",     okCol,      C_WHITE);
}

void kbHandleKey(const char* key) {
  bool layoutChanged = false;

  if (strcmp(key, "DEL") == 0) {
    if (!g_currentInput.isEmpty())
      g_currentInput = g_currentInput.substring(0, g_currentInput.length() - 1);
  }
  else if (strcmp(key, "ESPACO") == 0) {
    if (g_kbTarget == KB_PASSPHRASE || g_kbTarget == KB_GENKEY)
      g_currentInput += " ";
  }
  else if (strcmp(key, "CAPS") == 0) {
    // Alterna EXCLUSIVAMENTE entre minúsculo <-> maiúsculo
    g_kbLayout = (g_kbLayout == KB_UPPER) ? KB_LOWER : KB_UPPER;
    layoutChanged = true;
  }
  else if (strcmp(key, "NUMS") == 0) {
    // Entra em modo números (ou volta para minúsculo se já estiver em números)
    g_kbLayout = (g_kbLayout == KB_NUMBERS) ? KB_LOWER : KB_NUMBERS;
    layoutChanged = true;
  }
  else if (strcmp(key, "ABC") == 0 || strcmp(key, "123") == 0) {
    // Legado: barra inferior — "123" entra em números, "ABC" volta para lower
    g_kbLayout = (strcmp(key, "123") == 0) ? KB_NUMBERS : KB_LOWER;
    layoutChanged = true;
  }
  else if (strcmp(key, "OK") == 0 || strcmp(key, "ENTER") == 0) {
    if (g_kbTarget == KB_PASSPHRASE) {
      g_passphrase = g_currentInput; g_currentInput = "";
      kbClearSuggestions(); drawAccountScreen(); return;
    }
    else if (g_kbTarget == KB_GENKEY) {
      g_keyPassword = g_currentInput; g_currentInput = "";
      kbClearSuggestions(); drawGenKey(); return;
    }
    else {
      if (!g_currentInput.isEmpty()) {
        commitRecoveryWord(g_currentInput);
        return;
      }
    }
  }
  else {
    g_currentInput += String(key);
  }

  if (layoutChanged) {
    drawKeyboard();
  } else {
    kbRedrawInput();
    kbUpdateSuggestions();
    uint16_t okCol = (g_kbTarget == KB_PASSPHRASE || g_kbTarget == KB_GENKEY) ? C_GREEN
                     : (g_currentInput.isEmpty() ? C_DGRAY : C_GREEN);
    drawKey(KB_BAR_OK_X, KB_BAR_Y, KB_BAR_OK_W, KB_BAR_H, "OK", okCol, C_WHITE);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// PASSPHRASE
// ─────────────────────────────────────────────────────────────────────────────
void drawPassphraseScreen() {
  currentScreen = SCR_PASSPHRASE;
  drawHeader("PASSPHRASE BIP39", C_PURPLE);
  drawBackArrow();

  drawCard(8, 52, SW - 16, 68, C_SURFACE, C_PURPLE);
  //drawBadge(148, 60, 78, "OPCIONAL", C_SURFACE2, C_CREAM);
  tft.setTextColor(C_WHITE, C_SURFACE); tft.setTextSize(1);
  tft.setCursor(16, 74); tft.print("DESEJA ADICIONAR PASSPHRASE?");
  tft.setCursor(16, 90); tft.print("GERA UM CONJUNTO DIFERENTE");
  tft.setCursor(16, 104); tft.print("DE ENDERECOS BITICOIN.");

  // Botões SIM / NAO lado a lado
  drawBtn(8,   134, 106, 52, "SIM", C_PURPLE, C_WHITE);
  drawBtn(126, 134, 106, 52, "NAO", C_GREEN,  C_BG);

  tft.setTextColor(C_TEXTDIM, C_BG); tft.setTextSize(1);
  tft.setCursor(8, 230);
  tft.print("USAR PASSPHRASE E OPCIONAL.");
}

// ─────────────────────────────────────────────────────────────────────────────
// SCR_ACCOUNT — Escolha do account de derivação (0..99)
//
// Layout Portrait 240×320:
//   Header "ACCOUNT / DERIVACAO"
//   Card informativo (o que é account)
//   Botões numéricos: 0  1  2  3  4  (linha 1)
//                     5  6  7  8  9  (linha 2)
//   Badge com account selecionado atualmente
//   Botão CONTINUAR
//
// O account selecionado é salvo em g_account e usado em doDerive().
// ─────────────────────────────────────────────────────────────────────────────
// Layout da tela de derivação — numpad estilo calculadora
//
//  Header        y=0..44
//  Card info     y=52..120
//  Display       y=126..168   (mostra dígitos acumulados, índice atual)
//  Numpad 0..9   y=174..272   (2 linhas × 5 botões)
//    Linha 1: 0 1 2 3 4       y=174
//    Linha 2: 5 6 7 8 9       y=218
//  Barra inf     y=278..316   [⌫ APAGAR]  [✓ CONTINUAR]
//
// Lógica:
//  - g_accDigits acumula string de dígitos (max 3 → 0..999)
//  - ao tocar um dígito: se g_accDigits=="", começa novo número;
//    se resultado novo seria > 999, ignora
//  - toque no ⌫ remove último dígito
//  - g_account = atoi(g_accDigits) ao confirmar
// ─────────────────────────────────────────────────────────────────────────────
#define ACC_BTN_W   40
#define ACC_BTN_H   38
#define ACC_BTN_GAP  4
#define ACC_ROW1_Y  178
#define ACC_ROW2_Y  220
#define ACC_DISP_Y  126
#define ACC_DISP_H   46
#define ACC_BAR_Y   270

static String g_accDigits = "0";   // string de dígitos; nunca vazia

// ── Redesenha só o display de dígitos (sem redesenhar a tela toda) ───────────
static void accRedrawDisplay() {
  // Fundo do display
  tft.fillRoundRect(8, ACC_DISP_Y, SW - 16, ACC_DISP_H, 8, C_SURFACE);
  tft.drawRoundRect(8, ACC_DISP_Y, SW - 16, ACC_DISP_H, 8, C_TEAL);
  // Barra de cor no topo do card
  tft.fillRoundRect(8, ACC_DISP_Y, SW - 16, 4, 4, C_TEAL);

  // Rótulo "INDICE:" à esquerda
  tft.setTextColor(C_TEAL, C_SURFACE);
  tft.setTextSize(1);
  tft.setCursor(18, ACC_DISP_Y + 10);
  tft.print("INDICE:");

  // Número grande à direita
  tft.setTextColor(C_WHITE, C_SURFACE);
  tft.setTextSize(3);
  int numW = g_accDigits.length() * 18;
  tft.setCursor(SW - 20 - numW, ACC_DISP_Y + 12);
  tft.print(g_accDigits);

  // Path de derivação abaixo
  tft.setTextSize(1);
  tft.setTextColor(C_TEXTDIM, C_SURFACE);
  char pathBuf[32];
  snprintf(pathBuf, sizeof(pathBuf), "m/44'/0'/0'/0/%s", g_accDigits.c_str());
  int pw = strlen(pathBuf) * 6;
  tft.setCursor((SW - pw) / 2, ACC_DISP_Y + 32);
  tft.print(pathBuf);
}

void drawAccountScreen() {
  currentScreen = SCR_ACCOUNT;
  // Reseta para "0" ao entrar na tela (começa limpo)
  g_accDigits = String(g_account);  // mantém o último índice escolhido
  if (g_accDigits.isEmpty()) g_accDigits = "0";

  drawHeader(" DERIVATION INDEX", C_TEAL);
  drawBackArrow();

  // ── Card informativo ───────────────────────────────────────────────────────
  drawCard(8, 52, SW - 16, 68, C_SURFACE, C_LINE);
  tft.setTextColor(C_WHITE, C_SURFACE); tft.setTextSize(1);
  tft.setCursor(14, 64);  tft.print("TOQUE UM DIGITO: seleciona 0-9");
  tft.setCursor(14, 78);  tft.print("TOQUE VARIOS: forma numero maior");
  tft.setCursor(14, 92);  tft.print("Ex: 1+0+2 = indice 102-maximo 999");
  tft.setCursor(14, 106); tft.print("Use DEL para apagar o ultimo digito");

  // ── Display do índice atual ────────────────────────────────────────────────
  accRedrawDisplay();

  // ── Numpad: linha 1 (0..4) e linha 2 (5..9) ──────────────────────────────
  int totalW = 5 * ACC_BTN_W + 4 * ACC_BTN_GAP;
  int xOff   = (SW - totalW) / 2;
  for (int i = 0; i < 10; i++) {
    char lbl[3];
    snprintf(lbl, sizeof(lbl), "%d", i);
    int col  = i % 5;
    int row  = i / 5;
    int bx   = xOff + col * (ACC_BTN_W + ACC_BTN_GAP);
    int by   = (row == 0) ? ACC_ROW1_Y : ACC_ROW2_Y;
    // Destaque se o dígito faz parte do número atual
    bool active = (g_accDigits.indexOf(lbl[0]) >= 0 && g_accDigits.length() > 0);
    // Destaque mais forte: último dígito digitado
    bool lastDigit = (g_accDigits.length() > 0 &&
                      g_accDigits[g_accDigits.length()-1] == lbl[0]);
    uint16_t bg = lastDigit ? C_TEAL :
                  active    ? C_SURFACE2 : C_PANEL;
    uint16_t fg = lastDigit ? C_BG : C_WHITE;
    drawBtn(bx, by, ACC_BTN_W, ACC_BTN_H, lbl, bg, fg);
  }

  // ── Barra inferior: [⌫ DEL] [✓ CONTINUAR] ────────────────────────────────
  drawBtn(8,           ACC_BAR_Y, 68, 40, "DEL",     C_SURFACE2, C_RED);
  drawBtn(82,          ACC_BAR_Y, SW - 90, 40, "CONTINUAR", C_GOLD,     C_BG);
}

// ─────────────────────────────────────────────────────────────────────────────
// doDerive
// Deriva endereços e WIFs usando o account escolhido pelo usuário (g_account).
// Paths: m/44'/0'/0'/X  m/49'/0'/0'/X  m/84'/0'/0'/X  (X = g_account)
// ─────────────────────────────────────────────────────────────────────────────
void doDerive() {
  tft.fillScreen(C_BG);
  drawHeader("DERIVANDO...", C_ORANGE);
  tft.setTextColor(C_GRAY, C_BG); tft.setTextSize(1);

  // Paths CORRETOS (padrão usado por Electrum, Sparrow, etc.)
  char path44[32], path49[32], path84[32];
  snprintf(path44, sizeof(path44), "m/44'/0'/0'/0/%d", g_account);
  snprintf(path49, sizeof(path49), "m/49'/0'/0'/0/%d", g_account);
  snprintf(path84, sizeof(path84), "m/84'/0'/0'/0/%d", g_account);

  tft.setCursor(8, 52); tft.print("GERANDO SEED BIP39...");
  HDPrivateKey root;
  root.fromMnemonic(g_mnemonic.c_str(), g_passphrase.c_str());

  // ── BIP44 Legacy ──────────────────────────────────────────────────────────
tft.setCursor(8, 66);
tft.printf("BIP44 %s", path44);
{
    HDPrivateKey child = root.derive(path44);
    PublicKey pub = child.publicKey();
    char buf[64] = {0};
    pub.legacyAddress(buf, sizeof(buf), &Mainnet);
    g_addrP2PKH = String(buf);

    // === WIF CORRETO para esta versão da uBitcoin ===
    uint8_t secret[32] = {0};
    child.getSecret(secret);

    PrivateKey pk(secret, true, &Mainnet);   // compressed = true

    char wif[64] = {0};
    pk.wif(wif, sizeof(wif));                // método correto é .wif()

    g_wifP2PKH = String(wif);
    memset(secret, 0, sizeof(secret));       // limpar memória
}

// ── BIP49 P2SH ────────────────────────────────────────────────────────────
tft.setCursor(8, 80);
tft.printf("BIP49 %s", path49);
{
    HDPrivateKey child = root.derive(path49);
    PublicKey pub = child.publicKey();
    char buf[64] = {0};
    pub.nestedSegwitAddress(buf, sizeof(buf), &Mainnet);
    g_addrP2SH = String(buf);

    uint8_t secret[32] = {0};
    child.getSecret(secret);

    PrivateKey pk(secret, true, &Mainnet);
    char wif[64] = {0};
    pk.wif(wif, sizeof(wif));

    g_wifP2SH = String(wif);
    memset(secret, 0, sizeof(secret));
}

// ── BIP84 Native SegWit ───────────────────────────────────────────────────
tft.setCursor(8, 94);
tft.printf("BIP84 %s", path84);
{
    HDPrivateKey child = root.derive(path84);
    PublicKey pub = child.publicKey();
    char buf[96] = {0};
    pub.segwitAddress(buf, sizeof(buf), &Mainnet);
    g_addrBech32 = String(buf);

    uint8_t secret[32] = {0};
    child.getSecret(secret);

    PrivateKey pk(secret, true, &Mainnet);
    char wif[64] = {0};
    pk.wif(wif, sizeof(wif));

    g_wifBech32 = String(wif);
    memset(secret, 0, sizeof(secret));
}

  Serial.printf("=== DERIVACAO (account=%d) ===\n", g_account);
  Serial.println("BIP44: " + g_addrP2PKH + "  WIF: " + g_wifP2PKH);
  Serial.println("BIP49: " + g_addrP2SH  + "  WIF: " + g_wifP2SH);
  Serial.println("BIP84: " + g_addrBech32 + "  WIF: " + g_wifBech32);

  g_addrScroll = 0;
  tft.setCursor(8, 112); tft.setTextColor(C_WHITE, C_BG);
  tft.print("SALVANDO NO SD...");

  if (g_keyPassword.isEmpty()) {
    tft.setCursor(8, 128); tft.setTextColor(C_YELLOW, C_BG);
    tft.print("ATENCAO: CONFIGURE CHAVE.KEY!");
    delay(2000);
  }

  // Salva wallet.enc (mnemonic + passphrase + endereços cifrados)
  bool saved = saveWalletToSD(g_mnemonic, g_passphrase,
                              g_addrP2PKH, g_addrP2SH, g_addrBech32,
                              g_keyPassword, g_account);

  // Salva arquivo de WIFs no SD (cifrado com AES-256-GCM também)
  // Formato: wallet_wifs_<millis>.enc  — mesmo esquema de criptografia
  if (saved && !g_keyPassword.isEmpty()) {
    String wifContent  = "=== WIFs - ACCOUNT " + String(g_account) + " ===\n";
    wifContent += "Path BIP44: " + String(path44) + "\n";
    wifContent += "WIF  BIP44: " + g_wifP2PKH + "\n";
    wifContent += "Addr BIP44: " + g_addrP2PKH + "\n\n";
    wifContent += "Path BIP49: " + String(path49) + "\n";
    wifContent += "WIF  BIP49: " + g_wifP2SH + "\n";
    wifContent += "Addr BIP49: " + g_addrP2SH + "\n\n";
    wifContent += "Path BIP84: " + String(path84) + "\n";
    wifContent += "WIF  BIP84: " + g_wifBech32 + "\n";
    wifContent += "Addr BIP84: " + g_addrBech32 + "\n";
    wifContent += "=== FIM ===";
    // Reutiliza saveWalletToSD para cifrar — passa o conteúdo WIF como mnemonic
    // para aproveitar a criptografia, usando filename diferente implicitamente via millis()
    // (saveWalletToSD já gera nome único com millis)
    saveWalletToSD(wifContent, "", g_addrP2PKH, g_addrP2SH, g_addrBech32, g_keyPassword, g_account);
  }

  tft.setCursor(8, 144);
  tft.setTextColor(saved ? C_WHITE : C_WHITE, C_BG);
  tft.print(saved ? "SALVO COM SUCESSO!" : "Falha no SD!");
  delay(2000);

  touchSPI.begin(25, 39, 32, 33);
  touch.begin(touchSPI);
  touch.setRotation(0);

  drawAddresses();
}

// ─────────────────────────────────────────────────────────────────────────────
// ADDRESSES — Portrait
// Header y=0..44  | Tabs   y=48..68
// Card addr y=72..168 | Botões NOVA/PASS/SEED y=174..212
// Botão QR  y=218..256
// ─────────────────────────────────────────────────────────────────────────────
#define ADDR_TAB_Y    48   // topo das abas BIP44/49/84
#define ADDR_TAB_H    20   // altura das abas
#define ADDR_CARD_Y   72   // topo do card de endereço
#define ADDR_CARD_H   96   // altura do card de endereço
#define ADDR_BTN_Y   174   // topo dos botões NOVA/PASS/SEED
#define ADDR_BTN_H    36   // altura dos botões
#define ADDR_QR_Y    218   // topo do botão QR
#define ADDR_QR_H     38   // altura do botão QR

void drawAddresses() {
  currentScreen = SCR_ADDRESSES;
  bool hasPass = !g_passphrase.isEmpty();
  // Header mostra account atual
  char hdrBuf[28];
  if (hasPass)
    snprintf(hdrBuf, sizeof(hdrBuf), "PASSPHRASE", g_account);
  else
    snprintf(hdrBuf, sizeof(hdrBuf), "ADDRESS", g_account);
  drawHeader(hdrBuf, hasPass ? C_PURPLE : C_TEAL);
  drawBackArrow();

  // ── Abas BIP ──────────────────────────────────────────────────────────────
  uint16_t tabBg[3]     = {C_ORANGE, C_SOFT, C_RED};
  const char* tabLbl[3] = {"BIP44", "BIP49", "BIP84"};
  int tabW = SW / 3;  // 80px

  for (int i = 0; i < 3; i++) {
    uint16_t bg = (g_addrScroll == i) ? tabBg[i] : darken(darken(tabBg[i]));
    tft.fillRoundRect(i * tabW + 3, ADDR_TAB_Y, tabW - 6, ADDR_TAB_H, 8, bg);
    tft.setTextColor(C_WHITE, bg); tft.setTextSize(1);
    int tw = strlen(tabLbl[i]) * 6;
    tft.setCursor(i * tabW + (tabW - tw) / 2, ADDR_TAB_Y + 7);
    tft.print(tabLbl[i]);
  }

  // ── Card de endereço ──────────────────────────────────────────────────────
  char path44[28], path49[28], path84[28];
  snprintf(path44, sizeof(path44), "m/44'/0'/0'/0/%d", g_account);
  snprintf(path49, sizeof(path49), "m/49'/0'/0'/0/%d", g_account);
  snprintf(path84, sizeof(path84), "m/84'/0'/0'/0/%d", g_account);
  const char* paths[3] = {path44, path49, path84};
  String* addrs[3]      = {&g_addrP2PKH, &g_addrP2SH, &g_addrBech32};
  uint16_t cols[3]      = {C_ORANGE, C_GOLD, C_GOLD};

  drawCard(6, ADDR_CARD_Y, SW - 12, ADDR_CARD_H, C_SURFACE, cols[g_addrScroll]);
  drawBadge(14, ADDR_CARD_Y + 8, 52, tabLbl[g_addrScroll], cols[g_addrScroll], C_WHITE);
  if (hasPass) drawBadge(168, ADDR_CARD_Y + 8, 58, "PASS", C_LINE, C_WHITE);

  tft.setTextColor(cols[g_addrScroll], C_SURFACE); tft.setTextSize(1);
  tft.setCursor(70, ADDR_CARD_Y + 12);
  tft.print(paths[g_addrScroll]);

  String& addr = *addrs[g_addrScroll];
  tft.setTextColor(C_WHITE, C_SURFACE);
  if (addr.isEmpty()) {
    tft.setCursor(14, ADDR_CARD_Y + 40);
    tft.setTextColor(C_RED, C_SURFACE);
    tft.print("ADDRESS NAO GERADO!");
  } else {
    int charsPerLine = 20;
    for (int l = 0; l < 6; l++) {
      String line = addr.substring(l * charsPerLine, (l + 1) * charsPerLine);
      if (line.isEmpty()) break;
      tft.setCursor(14, ADDR_CARD_Y + 28 + l * 12);
      tft.print(line);
    }
  }

  // ── Botões NOVA / PASS / SEED ─────────────────────────────────────────────
  drawCard(6, ADDR_BTN_Y - 4, SW - 12, ADDR_BTN_H + 8, C_SURFACE, C_SURFACE2);
  int bw = (SW - 24) / 3;
  drawBtn(8,                  ADDR_BTN_Y, bw, ADDR_BTN_H, "NEW", C_ORANGE, C_BG);
  drawBtn(8 + bw + 4,         ADDR_BTN_Y, bw, ADDR_BTN_H, "PASS", C_PURPLE, C_WHITE);
  drawBtn(8 + (bw + 4) * 2,   ADDR_BTN_Y, bw, ADDR_BTN_H, "SEED", C_PANEL,  C_GOLD);

  // ── Botão QR CODE ─────────────────────────────────────────────────────────
  uint16_t qrColors[3] = {C_ORANGE, C_BLUE, C_TEAL};
  drawBtn(8, ADDR_QR_Y, SW - 16, ADDR_QR_H, "QR CODE", qrColors[g_addrScroll], C_BG);
}

// ─────────────────────────────────────────────────────────────────────────────
// QR CODE
// ─────────────────────────────────────────────────────────────────────────────
void showQRCode(int addrIdx) {
  currentScreen = SCR_QRCODE;
  String* addrs[3]      = {&g_addrP2PKH, &g_addrP2SH, &g_addrBech32};
  const char* labels[3] = {"BIP44 LEGACY", "BIP49 P2SH", "BIP84 SEGWIT"};
  uint16_t colors[3]    = {C_ORANGE, C_BLUE, C_TEAL};

  String& addr = *addrs[addrIdx];
  if (addr.isEmpty()) {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_RED, C_BG); tft.setTextSize(1);
    tft.setCursor(8, 60); tft.print("ADDRESS NAO GERADO.");
    delay(1500); drawAddresses(); return;
  }
  drawQRScreen(tft, addr, labels[addrIdx], colors[addrIdx]);
}

// ─────────────────────────────────────────────────────────────────────────────
// SETTINGS — Portrait
// Brilho: y≈120..160 | RGB: y≈200..240
// ─────────────────────────────────────────────────────────────────────────────
void drawSettings() {
  currentScreen = SCR_SETTINGS;
  drawHeader("   CONFIG", C_DGRAY);
  drawBackArrow(C_GOLD);

  // Card de brilho
  drawCard(8, 52, SW - 16, 110, C_SURFACE, C_GOLD);
  tft.setTextColor(C_GOLD, C_SURFACE); tft.setTextSize(1);
  tft.setCursor(18, 66); tft.print("BRILHO DA TELA");
  drawBadge(150, 62, 72, "DISPLAY", C_GOLD, C_BG);
  tft.setTextColor(C_TEXTDIM, C_SURFACE);
  tft.setCursor(18, 82); tft.print("MOVIMENTE A BARRA.");
  drawCard(90, 92, 60, 24, C_BG, C_WHITE);
  drawBrightnessBar(tft, true);

  // Card RGB
  drawCard(8, 172, SW - 16, 60, C_SURFACE, C_SURFACE2);
  tft.setTextColor(C_GOLD, C_SURFACE); tft.setTextSize(1);
  tft.setCursor(18, 186); tft.print("LED RGB TRASEIRO");
  drawBadge(148, 182, 78, g_rgbEnabled ? "ATIVO" : "DESLIG.",
            g_rgbEnabled ? C_GREEN : C_RED,
            g_rgbEnabled ? C_BG   : C_WHITE);

  uint16_t togCol = g_rgbEnabled ? C_GREEN : C_RED;
  drawBtn(12,  200, 64, 26, g_rgbEnabled ? "ON" : "OFF", togCol, C_BG);
  drawBtn(82,  200, 40, 26, "R", C_RED,   C_WHITE);
  drawBtn(128, 200, 40, 26, "G", C_GREEN, C_BG);
  drawBtn(174, 200, 40, 26, "B", C_BLUE,  C_WHITE);
}

static void redrawSettingsBar() {
  // Coordenadas da barra portrait — espelha drawBrightnessBar(tft, true)
  // big=true: w=min(280, 240-40)=200, x=(240-200)/2=20, y=130, h=28
  const int BAR_X   = 20, BAR_Y = 130, BAR_W = 200, BAR_H = 28;
  const int TRACK_X = BAR_X + 22, TRACK_W = BAR_W - 44;
  const int TRACK_Y = BAR_Y + (BAR_H - 6) / 2;
  const int KNOB_R  = 9;
  const int CTR_Y   = BAR_Y + BAR_H / 2;

  // Percentagem
  drawCard(90, 92, 60, 24, C_BG, C_SURFACE2);
  tft.setTextColor(C_WHITE, C_BG); tft.setTextSize(2);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (int)((long)g_brightness * 100 / 255));
  tft.setCursor(96, 98);
  tft.print(pct);

  // Limpa trilha
  tft.fillRect(TRACK_X - KNOB_R - 1, CTR_Y - KNOB_R - 1,
               TRACK_W + (KNOB_R + 1) * 2, (KNOB_R + 1) * 2, C_SURFACE);
  tft.fillRoundRect(TRACK_X, TRACK_Y, TRACK_W, 6, 3, C_DGRAY);
  int fillW = (int)((long)g_brightness * TRACK_W / 255);
  if (fillW > 0)
    tft.fillRoundRect(TRACK_X, TRACK_Y, fillW, 6, 3, C_GOLD);
  int knobX = constrain(TRACK_X + fillW, TRACK_X + KNOB_R, TRACK_X + TRACK_W - KNOB_R);
  tft.fillCircle(knobX, CTR_Y, KNOB_R, C_WHITE);
  tft.drawCircle(knobX, CTR_Y, KNOB_R, C_GOLD);
}

// ─────────────────────────────────────────────────────────────────────────────
// handleTouch — Portrait 240×320
// ─────────────────────────────────────────────────────────────────────────────
void handleTouch(int tx, int ty) {

  // Brilho — só na tela Settings
  if (currentScreen == SCR_SETTINGS) {
    if (touchBrightnessBar(tft, tx, ty, true)) {
      redrawSettingsBar();
      return;
    }
  }

  // ── HOME — detecção retangular Metro ─────────────────────────────────────
  if (currentScreen == SCR_HOME) {
    // Flash: escurece o tile por 60ms
    auto flashTile = [&](int x, int y, int w, int h, uint16_t col) {
      tft.fillRect(x, y, w, h, darken(darken(col)));
      delay(60);
    };

    if (isPointInRect(tx, ty, MT_12_X, MT_12_Y, MT_TW, MT_TH)) {
      flashTile(MT_12_X, MT_12_Y, MT_TW, MT_TH, METRO_BLUE);
      g_isRecovery = false; g_numWords = 12; doGenerate();
    }
    else if (isPointInRect(tx, ty, MT_24_X, MT_24_Y, MT_TW, MT_TH)) {
      flashTile(MT_24_X, MT_24_Y, MT_TW, MT_TH, METRO_DBLUE);
      g_isRecovery = false; g_numWords = 24; doGenerate();
    }
    else if (isPointInRect(tx, ty, MT_CFG_X, MT_CFG_Y, MT_CFG_W, MT_CFG_H)) {
      flashTile(MT_CFG_X, MT_CFG_Y, MT_CFG_W, MT_CFG_H, METRO_ORANGE);
      drawSettings();
    }
    else if (isPointInRect(tx, ty, MT_REC_X, MT_REC_Y, MT_BW, MT_BH)) {
      flashTile(MT_REC_X, MT_REC_Y, MT_BW, MT_BH, METRO_GREEN);
      g_isRecovery = true; drawChooseWords();
    }
    else if (isPointInRect(tx, ty, MT_KEY_X, MT_KEY_Y, MT_BW, MT_BH)) {
      bool keyOk = !g_keyPassword.isEmpty();
      flashTile(MT_KEY_X, MT_KEY_Y, MT_BW, MT_BH, keyOk ? METRO_GREEN : METRO_PURPLE);
      drawGenKey();
    }
  }

  // ── SETTINGS ──────────────────────────────────────────────────────────────
  else if (currentScreen == SCR_SETTINGS) {
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) { drawHome(); return; }

    if (ty >= 198 && ty <= 230) {
      if      (tx >= 12 && tx <= 76)  { g_rgbEnabled = !g_rgbEnabled; setRGBEnabled(g_rgbEnabled); drawSettings(); }
      else if (tx >= 82 && tx <= 122) { setLedColor(0);   g_ledColorPos = 0;   drawSettings(); }
      else if (tx >= 128 && tx <= 168){ setLedColor(85);  g_ledColorPos = 85;  drawSettings(); }
      else if (tx >= 174 && tx <= 214){ setLedColor(170); g_ledColorPos = 170; drawSettings(); }
    }
  }

  // ── CHOOSE WORDS ──────────────────────────────────────────────────────────
  else if (currentScreen == SCR_CHOOSE_WORDS) {
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) { drawHome(); return; }
    if (isPointInRect(tx, ty, 14, 96, SW - 28, 52)) {
      flashBtn(14, 96, SW - 28, 52, "12 PALAVRAS", C_ORANGE, C_GREEN);
      g_numWords = 12;
      if (g_isRecovery) drawRecoverIntro(); else doGenerate();
    }
    else if (isPointInRect(tx, ty, 14, 158, SW - 28, 52)) {
      flashBtn(14, 158, SW - 28, 52, "24 PALAVRAS", C_ORANGE, C_GREEN);
      g_numWords = 24;
      if (g_isRecovery) drawRecoverIntro(); else doGenerate();
    }
  }

  // ── SHOW SEED ─────────────────────────────────────────────────────────────
  else if (currentScreen == SCR_SHOW_SEED) {
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) { drawHome(); return; }

    // Toque nas abas (só para 24 palavras)
    if (g_numWords == 24 && ty >= 50 && ty <= 68) {
      int tabW = (SW - 8) / 2;
      int newTab = (tx < 4 + tabW) ? 0 : 1;
      if (newTab != g_seedTab) {
        g_seedTab = newTab;
        drawShowSeed();
        return;
      }
    }

    // ── Barra de Zoom ──────────────────────────────────────────────────────
    // Recalcula a posição igual a drawShowSeed()
    {
      int cardYz   = (g_numWords == 24) ? 70 : 52;
      int cardHz   = SH - cardYz - 48 - 32;
      int zoomBarY = cardYz + cardHz + 4;
      if (ty >= zoomBarY && ty <= zoomBarY + 26) {
        if (isPointInRect(tx, ty, 8, zoomBarY, 54, 26)) {
          if (g_seedZoom > 1) { g_seedZoom--; drawShowSeed(); }
          return;
        }
        if (isPointInRect(tx, ty, SW - 64, zoomBarY, 54, 26)) {
          if (g_seedZoom < 2) { g_seedZoom++; drawShowSeed(); }
          return;
        }
      }
    }

    if (isPointInRect(tx, ty, 12, SH - 42, SW - 24, 36)) {
      flashBtn(12, SH - 42, SW - 24, 36, "CONTINUAR", C_GOLD, C_BG);
      drawPassphraseScreen();
    }
  }

  // ── RECOVER INTRO ─────────────────────────────────────────────────────────
  else if (currentScreen == SCR_RECOVER_INTRO) {
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) { drawHome(); return; }
    if (isPointInRect(tx, ty, 30, 136, SW - 60, 52)) {
      flashBtn(30, 136, SW - 60, 52, "INICIAR", C_WHITE, C_BG);
      g_kbTarget = KB_RECOVER_WORD; g_kbLayout = KB_LOWER;
      g_currentInput = ""; g_kbPrevScreen = SCR_RECOVER_INTRO;
      drawKeyboard();
    }
  }

  // ── PASSPHRASE ────────────────────────────────────────────────────────────
  else if (currentScreen == SCR_PASSPHRASE) {
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) {
      if (g_isRecovery) drawRecoverIntro(); else drawShowSeed(); return;
    }
    if (isPointInRect(tx, ty, 8, 134, 106, 52)) {
      flashBtn(8, 134, 106, 52, "SIM", C_PURPLE, C_WHITE);
      g_kbTarget = KB_PASSPHRASE; g_kbLayout = KB_LOWER;
      g_currentInput = ""; g_kbPrevScreen = SCR_PASSPHRASE;
      drawKeyboard();
    }
    else if (isPointInRect(tx, ty, 126, 134, 106, 52)) {
      flashBtn(126, 134, 106, 52, "NAO", C_GREEN, C_BG);
      g_passphrase = ""; drawAccountScreen();
    }
  }

  // ── ACCOUNT ───────────────────────────────────────────────────────────────
  else if (currentScreen == SCR_ACCOUNT) {
    // Voltar
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) {
      drawPassphraseScreen(); return;
    }

    int totalW = 5 * ACC_BTN_W + 4 * ACC_BTN_GAP;
    int xOff   = (SW - totalW) / 2;

    // ── Numpad: linhas 0..4 e 5..9 ────────────────────────────────────────
    auto handleDigit = [&](int digit) {
      char d[2] = { (char)('0' + digit), 0 };
      // Flash do botão
      int col = digit % 5, row = digit / 5;
      int bx  = xOff + col * (ACC_BTN_W + ACC_BTN_GAP);
      int by  = (row == 0) ? ACC_ROW1_Y : ACC_ROW2_Y;
      flashBtn(bx, by, ACC_BTN_W, ACC_BTN_H, d, C_TEAL, C_BG);

      // Lógica de acumulação
      if (g_accDigits == "0") {
        // Substitui o zero inicial (exceto se digitou 0 de novo)
        g_accDigits = String(d);
      } else {
        // Acumula — limita a 3 dígitos (índice máx 999)
        if (g_accDigits.length() < 3) {
          String candidate = g_accDigits + String(d);
          g_accDigits = candidate;
        }
        // Se já tem 3 dígitos, ignora (overflow silencioso)
      }
      g_account = g_accDigits.toInt();
      accRedrawDisplay();
      // Redesenha os botões para atualizar destaques
      for (int i2 = 0; i2 < 10; i2++) {
        char lbl2[3]; snprintf(lbl2, sizeof(lbl2), "%d", i2);
        int c2 = i2 % 5, r2 = i2 / 5;
        int bx2 = xOff + c2 * (ACC_BTN_W + ACC_BTN_GAP);
        int by2 = (r2 == 0) ? ACC_ROW1_Y : ACC_ROW2_Y;
        bool last2 = (g_accDigits.length() > 0 &&
                      g_accDigits[g_accDigits.length()-1] == lbl2[0]);
        bool act2  = (g_accDigits.indexOf(lbl2[0]) >= 0);
        uint16_t bg2 = last2 ? C_TEAL : act2 ? C_SURFACE2 : C_PANEL;
        uint16_t fg2 = last2 ? C_BG : C_WHITE;
        drawBtn(bx2, by2, ACC_BTN_W, ACC_BTN_H, lbl2, bg2, fg2);
      }
    };

    if (ty >= ACC_ROW1_Y && ty <= ACC_ROW1_Y + ACC_BTN_H) {
      for (int i = 0; i < 5; i++) {
        int bx = xOff + i * (ACC_BTN_W + ACC_BTN_GAP);
        if (tx >= bx && tx <= bx + ACC_BTN_W) { handleDigit(i); return; }
      }
    }
    if (ty >= ACC_ROW2_Y && ty <= ACC_ROW2_Y + ACC_BTN_H) {
      for (int i = 5; i < 10; i++) {
        int bx = xOff + (i - 5) * (ACC_BTN_W + ACC_BTN_GAP);
        if (tx >= bx && tx <= bx + ACC_BTN_W) { handleDigit(i); return; }
      }
    }

    // ── Barra inferior ─────────────────────────────────────────────────────
    if (ty >= ACC_BAR_Y && ty <= ACC_BAR_Y + 40) {
      // Botão DEL (backspace)
      if (isPointInRect(tx, ty, 8, ACC_BAR_Y, 68, 40)) {
        flashBtn(8, ACC_BAR_Y, 68, 40, "DEL", C_SURFACE2, C_RED);
        if (g_accDigits.length() > 1) {
          g_accDigits.remove(g_accDigits.length() - 1);
        } else {
          g_accDigits = "0";
        }
        g_account = g_accDigits.toInt();
        accRedrawDisplay();
        // Redesenha botões
        for (int i2 = 0; i2 < 10; i2++) {
          char lbl2[3]; snprintf(lbl2, sizeof(lbl2), "%d", i2);
          int c2 = i2 % 5, r2 = i2 / 5;
          int bx2 = xOff + c2 * (ACC_BTN_W + ACC_BTN_GAP);
          int by2 = (r2 == 0) ? ACC_ROW1_Y : ACC_ROW2_Y;
          bool last2 = (g_accDigits.length() > 0 &&
                        g_accDigits[g_accDigits.length()-1] == lbl2[0]);
          bool act2  = (g_accDigits.indexOf(lbl2[0]) >= 0);
          uint16_t bg2 = last2 ? C_TEAL : act2 ? C_SURFACE2 : C_PANEL;
          uint16_t fg2 = last2 ? C_BG : C_WHITE;
          drawBtn(bx2, by2, ACC_BTN_W, ACC_BTN_H, lbl2, bg2, fg2);
        }
        return;
      }
      // Botão CONTINUAR
      if (isPointInRect(tx, ty, 82, ACC_BAR_Y, SW - 90, 40)) {
        flashBtn(82, ACC_BAR_Y, SW - 90, 40, "CONTINUAR", C_GOLD, C_BG);
        g_account = g_accDigits.toInt();
        doDerive();
      }
    }
  }
  else if (currentScreen == SCR_KEYBOARD) {
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) {
      kbClearSuggestions();
      if      (g_kbPrevScreen == SCR_PASSPHRASE)    drawPassphraseScreen();
      else if (g_kbPrevScreen == SCR_RECOVER_INTRO) drawRecoverIntro();
      else if (g_kbPrevScreen == SCR_GENKEY)        drawGenKey();
      else                                           drawHome();
      return;
    }

    // ── Sugestões BIP39 ───────────────────────────────────────────────────────
    if (g_suggestVisible && g_suggestCount >= 1 &&
        ty >= SUGG_Y && ty < SUGG_Y + SUGG_H) {
      int n = g_suggestCount;
      int btnW = (SW - (n + 1) * SUGG_GAP) / n;
      for (int i = 0; i < n; i++) {
        int bx = SUGG_GAP + i * (btnW + SUGG_GAP);
        if (tx >= bx && tx < bx + btnW) {
          g_currentInput = String(g_suggestions[i]);
          g_suggestCount = 0; g_suggestVisible = false;
          kbRedrawInput(); delay(200);
          commitRecoveryWord(g_currentInput);
          return;
        }
      }
      return;
    }

    // ── Linhas 0 e 1 ─────────────────────────────────────────────────────────
    const char** rows = (g_kbLayout == KB_LOWER) ? KB_ROWS_LOWER :
                        (g_kbLayout == KB_UPPER) ? KB_ROWS_UPPER : KB_ROWS_NUM;
    uint16_t keyBg = kbKeyColor();

    for (int row = 0; row < 2; row++) {
      const char* r  = rows[row];
      int len    = strlen(r);
      int totalW = len * KB_KEY_W + (len - 1) * KB_KEY_GAP;
      int xOff   = (SW - totalW) / 2;
      int ky     = KB_KEYS_Y + row * (KB_KEY_H + KB_ROW_GAP);
      if (ty >= ky && ty < ky + KB_KEY_H) {
        for (int c = 0; c < len; c++) {
          int kx = xOff + c * (KB_KEY_W + KB_KEY_GAP);
          if (tx >= kx && tx < kx + KB_KEY_W) {
            char lbl[2] = {r[c], 0};
            pressKey(kx, ky, KB_KEY_W, KB_KEY_H, lbl, keyBg, C_WHITE);
            kbHandleKey(lbl); return;
          }
        }
      }
    }

    // ── Linha 2: [⬆ CAPS] [zxcvbnm / símbolos] [DEL] ─────────────────────────
    int ky2 = KB_KEYS_Y + 2 * (KB_KEY_H + KB_ROW_GAP);
    if (ty >= ky2 && ty < ky2 + KB_KEY_H) {
      // Botão CAPS — inativo em modo números
      if (tx >= KB_R2_CAPS_X && tx < KB_R2_CAPS_X + KB_R2_CAPS_W) {
        if (g_kbLayout != KB_NUMBERS) {
          pressCapsKey(KB_R2_CAPS_X, ky2, KB_R2_CAPS_W, KB_KEY_H, g_kbLayout == KB_UPPER);
          kbHandleKey("CAPS");
        }
        return;
      }
      // Letras / símbolos da linha 2
      const char* r2 = rows[2];
      int r2len = strlen(r2);
      for (int c = 0; c < r2len; c++) {
        int kx = KB_R2_LTR_X + c * (KB_KEY_W + KB_KEY_GAP);
        if (tx >= kx && tx < kx + KB_KEY_W) {
          char lbl[2] = {r2[c], 0};
          pressKey(kx, ky2, KB_KEY_W, KB_KEY_H, lbl, keyBg, C_WHITE);
          kbHandleKey(lbl); return;
        }
      }
      // DEL
      if (tx >= KB_R2_DEL_X && tx < KB_R2_DEL_X + KB_R2_DEL_W) {
        pressKey(KB_R2_DEL_X, ky2, KB_R2_DEL_W, KB_KEY_H, "DEL", C_RED, C_WHITE);
        kbHandleKey("DEL"); return;
      }
    }

    // ── Barra inferior ────────────────────────────────────────────────────────
    if (ty >= KB_BAR_Y && ty < KB_BAR_Y + KB_BAR_H) {
      // Botão NUMS: "123" entra em números, "ABC" volta para letras
      if (tx >= KB_BAR_NUMS_X && tx < KB_BAR_NUMS_X + KB_BAR_NUMS_W) {
        const char* numsLbl = (g_kbLayout == KB_NUMBERS) ? "ABC" : "123";
        pressKey(KB_BAR_NUMS_X, KB_BAR_Y, KB_BAR_NUMS_W, KB_BAR_H, numsLbl, C_SURFACE2, C_YELLOW);
        kbHandleKey("NUMS"); return;
      }
      if (tx >= KB_BAR_SPC_X && tx < KB_BAR_SPC_X + KB_BAR_SPC_W) {
        pressKey(KB_BAR_SPC_X, KB_BAR_Y, KB_BAR_SPC_W, KB_BAR_H, "ESPACO", C_PANEL, C_WHITE);
        kbHandleKey("ESPACO"); return;
      }
      if (tx >= KB_BAR_OK_X && tx < KB_BAR_OK_X + KB_BAR_OK_W) {
        uint16_t okCol = (g_kbTarget == KB_PASSPHRASE || g_kbTarget == KB_GENKEY) ? C_GREEN
                         : (g_currentInput.isEmpty() ? C_DGRAY : C_GREEN);
        pressKey(KB_BAR_OK_X, KB_BAR_Y, KB_BAR_OK_W, KB_BAR_H, "OK", okCol, C_WHITE);
        kbHandleKey("OK"); return;
      }
    }
  }

  // ── GENKEY ────────────────────────────────────────────────────────────────
  else if (currentScreen == SCR_GENKEY) {
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) { drawHome(); return; }
    if (isPointInRect(tx, ty, 12, 134, SW - 24, 40)) {
      flashBtn(12, 134, SW - 24, 40, "DIGITAR SENHA", C_WHITE, C_BG);
      g_kbTarget = KB_GENKEY; g_kbLayout = KB_LOWER;
      g_currentInput = ""; g_kbPrevScreen = SCR_GENKEY;
      drawKeyboard();
    }
    else if (isPointInRect(tx, ty, 12, 182, SW - 24, 40)) {
      flashBtn(12, 182, SW - 24, 40, "GERAR E SALVAR .KEY", C_TEAL, C_BG);
      doSaveKeyFile();
    }
  }

  // ── ADDRESSES ─────────────────────────────────────────────────────────────
  else if (currentScreen == SCR_ADDRESSES) {
    if (tx >= 5 && tx <= 35 && ty >= 5 && ty <= 35) { drawHome(); return; }

    // Abas BIP44/49/84
    int tabW = SW / 3;
    if (ty >= ADDR_TAB_Y && ty <= ADDR_TAB_Y + ADDR_TAB_H) {
      int tab = tx / tabW;
      if (tab >= 0 && tab <= 2 && tab != g_addrScroll) {
        g_addrScroll = tab; drawAddresses(); return;
      }
    }

    // Botões NOVA / PASS / SEED
    int bw = (SW - 24) / 3;
    if (ty >= ADDR_BTN_Y && ty <= ADDR_BTN_Y + ADDR_BTN_H) {
      if      (tx < 8 + bw)           { flashBtn(8, ADDR_BTN_Y, bw, ADDR_BTN_H, "NOVA", C_ORANGE, C_BG); g_mnemonic = ""; g_passphrase = ""; g_currentInput = ""; drawHome(); }
      else if (tx < 8 + (bw + 4) * 2) { flashBtn(8 + bw + 4, ADDR_BTN_Y, bw, ADDR_BTN_H, "PASS", C_PURPLE, C_WHITE); drawPassphraseScreen(); }
      else                             { flashBtn(8 + (bw + 4) * 2, ADDR_BTN_Y, bw, ADDR_BTN_H, "SEED", C_PANEL, C_GOLD); drawShowSeed(); }
    }

    // Botão QR CODE
    if (isPointInRect(tx, ty, 8, ADDR_QR_Y, SW - 16, ADDR_QR_H)) {
      uint16_t qrColors[3] = {C_ORANGE, C_BLUE, C_TEAL};
      flashBtn(8, ADDR_QR_Y, SW - 16, ADDR_QR_H, "QR CODE", qrColors[g_addrScroll], C_BG);
      showQRCode(g_addrScroll);
    }
  }

  // ── QRCODE ────────────────────────────────────────────────────────────────
  else if (currentScreen == SCR_QRCODE) {
    drawAddresses();
  }
}