#ifndef EPDIF_H
#define EPDIF_H

#include <Arduino.h>
#include "epd_compat_pgmspace.h"

// Panel wiring (SPI + control)
#define EPD_SPI_SCK     23
#define EPD_SPI_MOSI    15   // DIN
#define EPD_SPI_MISO    (-1)

#define RST_PIN         20
#define DC_PIN          21
#define CS_PIN          22
#define BUSY_PIN        19

class EpdIf {
public:
    EpdIf(void);
    ~EpdIf(void);

    static int  IfInit(void);
    static void DigitalWrite(int pin, int value); 
    static int  DigitalRead(int pin);
    static void DelayMs(unsigned int delaytime);
    static void SpiTransfer(unsigned char data);
};

#endif
