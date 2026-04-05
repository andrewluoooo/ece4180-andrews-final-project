#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("ESP32-C6 — PlatformIO ready");
}

void loop() {
  static uint32_t n = 0;
  Serial.printf("uptime: %lu s\n", n++);
  delay(1000);
}
