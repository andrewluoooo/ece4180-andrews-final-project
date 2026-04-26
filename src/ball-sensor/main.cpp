#include <Arduino.h>
#include "ESP32_NOW_Serial.h"
#include "MacAddress.h"
#include "WiFi.h"
#include "esp_wifi.h"
 
// ---------------------------------------------------------------------------
// Beam-break table
// ---------------------------------------------------------------------------
struct IRBeamBreak {
  uint8_t inputPin;
  uint8_t outputPin;
  const char *name;
};
 
#define NUM_BEAMS 7
 
IRBeamBreak beams[NUM_BEAMS] = {
  { 0, 10, "Beam 1" },
  { 1, 11, "Beam 2" },
  { 2, 15, "Beam 3" },
  { 3, 18, "Beam 4" },
  { 4, 19, "Beam 5" },
  { 6, 20, "Beam 6" },
  { 5, 21, "Beam 7" },
};
 
// ---------------------------------------------------------------------------
// Detection parameters
// ---------------------------------------------------------------------------
#define VOLTAGE_THRESHOLD 2.0f
#define THRESHOLD_COUNTS  ((uint16_t)(VOLTAGE_THRESHOLD * 4095.0f / 3.3f))
#define BALL_COOLDOWN_MS  100
#define DETECT_PERIOD_US  1000
 
// ---------------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------------
#define BUZZER_PIN 7
 
// ---------------------------------------------------------------------------
// Tone sequencer
// ---------------------------------------------------------------------------
struct Note {
  uint16_t freqHz;      // 0 = silent gap
  uint16_t durationMs;
};
 
// Play a tone sequence (blocking). freqHz == 0 is a silent gap.
static void playSfx(const Note *seq) {
  for (int i = 0; seq[i].freqHz != 0 || seq[i].durationMs != 0; i++) {
    if (seq[i].freqHz == 0) {
      noTone(BUZZER_PIN);
      delay(seq[i].durationMs);
    } else {
      tone(BUZZER_PIN, seq[i].freqHz, seq[i].durationMs);
      delay(seq[i].durationMs + 12);
      noTone(BUZZER_PIN);
    }
  }
}
 
// ---------------------------------------------------------------------------
// Beam sound effects
//
//   Beam 1 (10 pts)  — single low blip
//   Beam 2 (20 pts)  — two-tone acknowledgement
//   Beam 3 (30 pts)  — three-note rising clip
//   Beam 4 (40 pts)  — cheerful four-step climb
//   Beam 5 (50 pts)  — brighter five-note fanfare
//   Beams 6&7 (100)  — jackpot: rapid ascent + triumphant double-hit
// ---------------------------------------------------------------------------
static const Note SFX_BEAM1[] = {
  { 400, 90 },
  { 0,   0  }
};
 
static const Note SFX_BEAM2[] = {
  { 500, 70 },
  { 750, 90 },
  { 0,   0  }
};
 
static const Note SFX_BEAM3[] = {
  { 600,  55 },
  { 900,  55 },
  { 1200, 90 },
  { 0,    0  }
};
 
static const Note SFX_BEAM4[] = {
  { 700,  50 },
  { 950,  50 },
  { 1250, 50 },
  { 1550, 110 },
  { 0,    0   }
};
 
static const Note SFX_BEAM5[] = {
  { 800,  45 },
  { 1000, 45 },
  { 1300, 45 },
  { 1600, 45 },
  { 2000, 130 },
  { 0,    0   }
};
 
static const Note SFX_BEAM67[] = {
  { 800,  35 },
  { 1000, 35 },
  { 1300, 35 },
  { 1600, 35 },
  { 2000, 35 },
  { 2500, 35 },
  { 2400, 160 },
  { 0,    40  },
  { 2400, 220 },
  { 0,    0   }
};
 
static const Note *beamSfx[NUM_BEAMS] = {
  SFX_BEAM1,
  SFX_BEAM2,
  SFX_BEAM3,
  SFX_BEAM4,
  SFX_BEAM5,
  SFX_BEAM67,
  SFX_BEAM67,
};
 
// ---------------------------------------------------------------------------
// Game start / end sound effects
//
//   Start — upbeat ascending fanfare, "get ready to play"
//   End   — descending wind-down, "time's up"
// ---------------------------------------------------------------------------
static const Note SFX_GAME_START[] = {
  { 600,  80 },
  { 800,  80 },
  { 1000, 80 },
  { 1300, 80 },
  { 1600, 80 },
  { 2000, 180 },
  { 0,    0   }
};
 
static const Note SFX_GAME_END[] = {
  { 1400, 100 },
  { 1100, 100 },
  { 800,  100 },
  { 600,  100 },
  { 400,  250 },
  { 0,    0   }
};
 
// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------
#define GAME_START_BYTE 0xAA
#define GAME_END_BYTE   0xBB
 
bool gameStarted = false;
 
static void startGame() {
  gameStarted = true;
  Serial.println("Game START (0xAA rx)");
  playSfx(SFX_GAME_START);
}
 
static void endGame() {
  gameStarted = false;
  Serial.println("Game END (0xBB rx) -> beam detections disabled until next 0xAA");
  playSfx(SFX_GAME_END);
}
 
// ---------------------------------------------------------------------------
// ESP-NOW
// ---------------------------------------------------------------------------
#define ESPNOW_WIFI_CHANNEL 1
 
const MacAddress peer_mac({0x10, 0x51, 0xDB, 0x0D, 0xF2, 0xBC});
ESP_NOW_Serial_Class NowSerial(peer_mac, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA);
 
// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
uint16_t latestSample[NUM_BEAMS]        = { 0 };
bool     ballCurrentlyInBeam[NUM_BEAMS] = { false };
uint32_t ballCooldownTimer[NUM_BEAMS]   = { 0 };
uint8_t  pendingDetectionMask           = 0;
 
hw_timer_t *detectTimer = NULL;
 
// ---------------------------------------------------------------------------
// Detection timer ISR
// ---------------------------------------------------------------------------
void IRAM_ATTR onDetectTimerISR() {
  uint32_t nowMs = millis();
 
  for (int i = 0; i < NUM_BEAMS; i++) {
    bool isBlocking = (latestSample[i] < THRESHOLD_COUNTS);
 
    if (isBlocking && !ballCurrentlyInBeam[i]) {
      if (nowMs - ballCooldownTimer[i] > BALL_COOLDOWN_MS) {
        ballCurrentlyInBeam[i] = true;
        ballCooldownTimer[i]   = nowMs;
        pendingDetectionMask  |= (1 << i);
      }
    }
 
    if (!isBlocking && ballCurrentlyInBeam[i]) {
      ballCurrentlyInBeam[i] = false;
    }
  }
}
 
// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.printf("Initialized with %d IR beam breaks\n", NUM_BEAMS);
 
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
 
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
 
  for (int i = 0; i < NUM_BEAMS; i++) {
    pinMode(beams[i].outputPin, OUTPUT);
    digitalWrite(beams[i].outputPin, HIGH);
    analogRead(beams[i].inputPin);
    Serial.printf("  %s -> input=GPIO%d, output=GPIO%d\n",
                  beams[i].name, beams[i].inputPin, beams[i].outputPin);
  }
 
  detectTimer = timerBegin(1000000);
  timerAttachInterrupt(detectTimer, &onDetectTimerISR);
  timerAlarm(detectTimer, DETECT_PERIOD_US, true, 0);
 
  WiFi.mode(WIFI_STA);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  while (!WiFi.STA.started()) delay(100);
 
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
 
  NowSerial.begin(115200);
  Serial.println(NowSerial ? "ESP-NOW Connected" : "ESP-NOW FAILED");
 
  Serial.printf("Waiting for ESP-NOW start byte (0x%02X)...\n", GAME_START_BYTE);
}
 
// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------
void loop() {
  // 0. Drain incoming ESP-NOW bytes.
  while (NowSerial.available() > 0) {
    int rx = NowSerial.read();
    if (rx < 0) break;
    if ((uint8_t)rx == GAME_START_BYTE) {
      startGame();
    } else if ((uint8_t)rx == GAME_END_BYTE) {
      endGame();
    } else {
      Serial.printf("ESP-NOW rx: 0x%02X (ignored)\n", (uint8_t)rx);
    }
  }
 
  // 1. Sample every ADC channel
  for (int i = 0; i < NUM_BEAMS; i++) {
    latestSample[i] = analogRead(beams[i].inputPin);
  }
 
  // 2. Snapshot and clear the mask
  timerStop(detectTimer);
  uint8_t firedMask = pendingDetectionMask;
  pendingDetectionMask = 0;
  timerStart(detectTimer);
 
  // Ignore detections when no game is running
  if (firedMask == 0 || !gameStarted) return;
 
  // 3. Log, play sound, and send — once per fired beam
  for (int i = 0; i < NUM_BEAMS; i++) {
    if (!(firedMask & (1 << i))) continue;
 
    float v = latestSample[i] * (3.3f / 4095.0f);
    Serial.printf("Ball detected on %s | Voltage: %.2f V | mask=%d\n",
                  beams[i].name, v, firedMask);
 
    playSfx(beamSfx[i]);
 
    NowSerial.write((uint8_t)(1 << i));
  }
}