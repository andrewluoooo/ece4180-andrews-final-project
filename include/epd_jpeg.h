#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Decode a baseline JPEG from SPIFFS into a packed 1bpp EPD buffer (MSB = left pixel).
 * Buffer is filled with 0xFF first. Pixels use the same convention as Paint with
 * IF_INVERT_COLOR=1: cleared bit = black ink, set bit = white paper.
 *
 * @param path       SPIFFS path, e.g. "/picture.jpg"
 * @param epdBuf     framebuffer (e.g. image[] from main)
 * @param epdBufBytes sizeof framebuffer
 * @param stridePx   row stride in pixels (Paint rounds width to multiple of 8; use paint.GetWidth())
 * @param epdW       visible width (EPD_WIDTH)
 * @param epdH       visible height (EPD_HEIGHT)
 * @param outErr     optional error code: 0 ok, -2 SPIFFS, -3 file, -4 size, -5 malloc, -6 read, -7 decode
 * @return true on success
 *
 * Upload files: put JPEG under data/ then run: pio run -t uploadfs
 */
bool epdDrawJpegFromSpiffs(const char *path, uint8_t *epdBuf, size_t epdBufBytes, int stridePx,
                           int epdW, int epdH, int *outErr);
