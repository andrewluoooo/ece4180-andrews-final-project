#include <Arduino.h>
#include <Goldelox_Serial_4DLib.h>
#include <Goldelox_Const4D.h>

// uLCD-144G2 (Goldelox SPE / serial): host UART on GPIO17 (RX) / GPIO16 (TX).
// Wire: ESP32 TX (16) -> display RX, ESP32 RX (17) -> display TX, common GND.
// Display must be programmed for Serial (SPE) in Workshop4; default SPE baud is often 9600.
static constexpr unsigned long kDisplayBaud = 9600;

Goldelox_Serial_4DLib Display(&Serial0);

static void onDisplayError(int errCode, unsigned char errByte) {
  Serial.printf("uLCD error: code=%d byte=0x%02x\n", errCode, errByte);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial0.begin(kDisplayBaud, SERIAL_8N1, 17, 16);
  Display.Callback4D = onDisplayError;
  Display.TimeLimit4D = 5000;
  delay(800);

  Display.gfx_ScreenMode(LANDSCAPE);
  Display.SSTimeout(0);
  Display.SSSpeed(0);
  Display.SSMode(0);

  Display.gfx_Cls();
  Display.txt_FGcolour(WHITE);
  Display.txt_BGcolour(BLACK);
  Display.txt_FontID(0);
  Display.txt_MoveCursor(1, 1);
  Display.print("Hello World!");

  Serial.println("Sent \"Hello World!\" to uLCD-144G2");
}

void loop() {}
