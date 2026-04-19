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
#define PILL_COOLDOWN_MS  100
#define DETECT_PERIOD_US  1000
 
// ---------------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------------
#define BUZZER_PIN       7
#define BEEP_FREQ_HZ     1700
#define BEEP_DURATION_MS 80
 
// ---------------------------------------------------------------------------
// Servo (HiTec HS-422)
// ---------------------------------------------------------------------------
// Signal (yellow/orange) -> GPIO 23
// Power  (red)           -> 5 V (USB VBUS), NOT 3V3 -- HS-422 runs 4.8-6 V and
//                           can pull >500 mA at stall, so it MUST have a
//                           common ground with the ESP32 and should ideally
//                           be powered from a separate 5 V supply with a
//                           bulk cap (e.g. 470 uF) to prevent brown-outs.
// Ground (black/brown)   -> GND (shared with ESP32 GND)
//
// 50 Hz PWM, 16-bit duty. Per HiTec, pulse width runs ~900 us (0 deg) to
// ~2100 us (180 deg), with 1500 us at the 90 deg center.
#define SERVO_PIN          23
#define SERVO_PWM_FREQ_HZ  50
#define SERVO_PWM_RES_BITS 16
#define SERVO_MIN_US       900
#define SERVO_MAX_US       2100
#define SERVO_OPEN_DEG     180     // game-start position
#define SERVO_CLOSE_DEG    0       // locked/close position
#define SERVO_OPEN_MS      10000   // keep open for 10 s after game start

// Game-start byte sent over ESP-NOW (raw, not ASCII "AA")
#define GAME_START_BYTE    0xAA

uint32_t gameStartMs = 0;
bool     gameStarted = false;
bool     servoClosed = true;       // start closed until a 0xAA arrives

static inline uint32_t servoAngleToDuty(uint16_t angleDeg) {
  if (angleDeg > 180) angleDeg = 180;
  uint32_t pulseUs =
      SERVO_MIN_US + (uint32_t)angleDeg * (SERVO_MAX_US - SERVO_MIN_US) / 180;
  uint32_t periodUs = 1000000UL / SERVO_PWM_FREQ_HZ;       // 20000 us
  uint32_t maxDuty  = (1UL << SERVO_PWM_RES_BITS) - 1;     // 65535
  return (uint32_t)((uint64_t)pulseUs * maxDuty / periodUs);
}

static inline void writeServoAngle(uint16_t angleDeg) {
  ledcWrite(SERVO_PIN, servoAngleToDuty(angleDeg));
}

// Called when a GAME_START_BYTE (0xAA) is received over ESP-NOW. Opens the
// servo and (re)arms the 10 s close-timeout. Safe to call repeatedly.
static void startGame() {
  writeServoAngle(SERVO_OPEN_DEG);
  gameStartMs = millis();
  gameStarted = true;
  servoClosed = false;
  Serial.printf("Game START (0xAA rx) -> servo OPEN (%d deg), closing in %lu ms\n",
                SERVO_OPEN_DEG, (unsigned long)SERVO_OPEN_MS);
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
bool     pillCurrentlyInBeam[NUM_BEAMS] = { false };
uint32_t pillCooldownTimer[NUM_BEAMS]   = { 0 };
uint8_t  pendingDetectionMask           = 0;
 
hw_timer_t *detectTimer = NULL;
 
// ---------------------------------------------------------------------------
// Detection timer ISR
// ---------------------------------------------------------------------------
void IRAM_ATTR onDetectTimerISR() {
  uint32_t nowMs = millis();
 
  for (int i = 0; i < NUM_BEAMS; i++) {
    bool isBlocking = (latestSample[i] < THRESHOLD_COUNTS);
 
    if (isBlocking && !pillCurrentlyInBeam[i]) {
      if (nowMs - pillCooldownTimer[i] > PILL_COOLDOWN_MS) {
        pillCurrentlyInBeam[i] = true;
        pillCooldownTimer[i]   = nowMs;
        pendingDetectionMask  |= (1 << i);
      }
    }
 
    if (!isBlocking && pillCurrentlyInBeam[i]) {
      pillCurrentlyInBeam[i] = false;
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

  if (!ledcAttach(SERVO_PIN, SERVO_PWM_FREQ_HZ, SERVO_PWM_RES_BITS)) {
    Serial.println("ledcAttach(SERVO_PIN) FAILED");
  }
  writeServoAngle(SERVO_CLOSE_DEG);
  gameStarted = false;
  servoClosed = true;
  Serial.printf("Waiting for ESP-NOW start byte (0x%02X)...\n", GAME_START_BYTE);
}
 
// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------
void loop() {
  // 0a. Drain incoming ESP-NOW bytes; a 0xAA starts (or restarts) the game.
  while (NowSerial.available() > 0) {
    int rx = NowSerial.read();
    if (rx < 0) break;
    if ((uint8_t)rx == GAME_START_BYTE) {
      startGame();
    } else {
      Serial.printf("ESP-NOW rx: 0x%02X (ignored)\n", (uint8_t)rx);
    }
  }

  // 0b. Servo close-after-timeout (non-blocking, only after a game has started)
  if (gameStarted && !servoClosed &&
      (millis() - gameStartMs >= SERVO_OPEN_MS)) {
    writeServoAngle(SERVO_CLOSE_DEG);
    servoClosed = true;
    gameStarted = false;
    Serial.printf("Timer expired -> servo CLOSED (%d deg)\n", SERVO_CLOSE_DEG);
  }

  // 1. Sample every ADC channel
  for (int i = 0; i < NUM_BEAMS; i++) {
    latestSample[i] = analogRead(beams[i].inputPin);
  }
 
  // 2. Snapshot and clear the mask (stop timer briefly to avoid a race)
  timerStop(detectTimer);
  uint8_t firedMask = pendingDetectionMask;
  pendingDetectionMask = 0;
  timerStart(detectTimer);
 
  if (firedMask == 0) return;
 
  // 3. Log each fired beam
  for (int i = 0; i < NUM_BEAMS; i++) {
    if (firedMask & (1 << i)) {
      float v = latestSample[i] * (3.3f / 4095.0f);
      Serial.printf("Pill detected on %s | Voltage: %.2f V | mask=%d\n",
                    beams[i].name, v, firedMask);
    }
  }
 
  // 4. Beep
  tone(BUZZER_PIN, BEEP_FREQ_HZ, BEEP_DURATION_MS);
 
  // 5. Send mask over ESP-NOW
  NowSerial.write(firedMask);
  // NowSerial.print('\n');
}