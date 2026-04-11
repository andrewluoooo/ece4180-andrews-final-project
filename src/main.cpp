#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include "epd2in13_V4.h"
#include "epdpaint.h"
#include "star.h"


// Lafvin / Waveshare 2.13" V4: 250 x 122, 1 bit per pixel (see
// https://lafvin-213inch-epaper-hat.readthedocs.io/en/latest/about_this_kit.html )
#define COLORED     0
#define UNCOLORED   1

// Must match Epd::Display(): ceil(EPD_WIDTH/8) * EPD_HEIGHT bytes (4000 for 122x250).
static constexpr int kImageBytes = ((EPD_WIDTH + 7) / 8) * EPD_HEIGHT;

unsigned char image[kImageBytes];
Paint paint(image, EPD_WIDTH, EPD_HEIGHT);
Epd epd;

void setup() {
  Serial.begin(115200);
  delay(500);

  if (epd.Init(FULL) != 0) {
    Serial.println("EPD init failed (check RST/DC/CS/BUSY wiring vs epdif.h)");
    return;
  }
  epd.Clear();
  // Lafvin doc: 0 = black, 1 = white in image data; this driver maps that via
  // COLORED/UNCOLORED + IF_INVERT_COLOR in epdpaint.h.
  paint.Clear(UNCOLORED);
  paint.DrawStringAt(20, 15, "Smart Display", &Font12, COLORED);
  epd.Display(image);
  Serial.println("Displayed image");

  // // put your setup code here, to run once:
  // Serial.begin(115200);
  // Serial.println("epd FAST");
  // epd.Init(FAST);
  // // epd.Init(FULL);
  // epd.Display_Fast(star);
  // // epd.Display1(star);
  // delay(2000);
}

// start game
// leaderboard
void loop() {
  delay(2000);
}
