#include "epd_jpeg.h"
#include "epdpaint.h"

#include <JPEGDEC.h>
#include <SPIFFS.h>
#include <algorithm>
#include <cstring>

struct JpegEpdCtx {
  uint8_t *fb;
  int epdW;
  int epdH;
  int stridePx;
  uint8_t threshold;
};

static int jpegDrawThunk(JPEGDRAW *p) {
  auto *c = static_cast<JpegEpdCtx *>(p->pUser);
  for (int row = 0; row < p->iHeight; ++row) {
    for (int col = 0; col < p->iWidthUsed; ++col) {
      const int x = p->x + col;
      const int y = p->y + row;
      if (x < 0 || x >= c->epdW || y < 0 || y >= c->epdH) {
        continue;
      }
#if PAINT_MIRROR_X_AXIS
      const int bx = c->stridePx - 1 - x;
#else
      const int bx = x;
#endif
      if (bx < 0 || bx >= c->stridePx) {
        continue;
      }
      const uint16_t pix = p->pPixels[row * p->iWidth + col];
      const int r5 = (pix >> 11) & 0x1f;
      const int g6 = (pix >> 5) & 0x3f;
      const int b5 = pix & 0x1f;
      const int r = (r5 << 3) | (r5 >> 2);
      const int g = (g6 << 2) | (g6 >> 4);
      const int b = (b5 << 3) | (b5 >> 2);
      const int lum = (r * 38 + g * 75 + b * 15) >> 7;
      const int idx = (bx + y * c->stridePx) / 8;
      const uint8_t m = static_cast<uint8_t>(0x80 >> (bx & 7));
      if (lum < c->threshold) {
        c->fb[idx] &= static_cast<uint8_t>(~m);
      } else {
        c->fb[idx] |= m;
      }
    }
  }
  return 1;
}

bool epdDrawJpegFromSpiffs(const char *path, uint8_t *epdBuf, size_t epdBufBytes, int stridePx,
                           int epdW, int epdH, int *outErr) {
  int local = 0;
  int *e = outErr ? outErr : &local;
  *e = -1;

  if (!SPIFFS.begin(false) && !SPIFFS.begin(true)) {
    *e = -2;
    return false;
  }

  File f = SPIFFS.open(path, "r", false);
  if (!f) {
    *e = -3;
    return false;
  }

  const size_t sz = f.size();
  if (sz == 0 || sz > 3 * 1024 * 1024) {
    f.close();
    *e = -4;
    return false;
  }

  uint8_t *jpg = static_cast<uint8_t *>(malloc(sz));
  if (!jpg) {
    f.close();
    *e = -5;
    return false;
  }

  const size_t rd = f.read(jpg, sz);
  f.close();
  if (rd != sz) {
    free(jpg);
    *e = -6;
    return false;
  }

  std::memset(epdBuf, 0xFF, epdBufBytes);

  JPEGDEC jpeg;
  JpegEpdCtx ctx{epdBuf, epdW, epdH, stridePx, 140};

  static const int kScales[] = {
      0,
      JPEG_SCALE_HALF,
      JPEG_SCALE_HALF | JPEG_SCALE_QUARTER,
      JPEG_SCALE_HALF | JPEG_SCALE_QUARTER | JPEG_SCALE_EIGHTH,
  };

  bool ok = false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (!jpeg.openRAM(jpg, static_cast<int>(sz), jpegDrawThunk)) {
      continue;
    }
    jpeg.setUserPointer(&ctx);
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);

    const int jw = jpeg.getWidth();
    const int jh = jpeg.getHeight();
    const int cw = std::min(jw, epdW);
    const int ch = std::min(jh, epdH);
    int cx = (jw - cw) / 2;
    int cy = (jh - ch) / 2;
    if (cx < 0) {
      cx = 0;
    }
    if (cy < 0) {
      cy = 0;
    }
    jpeg.setCropArea(cx, cy, cw, ch);

    if (jpeg.decode(0, 0, kScales[attempt]) && jpeg.getLastError() == JPEG_SUCCESS) {
      ok = true;
    }
    jpeg.close();
    if (ok) {
      break;
    }
  }

  free(jpg);
  *e = ok ? 0 : -7;
  return ok;
}
