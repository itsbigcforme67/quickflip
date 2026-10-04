// Resistive touch (XPT2046) read by toggling pins, so it needs no SPI port of its own.
#pragma once
#include <stdint.h>

struct TouchCal {
  // screen = (raw - r0) * span / (r1 - r0) for each axis; swap = raw X drives screen Y
  int16_t x0, x1, y0, y1;
  uint8_t swap, valid;
};

void touch_begin();
// Raw reading: true while pressed (pressure over the threshold), averaged over a few samples.
bool touch_raw(int *rx, int *ry);
// Screen position (320x240) using the calibration. False when not pressed.
bool touch_read(int *x, int *y);
extern TouchCal touch_cal;
