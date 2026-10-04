// Pins and screen setup for each supported board. To add a board: a new QF_BOARD_ block here
// and an [env] in platformio.ini.
#pragma once
#include <LovyanGFX.hpp>

#if defined(QF_BOARD_CYD)
// ESP32-2432S028R. Screen on HSPI, touch on its own pins (read by touch.cpp, no SPI port
// needed), SD card on the default VSPI pins (18, 19, 23, CS 5).
#define QF_TFT_SCLK 14
#define QF_TFT_MOSI 13
#define QF_TFT_MISO 12
#define QF_TFT_DC 2
#define QF_TFT_CS 15
#define QF_TFT_BL 21
#define QF_TOUCH_CLK 25
#define QF_TOUCH_DIN 32
#define QF_TOUCH_DOUT 39
#define QF_TOUCH_CS 33
#define QF_TOUCH_IRQ 36
#define QF_SD_CS 5
#define QF_TFT_SPI_HOST HSPI_HOST
#else
#error "Pick a board in platformio.ini (QF_BOARD_CYD)"
#endif

class LGFX : public lgfx::LGFX_Device {
#ifdef QF_PANEL_ST7789
  lgfx::Panel_ST7789 panel_;
#else
  lgfx::Panel_ILI9341 panel_;
#endif
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;

 public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = QF_TFT_SPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = QF_TFT_SCLK;
      cfg.pin_mosi = QF_TFT_MOSI;
      cfg.pin_miso = QF_TFT_MISO;
      cfg.pin_dc = QF_TFT_DC;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = QF_TFT_CS;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_rotation = 0;
      cfg.readable = false;
#ifdef QF_PANEL_ST7789
      cfg.invert = true;
#else
      cfg.invert = false;
#endif
      cfg.rgb_order = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = QF_TFT_BL;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }
};
