#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <MacAddress.h>
#include <ESP32_NOW_Serial.h>
#include "epd2in13_V4.h"
#include "epdpaint.h"
#include "leaderboard.h"
 
#define COLORED   0
#define UNCOLORED 1
 
#define PIN_UP     5
#define PIN_CENTER 4
#define PIN_LEFT   3
#define PIN_DOWN   2
#define PIN_RIGHT  1
 
#define PIN_CLEAR_EPD 6
#define PIN_MUSIC_TOGGLE 7
#define SPEAKER_VOICE_1_PIN 0
#define SPEAKER_VOICE_2_PIN 10
#define SPEAKER_PWM_INIT_FREQ_HZ 440
#define SPEAKER_PWM_RES_BITS 8
 
#define NAV_MIN_INTERVAL_MS 450
 
#define ESPNOW_CHANNEL   1
#define GAME_START_BYTE  0xAA
#define GAME_END_BYTE    0xBB
 
static const MacAddress PEER_MAC({0x10, 0x51, 0xDB, 0x01, 0x91, 0xB4});
 
#define THROWS_TOTAL 10   // 5 throws per player, alternating P1/P2
 
// Points awarded per beam (index 0..6 maps to bits 0..6 of the beam mask)
static const uint16_t BEAM_POINTS[7] = {10, 20, 30, 40, 50, 100, 100};
 
enum Direction {
    DIR_NONE   = 0xFF,
    DIR_UP     = 0,
    DIR_CENTER = 1,
    DIR_LEFT   = 2,
    DIR_DOWN   = 3,
    DIR_RIGHT  = 4
};
 
enum AppScreen {
    SCREEN_MAIN_MENU,
    SCREEN_LEADERBOARD,
    SCREEN_GAME,
    SCREEN_GAME_OVER,
    SCREEN_NAME_ENTRY
};
 
enum RoundOutcome {
    OUTCOME_P1_WINS,
    OUTCOME_P2_WINS,
    OUTCOME_TIE
};
 
static volatile Direction gPendingDir   = DIR_NONE;
static volatile bool      gClearPending = false;
 
static AppScreen    gScreen    = SCREEN_MAIN_MENU;
static int          gMenuSel   = 0;   // 0 = Start Game, 1 = Leaderboard
static RoundOutcome gOutcome   = OUTCOME_TIE;
 
static uint32_t gScoreP1  = 0;
static uint32_t gScoreP2  = 0;
static uint32_t gRunScore = 0;   // winning score carried to name entry
static int      gThrowIdx = 0;   // 0..THROWS_TOTAL
 
static uint32_t gLastNavMs = 0;
 
// Keyboard / name-entry state
static int  gKbRow  = 0;
static int  gKbCol  = 0;
static char gInitials[4] = {'\0'};
static int  gNameLen = 0;
 
// ESP-NOW
static ESP_NOW_Serial_Class *gNowSerial = nullptr;
static bool                  gNowOk    = false;
 
// Display buffer
static const int IMAGE_BYTES = ((EPD_WIDTH + 7) / 8) * EPD_HEIGHT;
static unsigned char image[IMAGE_BYTES];
static Paint paint(image, EPD_WIDTH, EPD_HEIGHT);
static Epd   epd;

struct MenuSongStep {
    uint16_t melodyHz;   // speaker 1
    uint16_t bassHz;     // speaker 2
    uint16_t durMs;
};

// 16-slice loop based on the provided measures 9-12 table.
// For stacked middle notes (e.g. E4 + G#4), this uses the first note listed.
static const MenuSongStep kMenuSong[] = {
    {246, 0, 260},  // 1
    {277, 0, 260},  // 2 (melody hold, bass hold)
    {277, 138, 260},  // 3 (melody rest)
};
static const size_t kMenuSongLen = sizeof(kMenuSong) / sizeof(kMenuSong[0]);
static size_t gSongStep = 0;
static uint32_t gSongNextMs = 0;
static bool gSongWasInMenu = false;
static bool gMusicPaused = false;
static bool gMusicBtnLastDown = false;
static uint32_t gMusicBtnLastEdgeMs = 0;
static TaskHandle_t gMenuSongTaskHandle = nullptr;

static void setVoiceFreq(uint8_t pin, uint16_t hz) {
    if (hz == 0) {
        ledcWriteTone(pin, 0);
        ledcWrite(pin, 0);
        return;
    }
    ledcWriteTone(pin, hz);
}

static void stopMenuSong() {
    setVoiceFreq(SPEAKER_VOICE_1_PIN, 0);
    setVoiceFreq(SPEAKER_VOICE_2_PIN, 0);
}

static void updateMenuSong() {
    if (gMusicPaused) {
        stopMenuSong();
        return;
    }

    const bool inMenu = (gScreen == SCREEN_MAIN_MENU);
    if (!inMenu) {
        if (gSongWasInMenu) {
            stopMenuSong();
            gSongWasInMenu = false;
        }
        return;
    }

    if (!gSongWasInMenu) {
        gSongStep = 0;
        gSongNextMs = 0;
        gSongWasInMenu = true;
    }

    const int32_t dt = (int32_t)(millis() - gSongNextMs);
    if (gSongNextMs == 0 || dt >= 0) {
        const MenuSongStep &s = kMenuSong[gSongStep];
        setVoiceFreq(SPEAKER_VOICE_1_PIN, s.melodyHz);
        setVoiceFreq(SPEAKER_VOICE_2_PIN, s.bassHz);
        gSongNextMs = millis() + s.durMs;
        gSongStep = (gSongStep + 1) % kMenuSongLen;
    }
}

static void handleMusicToggleButton() {
    const bool down = (digitalRead(PIN_MUSIC_TOGGLE) == LOW);
    const uint32_t now = millis();

    if (down != gMusicBtnLastDown && (now - gMusicBtnLastEdgeMs) > 35) {
        gMusicBtnLastEdgeMs = now;
        gMusicBtnLastDown = down;
        if (down) {
            gMusicPaused = !gMusicPaused;
            if (!gMusicPaused) {
                gSongNextMs = 0;
                gSongWasInMenu = false;
            } else {
                stopMenuSong();
            }
            Serial.printf("Music %s (GPIO7)\n", gMusicPaused ? "paused" : "playing");
        }
    }
}

static void menuSongTask(void *arg) {
    (void)arg;
    for (;;) {
        updateMenuSong();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// joystick interrupts
void IRAM_ATTR isrUp()     { gPendingDir = DIR_UP;     }
void IRAM_ATTR isrCenter() { gPendingDir = DIR_CENTER;  }
void IRAM_ATTR isrLeft()   { gPendingDir = DIR_LEFT;    }
void IRAM_ATTR isrDown()   { gPendingDir = DIR_DOWN;    }
void IRAM_ATTR isrRight()  { gPendingDir = DIR_RIGHT;   }
void IRAM_ATTR isrClear()  { gClearPending = true;      }
 
 
// draw text twice offset by 1 pixel to fake bold (this is because the font library has no bold)
static void drawBold(Paint &p, int x, int y, const char *s, sFONT *font, int color) {
    p.DrawStringAt(x,     y, s, font, color);
    p.DrawStringAt(x + 1, y, s, font, color);
}
 
// Convert a one-hot beam mask to a point value; returns 0 for invalid masks
static uint32_t maskToPoints(uint8_t mask) {
    uint8_t m = mask & 0x7F;
    for (int i = 0; i < 7; i++) {
        if (m & (1u << i)) return BEAM_POINTS[i];
    }
    return 0;
}
 
// Returns the pin number for a given direction (used for debounce confirmation)
static int pinForDirection(Direction dir) {
    switch (dir) {
        case DIR_UP:     return PIN_UP;
        case DIR_CENTER: return PIN_CENTER;
        case DIR_LEFT:   return PIN_LEFT;
        case DIR_DOWN:   return PIN_DOWN;
        case DIR_RIGHT:  return PIN_RIGHT;
        default:         return -1;
    }
}
 
// Send a control byte over ESP-NOW, with logging
static void espNowSend(uint8_t byte) {
    if (gNowSerial && gNowOk) {
        gNowSerial->write(byte);
        Serial.printf("ESP-NOW TX: 0x%02X\n", byte);
    } else {
        Serial.printf("ESP-NOW TX skipped (link not ready): 0x%02X\n", byte);
    }
}
 
// ---------------------------------------------------------------------------
// Keyboard layout for name entry
// ---------------------------------------------------------------------------
static const char *KB_ROWS[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM<"
};
 
static int kbRowLen(int row) {
    if (row < 0 || row > 3) return 0;
    return (int)strlen(KB_ROWS[row]);
}
 
static bool kbOkVisible() {
    return gNameLen >= 3;
}
 
// Clamp gKbCol to valid range for the current gKbRow
static void kbClampCol() {
    if (gKbRow == 4) { gKbCol = 0; return; }
    int maxCol = kbRowLen(gKbRow) - 1;
    if (gKbCol > maxCol) gKbCol = maxCol;
    if (gKbCol < 0)      gKbCol = 0;
}
 
static void setupEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.setChannel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    for (int i = 0; i < 100 && !WiFi.STA.started(); i++) delay(50);
 
    gNowSerial = new ESP_NOW_Serial_Class(PEER_MAC, ESPNOW_CHANNEL, WIFI_IF_STA);
    gNowOk     = gNowSerial->begin(115200);
    Serial.printf("ESP-NOW ch %d: %s\n", ESPNOW_CHANNEL, gNowOk ? "OK" : "FAIL");
}
 
static bool pollEspNow() {
    if (!gNowSerial || !gNowOk)    return false;
    if (gScreen != SCREEN_GAME)    return false;
    if (gThrowIdx >= THROWS_TOTAL) return false;
 
    bool changed = false;
 
    while (gNowSerial->available() > 0 && gThrowIdx < THROWS_TOTAL) {
        int c = gNowSerial->read();
        if (c < 0) break;
 
        uint8_t  mask = (uint8_t)c;
        uint32_t pts  = maskToPoints(mask);
        bool     p1   = (gThrowIdx % 2) == 0;
 
        if (p1) gScoreP1 += pts;
        else    gScoreP2 += pts;
 
        gThrowIdx++;
        changed = true;
 
        Serial.printf("Throw %d mask=0x%02X +%lu (%s)\n",
                      gThrowIdx, mask, (unsigned long)pts, p1 ? "P1" : "P2");
 
        if (gThrowIdx >= THROWS_TOTAL) {
            // Notify the beam board the game is over
            espNowSend(GAME_END_BYTE);
 
            uint32_t total = gScoreP1 + gScoreP2;
            if (total == 0) {
                gScreen = SCREEN_MAIN_MENU;
                Serial.println("Round over (0 pts) -> menu");
            } else {
                if (gScoreP1 > gScoreP2) {
                    gOutcome  = OUTCOME_P1_WINS;
                    gRunScore = gScoreP1;
                } else if (gScoreP2 > gScoreP1) {
                    gOutcome  = OUTCOME_P2_WINS;
                    gRunScore = gScoreP2;
                } else {
                    gOutcome  = OUTCOME_TIE;
                    gRunScore = gScoreP1;
                }
                gScreen = SCREEN_GAME_OVER;
                Serial.printf("Game over: P1=%lu P2=%lu winner=%lu\n",
                              (unsigned long)gScoreP1,
                              (unsigned long)gScoreP2,
                              (unsigned long)gRunScore);
            }
            break;
        }
    }
    return changed;
}
 
// ---------------------------------------------------------------------------
// Draw functions - one per screen
// ---------------------------------------------------------------------------
 
static void drawMainMenu(Paint &p) {
    p.Clear(UNCOLORED);
    p.SetRotate(ROTATE_270);
 
    p.DrawFilledCircle(44, 37, 21, COLORED);
    p.DrawStringAt(60, 5, "SKEEBALL", &Font24, COLORED);
 
    if (gMenuSel == 0) {
        drawBold(p, 60, 30, "> Start Game",   &Font12, COLORED);
        p.DrawStringAt(60,   45, "  Leaderboard", &Font12, COLORED);
    } else {
        p.DrawStringAt(60,   30, "  Start Game",  &Font12, COLORED);
        drawBold(p, 60, 45, "> Leaderboard", &Font12, COLORED);
    }
    p.SetRotate(ROTATE_0);
}
 
static void drawLeaderboard(Paint &p) {
    p.Clear(UNCOLORED);
    p.SetRotate(ROTATE_270);
    p.DrawStringAt(55, 5, "LEADERBOARD", &Font16, COLORED);
 
    char row[28];
    for (int i = 1; i <= 3; i++) {
        leaderboardFormatRow(i, row, sizeof(row));
        p.DrawStringAt(8, 14 + i * 16, row, &Font12, COLORED);
    }
    p.DrawStringAt(12, 100, "< Menu", &Font12, COLORED);
    p.SetRotate(ROTATE_0);
}
 
static void drawGame(Paint &p) {
    p.Clear(UNCOLORED);
    p.SetRotate(ROTATE_270);
    p.DrawStringAt(50, 4, "PLAY", &Font16, COLORED);
 
    char buf[40];
    snprintf(buf, sizeof(buf), "P1: %lu", (unsigned long)gScoreP1);
    p.DrawStringAt(8, 22, buf, &Font12, COLORED);
    snprintf(buf, sizeof(buf), "P2: %lu", (unsigned long)gScoreP2);
    p.DrawStringAt(8, 36, buf, &Font12, COLORED);
 
    if (gThrowIdx < THROWS_TOTAL) {
        bool p1Turn = (gThrowIdx % 2) == 0;
        snprintf(buf, sizeof(buf), "Turn: %s", p1Turn ? "P1" : "P2");
        p.DrawStringAt(8, 52, buf, &Font12, COLORED);
        snprintf(buf, sizeof(buf), "Throw %d/%d", gThrowIdx + 1, THROWS_TOTAL);
        p.DrawStringAt(8, 66, buf, &Font12, COLORED);
    } else {
        p.DrawStringAt(8, 52, "Done", &Font12, COLORED);
    }
 
    p.DrawStringAt(8, 82, gNowOk ? "NOW OK" : "NOW --", &Font8, COLORED);
    p.SetRotate(ROTATE_0);
}
 
static void drawGameOver(Paint &p) {
    p.Clear(UNCOLORED);
    p.SetRotate(ROTATE_270);
    p.DrawStringAt(48, 2, "GAME OVER", &Font12, COLORED);
 
    const char *headline = "TIE!";
    if (gOutcome == OUTCOME_P1_WINS) headline = "P1 WINS";
    if (gOutcome == OUTCOME_P2_WINS) headline = "P2 WINS";
    drawBold(p, 42, 16, headline, &Font16, COLORED);
 
    char buf[40];
    snprintf(buf, sizeof(buf), "P1: %lu", (unsigned long)gScoreP1);
    p.DrawStringAt(8, 38, buf, &Font12, COLORED);
    snprintf(buf, sizeof(buf), "P2: %lu", (unsigned long)gScoreP2);
    p.DrawStringAt(8, 52, buf, &Font12, COLORED);
 
    p.DrawStringAt(4, 88,  "CENTER: tag score",  &Font8, COLORED);
    p.DrawStringAt(4, 100, "LEFT: back to menu", &Font8, COLORED);
    p.SetRotate(ROTATE_0);
}
 
static void drawNameEntry(Paint &p) {
    p.Clear(UNCOLORED);
    p.SetRotate(ROTATE_270);
    p.DrawStringAt(32, 2, "WINNER TAG", &Font12, COLORED);
 
    // Show entered initials (underscores for blanks)
    char preview[8];
    snprintf(preview, sizeof(preview), "%c %c %c",
             gNameLen > 0 ? gInitials[0] : '_',
             gNameLen > 1 ? gInitials[1] : '_',
             gNameLen > 2 ? gInitials[2] : '_');
    p.DrawStringAt(32, 16, preview, &Font20, COLORED);
 
    // Keyboard grid
    const int BASE_X = 2, BASE_Y = 38, CELL_W = 12, ROW_H = 16;
    const int KEY_W = 11, KEY_H = 16;
 
    for (int r = 0; r < 4; r++) {
        const char *row = KB_ROWS[r];
        int len = (int)strlen(row);
        for (int c = 0; c < len; c++) {
            int px = BASE_X + c * CELL_W;
            int py = BASE_Y + r * ROW_H;
            char key[2] = {row[c], '\0'};
            bool selected = (r == gKbRow && c == gKbCol && gKbRow < 4);
            if (selected) {
                p.DrawFilledRectangle(px - 1, py - 1, px + KEY_W, py + KEY_H, COLORED);
                p.DrawStringAt(px, py, key, &Font16, UNCOLORED);
            } else {
                p.DrawStringAt(px, py, key, &Font16, COLORED);
            }
        }
    }
 
    // OK button (only shown once 3 initials are entered)
    if (kbOkVisible()) {
        int okY = BASE_Y + 4 * ROW_H + 2;
        if (gKbRow == 4) drawBold(p, 50, okY, "> OK", &Font16, COLORED);
        else             p.DrawStringAt(50, okY, "  OK", &Font16, COLORED);
    }
 
    p.SetRotate(ROTATE_0);
}
 
static void drawCurrentScreen() {
    switch (gScreen) {
        case SCREEN_MAIN_MENU:   drawMainMenu(paint);    break;
        case SCREEN_LEADERBOARD: drawLeaderboard(paint); break;
        case SCREEN_GAME:        drawGame(paint);         break;
        case SCREEN_GAME_OVER:   drawGameOver(paint);     break;
        case SCREEN_NAME_ENTRY:  drawNameEntry(paint);    break;
    }
}
 
// ---------------------------------------------------------------------------
// Input handling - one function per screen
// ---------------------------------------------------------------------------
 
static void handleMainMenu(Direction dir) {
    if (dir == DIR_UP   && gMenuSel > 0) gMenuSel--;
    if (dir == DIR_DOWN && gMenuSel < 1) gMenuSel++;
    if (dir == DIR_CENTER) {
        if (gMenuSel == 1) {
            gScreen = SCREEN_LEADERBOARD;
        } else {
            espNowSend(GAME_START_BYTE);
            gScreen   = SCREEN_GAME;
            gScoreP1  = 0;
            gScoreP2  = 0;
            gRunScore = 0;
            gThrowIdx = 0;
        }
    }
}
 
static void handleLeaderboard(Direction dir) {
    if (dir == DIR_LEFT) gScreen = SCREEN_MAIN_MENU;
}
 
static void handleGameOver(Direction dir) {
    if (dir == DIR_CENTER) {
        gScreen  = SCREEN_NAME_ENTRY;
        gNameLen = 0;
        gInitials[0] = gInitials[1] = gInitials[2] = gInitials[3] = '\0';
        gKbRow = 0;
        gKbCol = 0;
        Serial.println("Going to name entry.");
    } else if (dir == DIR_LEFT) {
        gScreen   = SCREEN_MAIN_MENU;
        gRunScore = 0;
        Serial.println("Skipped name entry -> menu.");
    }
}
 
static void handleNameEntry(Direction dir) {
    // OK row
    if (gKbRow == 4 && kbOkVisible()) {
        if (dir == DIR_CENTER && gNameLen == 3 && gRunScore > 0) {
            leaderboardSubmitEntry(gRunScore, gInitials);
            gScreen   = SCREEN_MAIN_MENU;
            gRunScore = 0;
            Serial.println("Score saved -> menu.");
        } else if (dir == DIR_UP) {
            gKbRow = 3;
            kbClampCol();
        }
        return;
    }
 
    // Regular key rows
    if (dir == DIR_LEFT  && gKbCol > 0)                  gKbCol--;
    if (dir == DIR_RIGHT && gKbCol < kbRowLen(gKbRow)-1) gKbCol++;
 
    if (dir == DIR_UP && gKbRow > 0) {
        gKbRow--;
        kbClampCol();
    }
    if (dir == DIR_DOWN) {
        if (gKbRow < 3) { gKbRow++; kbClampCol(); }
        else if (gKbRow == 3 && kbOkVisible()) { gKbRow = 4; gKbCol = 0; }
    }
 
    if (dir == DIR_CENTER && gKbRow < 4) {
        const char *row = KB_ROWS[gKbRow];
        if (gKbCol < (int)strlen(row)) {
            char ch = row[gKbCol];
            if (ch == '<') {
                // Backspace
                if (gNameLen > 0) { gNameLen--; gInitials[gNameLen] = '\0'; }
            } else if (gNameLen < 3) {
                gInitials[gNameLen] = ch;
                gNameLen++;
                gInitials[gNameLen] = '\0';
                // Auto-advance to OK once all 3 initials entered
                if (gNameLen == 3) { gKbRow = 4; gKbCol = 0; }
            }
        }
    }
 
    // If OK row is no longer valid (e.g. after backspace), bounce back up
    if (gKbRow == 4 && !kbOkVisible()) {
        gKbRow = 3;
        kbClampCol();
    }
}
 
void setup() {
    Serial.begin(115200);
    delay(500);
 
    epd.Init(FAST);
    gScreen  = SCREEN_MAIN_MENU;
    gMenuSel = 0;
 
    leaderboardInit();
    drawMainMenu(paint);
    epd.Display_Fast(image);
 
    // Joystick: active LOW
    int joyPins[] = {PIN_UP, PIN_CENTER, PIN_LEFT, PIN_DOWN, PIN_RIGHT};
    void (*joyIsrs[])() = {isrUp, isrCenter, isrLeft, isrDown, isrRight};
    for (int i = 0; i < 5; i++) {
        pinMode(joyPins[i], INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(joyPins[i]), joyIsrs[i], FALLING);
    }
 
    // Clear button: active HIGH
    pinMode(PIN_CLEAR_EPD, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_CLEAR_EPD), isrClear, RISING);
    pinMode(PIN_MUSIC_TOGGLE, INPUT_PULLUP);
 
    setupEspNow();

    if (!ledcAttach(SPEAKER_VOICE_1_PIN, SPEAKER_PWM_INIT_FREQ_HZ, SPEAKER_PWM_RES_BITS)) {
        Serial.println("ledcAttach voice1 failed");
    }
    if (!ledcAttach(SPEAKER_VOICE_2_PIN, SPEAKER_PWM_INIT_FREQ_HZ, SPEAKER_PWM_RES_BITS)) {
        Serial.println("ledcAttach voice2 failed");
    }
    stopMenuSong();
    if (xTaskCreate(menuSongTask, "menuSong", 4096, nullptr, 1, &gMenuSongTaskHandle) != pdPASS) {
        Serial.println("menuSong task create failed");
    }
    Serial.println("Ready.");
}
 
void loop() {
    handleMusicToggleButton();

    bool clearReq = false;
    if (gClearPending) { gClearPending = false; clearReq = true; }
 
    if (clearReq) {
        delay(40);
        if (digitalRead(PIN_CLEAR_EPD) == HIGH) {
            paint.Clear(UNCOLORED);
            epd.Display_Fast(image);
            Serial.println("Display cleared.");
        }
    }
 
    // --- Poll ESP-NOW for new throws ---
    AppScreen prevScreen = gScreen;
    uint32_t  prevP1     = gScoreP1;
    uint32_t  prevP2     = gScoreP2;
    int       prevThrow  = gThrowIdx;
    bool      espNowDirty = pollEspNow();
 
    // --- Read joystick direction ---
    Direction dir = DIR_NONE;
    if (gPendingDir != DIR_NONE) { dir = gPendingDir; gPendingDir = DIR_NONE; }
 
    // Nothing to do this iteration
    if (dir == DIR_NONE && !espNowDirty) { delay(5); return; }
 
    // Debounce: confirm pin is still pressed
    if (dir != DIR_NONE) {
        delay(40);
        if (digitalRead(pinForDirection(dir)) != LOW) { delay(5); return; }
 
        // Rate-limit navigation so display has time to update
        uint32_t now = millis();
        if (gLastNavMs != 0 && (now - gLastNavMs) < NAV_MIN_INTERVAL_MS) {
            gPendingDir = dir;
            delay(5);
            return;
        }
    }
 
    // Snapshot previous UI state for dirty check
    int prevMenuSel = gMenuSel;
    int prevKbRow   = gKbRow;
    int prevKbCol   = gKbCol;
    int prevNameLen = gNameLen;
 
    // --- Dispatch input to the active screen ---
    if (dir != DIR_NONE) {
        switch (gScreen) {
            case SCREEN_MAIN_MENU:   handleMainMenu(dir);    break;
            case SCREEN_LEADERBOARD: handleLeaderboard(dir); break;
            case SCREEN_GAME:        /* no input during play */ break;
            case SCREEN_GAME_OVER:   handleGameOver(dir);    break;
            case SCREEN_NAME_ENTRY:  handleNameEntry(dir);   break;
        }
    }
 
    // --- Redraw only if something changed ---
    bool dirty = espNowDirty
              || (gScreen   != prevScreen)
              || (gMenuSel  != prevMenuSel)
              || (gScoreP1  != prevP1 || gScoreP2 != prevP2 || gThrowIdx != prevThrow)
              || (gKbRow    != prevKbRow || gKbCol != prevKbCol || gNameLen != prevNameLen);
 
    if (dirty) {
        drawCurrentScreen();
        epd.Display_Fast(image);
    }
 
    if (dir != DIR_NONE) gLastNavMs = millis();
 
    delay(5);
}