#include "touch.h"
#include <Arduino.h>
#include "board.h"

TouchCal touch_cal = {200, 3800, 240, 3800, 0, 0};

void touch_begin() {
  pinMode(QF_TOUCH_CLK, OUTPUT);
  pinMode(QF_TOUCH_DIN, OUTPUT);
  pinMode(QF_TOUCH_CS, OUTPUT);
  pinMode(QF_TOUCH_DOUT, INPUT);
  pinMode(QF_TOUCH_IRQ, INPUT);
  digitalWrite(QF_TOUCH_CS, HIGH);
  digitalWrite(QF_TOUCH_CLK, LOW);
}

// Send a command byte, read back the 12-bit result.
static int xfer(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(QF_TOUCH_DIN, (cmd >> i) & 1);
    digitalWrite(QF_TOUCH_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(QF_TOUCH_CLK, LOW);
    delayMicroseconds(1);
  }
  int r = 0;
  for (int i = 11; i >= 0; i--) {
    digitalWrite(QF_TOUCH_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(QF_TOUCH_CLK, LOW);
    delayMicroseconds(1);
    r |= digitalRead(QF_TOUCH_DOUT) << i;
  }
  return r;
}

bool touch_raw(int *rx, int *ry) {
  digitalWrite(QF_TOUCH_CS, LOW);
  int z = xfer(0xB0) + 4095 - xfer(0xC0);
  bool down = z > 600;
  int sx = 0, sy = 0;
  if (down) {
    xfer(0xD0);  // the first reading after switching is often off; throw it away
    int xs[5], ys[5];
    for (int i = 0; i < 5; i++) { xs[i] = xfer(0xD0); ys[i] = xfer(0x90); }
    // drop the highest and lowest, average the rest
    for (int a = 0; a < 5; a++)
      for (int b = a + 1; b < 5; b++) {
        if (xs[b] < xs[a]) { int t = xs[a]; xs[a] = xs[b]; xs[b] = t; }
        if (ys[b] < ys[a]) { int t = ys[a]; ys[a] = ys[b]; ys[b] = t; }
      }
    sx = (xs[1] + xs[2] + xs[3]) / 3;
    sy = (ys[1] + ys[2] + ys[3]) / 3;
    // check it is still pressed, so a reading taken while lifting the pen is not used
    z = xfer(0xB0) + 4095 - xfer(0xC0);
    down = z > 600;
  }
  xfer(0x80);  // power down between readings
  digitalWrite(QF_TOUCH_CS, HIGH);
  *rx = sx;
  *ry = sy;
  return down;
}

bool touch_read(int *x, int *y) {
  int rx, ry;
  if (!touch_raw(&rx, &ry)) return false;
  if (touch_cal.swap) { int t = rx; rx = ry; ry = t; }
  const TouchCal &c = touch_cal;
  int sx = c.x1 != c.x0 ? (rx - c.x0) * 320 / (c.x1 - c.x0) : 0;
  int sy = c.y1 != c.y0 ? (ry - c.y0) * 240 / (c.y1 - c.y0) : 0;
  *x = sx < 0 ? 0 : sx > 319 ? 319 : sx;
  *y = sy < 0 ? 0 : sy > 239 ? 239 : sy;
  return true;
}
