#ifndef EPD_COMPAT_PGMSPACE_H
#define EPD_COMPAT_PGMSPACE_H

#if defined(__AVR__)
#include <avr/pgmspace.h>
#else
#include <pgmspace.h>
#endif

#endif
