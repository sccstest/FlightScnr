#include "hardware/display.h"

#include "hardware/display_brightness.h"
#include "hardware/display_font.h"
#include "hardware/panel.h"
#include "hardware/pin_config.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "hardware/p4_dsi_panel.h"
#include "boards/waveshare_p4_4c_init.h"
#elif defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
#include "display/Arduino_ST77916.h"
#include "boards/waveshare_st77916_init.h"
#else
#include "display/Arduino_CO5300.h"
#include "display/Arduino_SH8601.h"
#endif

namespace {

Arduino_DataBus* s_bus = nullptr;
Arduino_GFX* s_panel = nullptr;

}  // namespace

PlaneGfx tft;

void displayInit() {
#if FLIGHTSCNR_HAS_LCD_VCI_EN
  pinMode(LCD_VCI_EN, OUTPUT);
  digitalWrite(LCD_VCI_EN, HIGH);
#endif

#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
#endif

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // MIPI-DSI framebuffer panel: the "bus" writes straight into PSRAM.
  {
    P4DsiConfig cfg = {};
    cfg.width = LCD_WIDTH;
    cfg.height = LCD_HEIGHT;
    cfg.lanes = LCD_DSI_LANES;
    cfg.lane_mbps = LCD_DSI_LANE_MBPS;
    cfg.dpi_clock_mhz = LCD_DPI_CLOCK_MHZ;
    cfg.hsync_pulse = LCD_HSYNC_PULSE;
    cfg.hsync_back_porch = LCD_HSYNC_BACK_PORCH;
    cfg.hsync_front_porch = LCD_HSYNC_FRONT_PORCH;
    cfg.vsync_pulse = LCD_VSYNC_PULSE;
    cfg.vsync_back_porch = LCD_VSYNC_BACK_PORCH;
    cfg.vsync_front_porch = LCD_VSYNC_FRONT_PORCH;
    cfg.phy_ldo_chan = LCD_DSI_PHY_LDO_CHAN;
    cfg.phy_ldo_mv = LCD_DSI_PHY_LDO_MV;
    cfg.init_cmds = waveshare_p4_4c::kInit;
    cfg.init_cmds_len = sizeof(waveshare_p4_4c::kInit) / sizeof(waveshare_p4_4c::kInit[0]);
    auto* fb_bus = new P4FramebufferBus();
    s_bus = fb_bus;
    s_panel = new Arduino_P4Dsi(fb_bus, LCD_RST, cfg);
    Arduino_TFT::setPixelAlign2(false);
    Serial.println("Display: Waveshare 4C MIPI-DSI 720x720");
  }
#else
  s_bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2,
                                  LCD_SDIO3);
#endif

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // (panel created above)
#elif defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
  // Knob 1.8 needs st77916_150-class init. Rotation comes from the board header
  // (USB/cable orientation); touch coordinates are remapped to match.
  s_panel =
      new Arduino_ST77916(s_bus, LCD_RST, FLIGHTSCNR_DISPLAY_ROTATION, true, LCD_WIDTH,
                          LCD_HEIGHT, 0, 0, 0, 0, kWaveshareSt77916Init,
                          sizeof(kWaveshareSt77916Init));
  Arduino_TFT::setPixelAlign2(false);
  Serial.printf("Display: ST77916 (150 init, rot=%u)\n",
                static_cast<unsigned>(FLIGHTSCNR_DISPLAY_ROTATION));
#else
  if (hardware::panelUsesCo5300()) {
    s_panel = new Arduino_CO5300(s_bus, LCD_RST, 0, false, LCD_WIDTH, LCD_HEIGHT,
                                 0, 0, 0, 0);
    Arduino_TFT::setPixelAlign2(true);
    Serial.println("Display: CO5300");
  } else {
    s_panel = new Arduino_SH8601(s_bus, LCD_RST, 0, false, LCD_WIDTH, LCD_HEIGHT);
    Arduino_TFT::setPixelAlign2(false);
    Serial.println("Display: SH8601");
  }
#endif

  if (!s_panel->begin(40000000)) {
    Serial.println("Display init failed");
  }

  tft.attach(s_panel, true);
  planeGfxPanelLockInit();
  tft.fillScreen(BLACK);

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18) || defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // ST77916 / P4 DSI brightness is PWM backlight (see display_brightness).
#else
  for (uint8_t brightness = 0; brightness < 255; ++brightness) {
    s_panel->Display_Brightness(brightness);
    delay(2);
  }

  if (hardware::panelUsesCo5300()) {
    s_panel->SetContrast(CO5300_ContrastOff);
  } else {
    s_panel->SetContrast(SH8601_ContrastOff);
  }
#endif

  hardware::displayBrightnessBootLoad();
  hardware::displayApplyBrightness();

  tft.setTextWrap(false);
}

void displaySleep() {
  if (s_panel != nullptr) {
    s_panel->displayOff();
  }
#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
  digitalWrite(LCD_BL, LOW);
#endif
}

void displayWake() {
#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
  hardware::displayApplyBrightness();
#endif
  if (s_panel != nullptr) {
    s_panel->displayOn();
  }
}
