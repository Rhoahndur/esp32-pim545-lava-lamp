#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "soc/usb_serial_jtag_reg.h"
#include "soc/soc.h"

#include "../pim545_lava_lamp/lava_lamp.h"
#include "../pim545_lava_lamp/ripple.h"

// Waveshare ESP32-S3-Touch-LCD-7: RGB + bounce buffer (stops PSRAM scan drift).
static Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    5, 3, 46, 7,
    1, 2, 42, 41, 40,
    39, 0, 45, 48, 47, 21,
    14, 38, 18, 17, 10,
    0, 8, 4, 8,
    0, 8, 4, 8,
    1, 12900000, false,
    0, 0, 800 * 10);

static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    800, 480, rgbpanel, 0, true);

static LavaLamp lamp;
static Ripples ripples;
static uint8_t rgb[LAMP_PIXELS * 3];
static uint8_t prev_rgb[LAMP_PIXELS * 3];
static bool ready = false;
static bool touching = false;
static uint32_t last_frame = 0;
static uint32_t last_drop_ms = 0;
static uint8_t ch422_shadow = 0;

static const int CELL_W = 40;
static const int CELL_H = 20;
static const int GAP = 4;
static const int GRID_X = (800 - (9 * CELL_W + 8 * GAP)) / 2;
static const int GRID_Y = 52;

struct Button { int x, y, w, h; char name; float px, py; };
static const Button buttons[] = {
    {24, 52, 140, 72, 'A', 0.3f, 0.3f},
    {636, 52, 140, 72, 'B', 7.8f, 0.3f},
    {24, 356, 140, 72, 'X', 0.3f, 15.7f},
    {636, 356, 140, 72, 'Y', 7.8f, 15.7f},
};

static void ch422_flush() {
  Wire.beginTransmission(0x24);
  Wire.write(0x01);
  Wire.endTransmission();
  Wire.beginTransmission(0x38);
  Wire.write(ch422_shadow);
  Wire.endTransmission();
}

static void ch422_pin(uint8_t pin, bool on) {
  if (on) {
    ch422_shadow |= (uint8_t)(1u << pin);
  } else {
    ch422_shadow &= (uint8_t)~(1u << pin);
  }
  ch422_flush();
}

static void expander_init() {
  Wire.begin(8, 9, 400000);
  // EXIO: 1 TP_RST, 2 BL, 3 LCD_RST, 4 SD_CS, 5 USB_SEL, 6 LCD_VDD
  ch422_shadow = (1 << 2) | (1 << 3) | (1 << 4) | (1 << 6);
  ch422_flush();
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  ch422_pin(1, false);
  delay(80);
  ch422_pin(1, true);
  delay(10);
  pinMode(4, INPUT);
}

static bool gt911_touch(uint16_t *x, uint16_t *y) {
  Wire.beginTransmission(0x5D);
  Wire.write(0x81);
  Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((uint8_t)0x5D, (uint8_t)1) != 1) {
    return false;
  }
  uint8_t status = Wire.read();
  if ((status & 0x80) == 0 || (status & 0x0F) == 0) {
    return false;
  }
  Wire.beginTransmission(0x5D);
  Wire.write(0x81);
  Wire.write(0x50);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((uint8_t)0x5D, (uint8_t)4) != 4) {
    return false;
  }
  uint8_t b0 = Wire.read();
  uint8_t b1 = Wire.read();
  uint8_t b2 = Wire.read();
  uint8_t b3 = Wire.read();
  *x = (uint16_t)(b0 | (b1 << 8));
  *y = (uint16_t)(b2 | (b3 << 8));
  Wire.beginTransmission(0x5D);
  Wire.write(0x81);
  Wire.write(0x4E);
  Wire.write(0x00);
  Wire.endTransmission();
  return *x < 800 && *y < 480;
}

static void identify() {
  Serial.println("LAVA_CONTROLLER 1 WAVESHARE_TOUCH_7 corners");
}

static void drop(const Button &b) {
  uint32_t now = millis();
  if (now - last_drop_ms < 180) {
    return;
  }
  last_drop_ms = now;
  ripples.drop(b.px, b.py);
  lamp.impulse(b.px, b.py, 2.2f);
  Serial.printf("PEBBLE %c\n", b.name);
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void draw_chrome() {
  gfx->fillScreen(rgb565(8, 10, 18));
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(250, 12);
  gfx->print("GREEN BUILDING LAVA");
  gfx->setTextSize(3);
  for (const Button &b : buttons) {
    gfx->fillRoundRect(b.x, b.y, b.w, b.h, 10, rgb565(40, 48, 72));
    gfx->setCursor(b.x + b.w / 2 - 10, b.y + b.h / 2 - 12);
    gfx->printf("%c", b.name);
  }
  gfx->setTextSize(1);
  gfx->setCursor(210, 464);
  gfx->print("9x17 windows  |  tap a corner  |  UART to simulator");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("boot");
  identify();
  if (!psramFound()) {
    Serial.println("ERROR: PSRAM missing");
    return;
  }
  Serial.printf("PSRAM %u bytes\n", ESP.getPsramSize());
  expander_init();
  if (!gfx->begin()) {
    Serial.println("ERROR: RGB panel begin failed");
    return;
  }
  // USB Type-C can stay plugged for 5 V. USB-Serial/JTAG DMA fights the
  // RGB panel; ROM re-enables it in download mode so BOOT+RESET still flashes.
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  gfx->fillScreen(0x0000);
  draw_chrome();
  memset(prev_rgb, 0, sizeof(prev_rgb));
  lamp.reseed(micros());
  ready = true;
  Serial.println("Touchscreen ready. Send ? to identify; a/b/x/y to tap.");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '?') {
      identify();
    }
    for (const Button &b : buttons) {
      if (c == b.name || c == b.name + ('a' - 'A')) {
        drop(b);
      }
    }
  }
  if (!ready) {
    delay(20);
    return;
  }

  uint16_t tx, ty;
  bool down = gt911_touch(&tx, &ty);
  if (down && !touching) {
    for (const Button &b : buttons) {
      if (tx >= (uint16_t)b.x && tx < (uint16_t)(b.x + b.w) &&
          ty >= (uint16_t)b.y && ty < (uint16_t)(b.y + b.h)) {
        drop(b);
      }
    }
  }
  touching = down;

  uint32_t now = millis();
  if (now - last_frame < 100) {
    delay(2);
    return;
  }
  last_frame = now;
  lamp.step(0.10f);
  ripples.step(0.10f, &lamp, true);
  lamp.render_rgb(rgb);
  ripples.apply_rgb(rgb);

  for (int gy = 0; gy < 17; ++gy) {
    for (int gx = 0; gx < 9; ++gx) {
      int i = (gy * 9 + gx) * 3;
      if (memcmp(rgb + i, prev_rgb + i, 3) == 0) {
        continue;
      }
      memcpy(prev_rgb + i, rgb + i, 3);
      int x = GRID_X + gx * (CELL_W + GAP);
      int y = GRID_Y + gy * (CELL_H + GAP);
      gfx->fillRect(x, y, CELL_W, CELL_H, rgb565(rgb[i], rgb[i + 1], rgb[i + 2]));
    }
  }
  delay(2);
}
