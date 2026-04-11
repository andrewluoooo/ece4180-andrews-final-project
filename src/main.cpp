#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include "epd2in13_V4.h"
#include "epdpaint.h"
#include "leaderboard.h"

// Lafvin / Waveshare 2.13" V4: 250 x 122, 1 bit per pixel (see
// https://lafvin-213inch-epaper-hat.readthedocs.io/en/latest/about_this_kit.html )
#define COLORED     0
#define UNCOLORED   1

// Five-way switch (active LOW + INPUT_PULLUP). Change pins to match your wiring.
#define PIN_UP 5
#define PIN_CENTER 4
#define PIN_LEFT 3
#define PIN_DOWN 2
#define PIN_RIGHT 1

// Momentary button to 3V3: clears panel (active HIGH + internal pull-down).
#define PIN_CLEAR_EPD 6

enum Dir : uint8_t { DIR_NONE = 0xFF, DIR_UP = 0, DIR_CENTER, DIR_LEFT, DIR_DOWN, DIR_RIGHT };

static volatile Dir gPendingDir = DIR_NONE;
static volatile bool gClearEpdPending = false;

// Ignore new navigation until this many ms after the last accepted press (post-debounce),
// so the EPD can finish updating before the next edge is handled.
static constexpr uint32_t kNavMinIntervalMs = 450;
static uint32_t gLastNavAcceptedMs = 0;

void IRAM_ATTR isrUp() { gPendingDir = DIR_UP; }
void IRAM_ATTR isrCenter() { gPendingDir = DIR_CENTER; }
void IRAM_ATTR isrLeft() { gPendingDir = DIR_LEFT; }
void IRAM_ATTR isrDown() { gPendingDir = DIR_DOWN; }
void IRAM_ATTR isrRight() { gPendingDir = DIR_RIGHT; }

void IRAM_ATTR isrClearEpd() { gClearEpdPending = true; }

void drawStringBold(Paint &p, int x, int y, const char *s, sFONT *f, int c) {
  p.DrawStringAt(x, y, s, f, c);
  p.DrawStringAt(x + 1, y, s, f, c);
}

// 0 = Start Game, 1 = Leaderboard
static int gMenuSel = 0;

enum class AppScreen : uint8_t { kMainMenu, kLeaderboard, kGame, kNameEntry };
static AppScreen gScreen = AppScreen::kMainMenu;

static uint32_t gRunScore = 0;
static int gGameMenuSel = 0;

static int gKbRow = 0;
static int gKbCol = 0;
static char gInitials[4] = {'\0', '\0', '\0', '\0'};
static int gNameLen = 0;

static const char *kbKeyRow(int r) {
  static const char *const rows[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM<"};
  if (r < 0 || r > 3) {
    return "";
  }
  return rows[r];
}

static int kbKeyRowLen(int r) {
  return static_cast<int>(strlen(kbKeyRow(r)));
}

static bool kbHasOkRow() { return gNameLen >= 3; }

static void kbClampCol() {
  if (gKbRow >= 4) {
    gKbRow = 4;
    gKbCol = 0;
    return;
  }
  const int mx = kbKeyRowLen(gKbRow) - 1;
  if (mx < 0) {
    gKbCol = 0;
    return;
  }
  if (gKbCol > mx) {
    gKbCol = mx;
  }
  if (gKbCol < 0) {
    gKbCol = 0;
  }
}

static void drawLeaderboardPage(Paint &p) {
  p.Clear(UNCOLORED);
  p.SetRotate(ROTATE_270);
  p.DrawStringAt(55, 5, "LEADERBOARD", &Font16, COLORED);
  char row[28];
  leaderboardFormatRow(1, row, sizeof(row));
  p.DrawStringAt(8, 30, row, &Font12, COLORED);
  leaderboardFormatRow(2, row, sizeof(row));
  p.DrawStringAt(8, 46, row, &Font12, COLORED);
  leaderboardFormatRow(3, row, sizeof(row));
  p.DrawStringAt(8, 62, row, &Font12, COLORED);
  p.DrawStringAt(12, 100, "< Menu", &Font12, COLORED);
  p.SetRotate(ROTATE_0);
}

static void drawMainMenu(Paint &p) {
  p.Clear(UNCOLORED);
  p.SetRotate(ROTATE_270);
  constexpr int kTitleX = 60;
  constexpr int kTitleY = 5;
  constexpr int kLogoR = 21;
  constexpr int kLogoCx = kTitleX - kLogoR - 8;
  constexpr int kLogoCy = kTitleY + 32;
  p.DrawFilledCircle(kLogoCx, kLogoCy, kLogoR, COLORED);
  p.DrawStringAt(kTitleX, kTitleY, "SKEEBALL", &Font24, COLORED);

  const int mx = 60;
  const int y0 = 30;
  const int y1 = 45;

  if (gMenuSel == 0) {
    drawStringBold(p, mx, y0, "> Start Game", &Font12, COLORED);
    p.DrawStringAt(mx, y1, "  Leaderboard", &Font12, COLORED);
  } else {
    p.DrawStringAt(mx, y0, "  Start Game", &Font12, COLORED);
    drawStringBold(p, mx, y1, "> Leaderboard", &Font12, COLORED);
  }
  p.SetRotate(ROTATE_0);
}

static const int kPtsAdd[5] = {10, 20, 30, 40, 50};

static void drawGameScreen(Paint &p) {
  p.Clear(UNCOLORED);
  p.SetRotate(ROTATE_270);
  p.DrawStringAt(55, 5, "GAME", &Font16, COLORED);
  char buf[32];
  snprintf(buf, sizeof(buf), "Pts: %lu", static_cast<unsigned long>(gRunScore));
  p.DrawStringAt(10, 22, buf, &Font12, COLORED);

  constexpr int mx = 8;
  int y = 38;
  constexpr int line = 14;

  for (int i = 0; i < 5; ++i) {
    if (gGameMenuSel == i) {
      snprintf(buf, sizeof(buf), "> +%d", kPtsAdd[i]);
      drawStringBold(p, mx, y, buf, &Font12, COLORED);
    } else {
      snprintf(buf, sizeof(buf), "  +%d", kPtsAdd[i]);
      p.DrawStringAt(mx, y, buf, &Font12, COLORED);
    }
    y += line;
  }
  if (gGameMenuSel == 5) {
    drawStringBold(p, mx, y, "> End Game", &Font12, COLORED);
  } else {
    p.DrawStringAt(mx, y, "  End Game", &Font12, COLORED);
  }
  p.SetRotate(ROTATE_0);
}

static void drawNameEntry(Paint &p) {
  p.Clear(UNCOLORED);
  p.SetRotate(ROTATE_270);
  p.DrawStringAt(38, 2, "YOUR TAG", &Font12, COLORED);

  char preview[8];
  snprintf(preview, sizeof(preview), "%c %c %c",
           gNameLen > 0 ? gInitials[0] : '_',
           gNameLen > 1 ? gInitials[1] : '_',
           gNameLen > 2 ? gInitials[2] : '_');
  p.DrawStringAt(32, 16, preview, &Font20, COLORED);

  // Key caps Font16 (~11x16); ROTATE_270 uses x along the long panel axis.
  constexpr int kbBaseX = 2;
  constexpr int kbBaseY = 38;
  constexpr int cellW = 12;
  constexpr int rowH = 16;
  constexpr int kKeyFontW = 11;
  constexpr int kKeyFontH = 16;

  for (int r = 0; r < 4; ++r) {
    const char *s = kbKeyRow(r);
    const int len = static_cast<int>(strlen(s));
    for (int c = 0; c < len; ++c) {
      const int px = kbBaseX + c * cellW;
      const int py = kbBaseY + r * rowH;
      char one[2] = {s[c], '\0'};
      if (r == gKbRow && c == gKbCol && gKbRow < 4) {
        p.DrawFilledRectangle(px - 1, py - 1, px + kKeyFontW, py + kKeyFontH, COLORED);
        p.DrawStringAt(px, py, one, &Font16, UNCOLORED);
      } else {
        p.DrawStringAt(px, py, one, &Font16, COLORED);
      }
    }
  }

  if (kbHasOkRow()) {
    constexpr int okX = 50;
    constexpr int okY = kbBaseY + 4 * rowH + 2;
    if (gKbRow == 4) {
      drawStringBold(p, okX, okY, "> OK", &Font16, COLORED);
    } else {
      p.DrawStringAt(okX, okY, "  OK", &Font16, COLORED);
    }
  }

  p.SetRotate(ROTATE_0);
}

static void attachFiveWayInterrupts() {
  const int pins[] = {PIN_UP, PIN_CENTER, PIN_LEFT, PIN_DOWN, PIN_RIGHT};
  void (*const handlers[])() = {isrUp, isrCenter, isrLeft, isrDown, isrRight};
  for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    pinMode(pins[i], INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pins[i]), handlers[i], FALLING);
  }
}

static void attachClearEpdInterrupt() {
  pinMode(PIN_CLEAR_EPD, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PIN_CLEAR_EPD), isrClearEpd, RISING);
}

static constexpr int kImageBytes = ((EPD_WIDTH + 7) / 8) * EPD_HEIGHT;

unsigned char image[kImageBytes];
Paint paint(image, EPD_WIDTH, EPD_HEIGHT);
Epd epd;

static void drawCurrentScreen() {
  switch (gScreen) {
    case AppScreen::kMainMenu:
      drawMainMenu(paint);
      break;
    case AppScreen::kLeaderboard:
      drawLeaderboardPage(paint);
      break;
    case AppScreen::kGame:
      drawGameScreen(paint);
      break;
    case AppScreen::kNameEntry:
      drawNameEntry(paint);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  epd.Init(FAST);
  gMenuSel = 0;
  gScreen = AppScreen::kMainMenu;
  gRunScore = 0;
  gGameMenuSel = 0;
  gKbRow = 0;
  gKbCol = 0;
  gNameLen = 0;
  gInitials[0] = gInitials[1] = gInitials[2] = gInitials[3] = '\0';

  leaderboardInit();
  drawMainMenu(paint);
  epd.Display_Fast(image);
  Serial.println("Menu: UP/DOWN, CENTER. Start Game -> play -> name -> save.");

  attachFiveWayInterrupts();
  attachClearEpdInterrupt();
  Serial.println("Five-way (FALL/low) + GPIO6 clear (RISING/high, pull-down).");
}

void loop() {
  bool clearReq = false;
  noInterrupts();
  if (gClearEpdPending) {
    gClearEpdPending = false;
    clearReq = true;
  }
  interrupts();

  if (clearReq) {
    delay(40);
    if (digitalRead(PIN_CLEAR_EPD) == HIGH) {
      paint.Clear(UNCOLORED);
      epd.Display_Fast(image);
      Serial.println("EPD cleared (GPIO6)");
    }
  }

  Dir hit = DIR_NONE;
  noInterrupts();
  if (gPendingDir != DIR_NONE) {
    hit = gPendingDir;
    gPendingDir = DIR_NONE;
  }
  interrupts();

  if (hit == DIR_NONE) {
    delay(5);
    return;
  }

  delay(40);
  const int pinFor[] = {PIN_UP, PIN_CENTER, PIN_LEFT, PIN_DOWN, PIN_RIGHT};
  if (digitalRead(pinFor[hit]) != LOW) {
    delay(5);
    return;
  }

  const uint32_t now = millis();
  if (gLastNavAcceptedMs != 0 && (now - gLastNavAcceptedMs) < kNavMinIntervalMs) {
    noInterrupts();
    gPendingDir = hit;
    interrupts();
    delay(5);
    return;
  }

  const AppScreen prevScreen = gScreen;
  const int prevMenuSel = gMenuSel;
  const uint32_t prevRunScore = gRunScore;
  const int prevGameSel = gGameMenuSel;
  const int prevKbRow = gKbRow;
  const int prevKbCol = gKbCol;
  const int prevNameLen = gNameLen;

  if (gScreen == AppScreen::kMainMenu) {
    if (hit == DIR_UP) {
      if (gMenuSel > 0) {
        gMenuSel--;
      }
    } else if (hit == DIR_DOWN) {
      if (gMenuSel < 1) {
        gMenuSel++;
      }
    } else if (hit == DIR_CENTER) {
      if (gMenuSel == 1) {
        gScreen = AppScreen::kLeaderboard;
      } else {
        gScreen = AppScreen::kGame;
        gRunScore = 0;
        gGameMenuSel = 0;
      }
    }
  } else if (gScreen == AppScreen::kLeaderboard) {
    if (hit == DIR_LEFT) {
      gScreen = AppScreen::kMainMenu;
    }
  } else if (gScreen == AppScreen::kGame) {
    if (hit == DIR_UP) {
      if (gGameMenuSel > 0) {
        gGameMenuSel--;
      }
    } else if (hit == DIR_DOWN) {
      if (gGameMenuSel < 5) {
        gGameMenuSel++;
      }
    } else if (hit == DIR_CENTER) {
      if (gGameMenuSel < 5) {
        gRunScore += static_cast<uint32_t>(kPtsAdd[gGameMenuSel]);
      } else {
        if (gRunScore == 0) {
          gScreen = AppScreen::kMainMenu;
        } else {
          gScreen = AppScreen::kNameEntry;
          gNameLen = 0;
          gInitials[0] = gInitials[1] = gInitials[2] = gInitials[3] = '\0';
          gKbRow = 0;
          gKbCol = 0;
        }
      }
    }
  } else if (gScreen == AppScreen::kNameEntry) {
    if (gKbRow == 4 && kbHasOkRow()) {
      if (hit == DIR_CENTER) {
        if (gNameLen == 3 && gRunScore > 0) {
          leaderboardSubmitEntry(gRunScore, gInitials);
          gScreen = AppScreen::kMainMenu;
          gRunScore = 0;
          Serial.println("Score saved; main menu");
        }
      } else if (hit == DIR_UP) {
        gKbRow = 3;
        kbClampCol();
      }
    } else {
      if (hit == DIR_LEFT) {
        if (gKbCol > 0) {
          gKbCol--;
        }
      } else if (hit == DIR_RIGHT) {
        const int mx = kbKeyRowLen(gKbRow) - 1;
        if (gKbCol < mx) {
          gKbCol++;
        }
      } else if (hit == DIR_UP) {
        if (gKbRow > 0) {
          gKbRow--;
          kbClampCol();
        }
      } else if (hit == DIR_DOWN) {
        if (gKbRow < 3) {
          gKbRow++;
          kbClampCol();
        } else if (gKbRow == 3 && kbHasOkRow()) {
          gKbRow = 4;
          gKbCol = 0;
        }
      } else if (hit == DIR_CENTER) {
        const char *row = kbKeyRow(gKbRow);
        if (gKbRow < 4 && gKbCol < static_cast<int>(strlen(row))) {
          const char ch = row[gKbCol];
          if (ch == '<') {
            if (gNameLen > 0) {
              gNameLen--;
              gInitials[gNameLen] = '\0';
            }
          } else if (gNameLen < 3) {
            gInitials[gNameLen] = ch;
            gNameLen++;
            gInitials[gNameLen] = '\0';
            if (gNameLen == 3 && gKbRow < 4) {
              gKbRow = 4;
              gKbCol = 0;
            }
          }
        }
      }
    }
    if (gKbRow == 4 && !kbHasOkRow()) {
      gKbRow = 3;
      kbClampCol();
    }
  }

  const bool dirty = (gScreen != prevScreen) ||
                     (gScreen == AppScreen::kMainMenu && gMenuSel != prevMenuSel) ||
                     (gScreen == AppScreen::kGame &&
                      (gRunScore != prevRunScore || gGameMenuSel != prevGameSel)) ||
                     (gScreen == AppScreen::kNameEntry &&
                      (gKbRow != prevKbRow || gKbCol != prevKbCol || gNameLen != prevNameLen));

  if (dirty) {
    drawCurrentScreen();
    epd.Display_Fast(image);
  }

  gLastNavAcceptedMs = millis();

  delay(5);
}
