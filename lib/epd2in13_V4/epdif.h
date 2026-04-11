#ifndef EPDIF_H
#define EPDIF_H

#include <Arduino.h>
#include "epd_compat_pgmspace.h"

// Pin definition
#define RST_PIN         8
#define DC_PIN          9
#define CS_PIN          34
#define BUSY_PIN        7

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
