// =============================================
// BTCWallet_QRCode.h
// Usa a biblioteca "QRCode" de Richard Moore
// Localização: /home/fox/Arduino/libraries/QRCode/src/qrcode.h
// =============================================

#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>
#include <string.h>
#include <qrcode.h>

// Cores do projeto
#ifndef C_BG
  #define C_BG    0x0000
  #define C_WHITE 0xFFFF
  #define C_GOLD  0xFEA0
  #define C_DGRAY 0x39E7
#endif
#ifndef C_PANEL
  #define C_PANEL   0x1082
#endif
#ifndef C_SURFACE
  #define C_SURFACE 0x18C3
#endif

// Cores fixas para o QR — não dependem do driver
#define QR_BLACK  ((uint16_t)0x0000)
#define QR_WHITE  ((uint16_t)0xFFFF)

// Buffer fixo para versão 4 ECC_MEDIUM = 137 bytes (sem VLA)
// Cobre todos os formatos Bitcoin: Legacy/P2SH(34) Bech32(42) Bech32m(62)
#define QR_BTC_BUF  137

// -----------------------------------------------------------------------------
void drawQRScreen(TFT_eSPI& tft, const String& addr,
                  const char* label, uint16_t color) {
  int screenW = tft.width();
  int screenH = tft.height();
  int headerH = 44;
  int footerY = screenH - 20;
  int footerW = screenW - 20;

  tft.fillScreen(C_BG);
  tft.fillCircle(screenW - 34, 12, 76, C_PANEL);
  tft.fillCircle(28, screenH - 20, 50, C_SURFACE);

  tft.fillRect(0, 0, screenW, headerH, color);
  tft.fillRect(0, headerH - 4, screenW, 4, C_WHITE);

  String title = String(label);
  title.trim();
  tft.setTextColor(QR_WHITE, color);
  tft.setTextSize(2);
  int tw = title.length() * 12;
  tft.setCursor((screenW - tw) / 2, 7);
  tft.print(title);

  tft.setTextColor(C_GOLD, C_BG);
  tft.setTextSize(1);
  tft.setCursor(106, 120);
  tft.print("Gerando QR...");

  // Versão mínima para ECC_MEDIUM
  // Capacidades: v1=14, v2=26, v3=42, v4=62 chars
  int len = (int)addr.length();
  uint8_t ver;
  if      (len <= 14) ver = 1;
  else if (len <= 26) ver = 2;
  else if (len <= 42) ver = 3;
  else                ver = 4;

  // Declara usando o nome exato da struct do qrcode.h de Richard Moore
  struct QRCode qr;
  uint8_t buf[QR_BTC_BUF];
  memset(buf, 0, sizeof(buf));

  // qrcode_initText: retorna 0 = sucesso, -1 = erro
  int8_t rc = qrcode_initText(&qr, buf, ver, ECC_MEDIUM, addr.c_str());

  tft.fillRect(80, 112, 160, 16, C_BG);

  if (rc != 0) {
    tft.setTextColor(0xF800, C_BG);
    tft.setTextSize(1);
    tft.setCursor(8, 100);
    tft.print("Falha ao gerar QR");
    tft.setCursor(8, 116);
    tft.printf("ver=%d len=%d rc=%d", ver, len, (int)rc);
    return;
  }

  const int QUIET  = 1;
  const int AREA_W = screenW;
  const int AREA_H = screenH - headerH - 20;

  int totalMod   = (int)qr.size + QUIET * 2;
  int moduleSize = min(AREA_W, AREA_H) / totalMod;
  if (moduleSize < 1) moduleSize = 1;
  if (moduleSize > 6) moduleSize = 6;

  int qrPx = totalMod * moduleSize;
  int xOff = (AREA_W - qrPx) / 2;
  int yOff = headerH + 2 + (AREA_H - qrPx) / 2;

  tft.fillRoundRect(xOff - 12, yOff - 12, qrPx + 24, qrPx + 24, 12, QR_WHITE);
  tft.drawRoundRect(xOff - 12, yOff - 12, qrPx + 24, qrPx + 24, 12, color);
  tft.fillRect(xOff, yOff, qrPx, qrPx, QR_WHITE);

  int x0 = xOff + QUIET * moduleSize;
  int y0 = yOff + QUIET * moduleSize;

  for (uint8_t row = 0; row < qr.size; row++) {
    for (uint8_t col = 0; col < qr.size; col++) {
      if (qrcode_getModule(&qr, col, row)) {
        tft.fillRect(
          x0 + col * moduleSize,
          y0 + row * moduleSize,
          moduleSize, moduleSize,
          QR_BLACK
        );
      }
    }
  }

  tft.fillRoundRect(10, footerY, footerW, 14, 7, C_PANEL);
  tft.drawRoundRect(10, footerY, footerW, 14, 7, C_DGRAY);
  tft.setTextColor(C_GOLD, C_PANEL);
  tft.setTextSize(1);
  String disp = addr;
  if (addr.length() > 34) {
    disp = addr.substring(0, 16) + "..." + addr.substring(addr.length() - 10);
  }
  int dw = disp.length() * 6;
  tft.setCursor((screenW - dw) / 2, footerY + 4);
  tft.print(disp);
}
