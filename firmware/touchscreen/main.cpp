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
static uint8_t gt_addr = 0x5D;

static const int CELL_W = 40;
static const int CELL_H = 20;
static const int GAP = 4;
static const int GRID_W = LAMP_WIDTH * CELL_W + (LAMP_WIDTH - 1) * GAP;
static const int GRID_H = LAMP_HEIGHT * CELL_H + (LAMP_HEIGHT - 1) * GAP;
static const int GRID_X = (800 - GRID_W) / 2;
static const int GRID_Y = 52;
static const int GRID_RIGHT = GRID_X + GRID_W;

// Touch panel orientation. The GT911 on this board reports in the same frame
// as the 800x480 LCD, so all three are 0. If a tap lands in the wrong corner,
// read the `TOUCH x y` log line and flip exactly one of these.
#define TOUCH_SWAP_XY 0
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0

// Treat the finger as lifted once the GT911 has gone this long without
// reporting a point. Polls that carry no fresh data must not look like a
// release, or a single press fires a burst of drops.
static const uint32_t TOUCH_RELEASE_MS = 70;
static const uint32_t FLASH_MS = 140;

struct Button { int x, y, w, h; char name; float px, py; };
static const Button buttons[] = {
    {24, 52, 140, 72, 'A', 0.3f, 0.3f},
    {636, 52, 140, 72, 'B', 7.8f, 0.3f},
    {24, 356, 140, 72, 'X', 0.3f, 15.7f},
    {636, 356, 140, 72, 'Y', 7.8f, 15.7f},
};
static const int BUTTON_COUNT = (int)(sizeof(buttons) / sizeof(buttons[0]));

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

static void i2c_scan() {
  Serial.print("I2C:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a);
    }
  }
  Serial.println();
}

static bool gt911_read_reg(uint8_t addr, uint16_t reg, uint8_t *out, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission() != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, len) != len) {
    return false;
  }
  for (uint8_t i = 0; i < len; i++) {
    out[i] = (uint8_t)Wire.read();
  }
  return true;
}

static bool gt911_write_reg(uint8_t addr, uint16_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// False only when the panel stops acknowledging on the bus. An idle panel with
// no finger on it still reports a healthy bus, so this must not be confused
// with "no touch": re-probing on idle would run forever.
static bool gt_bus_ok = true;

static bool gt911_read_addr(uint8_t addr, uint16_t *x, uint16_t *y) {
  uint8_t status = 0;
  if (!gt911_read_reg(addr, 0x814E, &status, 1)) {
    gt_bus_ok = false;
    return false;
  }
  gt_bus_ok = true;
  if ((status & 0x80) == 0) {
    // No fresh sample. The buffer flag belongs to the GT911 here; leave it.
    return false;
  }
  bool got = false;
  if ((status & 0x0F) != 0) {
    uint8_t p[4];
    if (gt911_read_reg(addr, 0x8150, p, 4)) {
      uint16_t rx = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
      uint16_t ry = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
      if (rx < 800 && ry < 480) {
        *x = rx;
        *y = ry;
        got = true;
      }
    }
  }
  // Hand the buffer back on EVERY ready sample, including the zero-point
  // report the GT911 emits on finger-up. Skipping that one latches bit 7 and
  // the controller never refreshes coordinates again: the first tap works and
  // every tap after it is silently dropped.
  gt911_write_reg(addr, 0x814E, 0x00);
  return got;
}

static bool gt911_probe(uint8_t addr) {
  uint8_t id[4];
  if (!gt911_read_reg(addr, 0x8140, id, 4)) {
    return false;
  }
  Serial.printf("GT911 0x%02X id %c%c%c%c\n", addr, id[0], id[1], id[2], id[3]);
  return true;
}

static void gt911_select_addr() {
  if (gt911_probe(0x5D)) {
    gt_addr = 0x5D;
    return;
  }
  if (gt911_probe(0x14)) {
    gt_addr = 0x14;
    return;
  }
  Serial.println("ERROR: GT911 not answering at 0x5D or 0x14");
}

static bool gt911_touch(uint16_t *x, uint16_t *y) {
  static uint32_t last_reprobe = 0;
  if (gt911_read_addr(gt_addr, x, y)) {
    return true;
  }
  // Re-probe only when the panel has actually gone off the bus, never just
  // because nobody is touching it.
  uint32_t now = millis();
  if (!gt_bus_ok && now - last_reprobe > 3000) {
    last_reprobe = now;
    gt911_select_addr();
  }
  return false;
}

static void send_rgb_frame() {
  uint8_t hdr[4] = {0xAA, 0x55, (uint8_t)LAMP_WIDTH, (uint8_t)LAMP_HEIGHT};
  uint8_t xsum = 0;
  for (int i = 0; i < 4; i++) {
    xsum ^= hdr[i];
  }
  for (int i = 0; i < LAMP_PIXELS * 3; i++) {
    xsum ^= rgb[i];
  }
  Serial.write(hdr, 4);
  Serial.write(rgb, LAMP_PIXELS * 3);
  Serial.write(xsum);
}

// Contact bounce is already handled by the press/release edge in loop(); this
// is only a floor so a held finger or a burst of serial commands cannot spam
// the ripple pool. Keep it well under a deliberate double tap.
static const uint32_t DROP_MIN_GAP_MS = 60;

static bool drop_xy(float px, float py, char name) {
  uint32_t now = millis();
  if (now - last_drop_ms < DROP_MIN_GAP_MS) {
    return false;
  }
  last_drop_ms = now;
  ripples.drop(px, py);
  lamp.impulse(px, py, 2.2f);
  if (name) {
    Serial.printf("PEBBLE %c\n", name);
  } else {
    Serial.printf("PEBBLE %d %d\n", (int)(px + 0.5f), (int)(py + 0.5f));
  }
  return true;
}

static void identify() {
  Serial.println("LAVA_CONTROLLER 1 WAVESHARE_TOUCH_7 corners");
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static int flash_index = -1;
static uint32_t flash_until = 0;

static void draw_button(const Button &b, bool lit) {
  gfx->fillRoundRect(b.x, b.y, b.w, b.h, 10,
                     lit ? rgb565(210, 226, 255) : rgb565(40, 48, 72));
  gfx->setTextColor(lit ? rgb565(8, 10, 18) : 0xFFFF);
  gfx->setTextSize(3);
  gfx->setCursor(b.x + b.w / 2 - 10, b.y + b.h / 2 - 12);
  gfx->printf("%c", b.name);
  gfx->setTextColor(0xFFFF);
}

static void drop(const Button &b) {
  if (!drop_xy(b.px, b.py, b.name) || !ready) {
    return;
  }
  // Visible on-device acknowledgement: if the button lights up but the
  // simulator does not move, the fault is in the bridge, not in the panel.
  flash_index = (int)(&b - buttons);
  flash_until = millis() + FLASH_MS;
  draw_button(b, true);
}

static void draw_chrome() {
  gfx->fillScreen(rgb565(8, 10, 18));
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(250, 12);
  gfx->print("GREEN BUILDING LAVA");
  for (const Button &b : buttons) {
    draw_button(b, false);
  }
  gfx->setTextSize(1);
  gfx->setCursor(190, 464);
  gfx->print("9x17 windows  |  tap a side or a window  |  UART to simulator");
}

// Map a raw GT911 sample onto the drawn layout, then onto a pebble.
// The whole left/right margin counts as its corner pair, so a fat finger or a
// panel that is a few pixels out of calibration still lands on the button.
static void handle_tap(uint16_t rawx, uint16_t rawy) {
  int tx = rawx;
  int ty = rawy;
#if TOUCH_SWAP_XY
  int swap = tx;
  tx = ty;
  ty = swap;
#endif
#if TOUCH_FLIP_X
  tx = 799 - tx;
#endif
#if TOUCH_FLIP_Y
  ty = 479 - ty;
#endif

  if (tx < GRID_X || tx >= GRID_RIGHT) {
    bool top = ty < 240;
    bool left = tx < GRID_X;
    char name = left ? (top ? 'A' : 'X') : (top ? 'B' : 'Y');
    for (int i = 0; i < BUTTON_COUNT; i++) {
      if (buttons[i].name == name) {
        drop(buttons[i]);
        return;
      }
    }
    return;
  }
  if (ty >= GRID_Y && ty < GRID_Y + GRID_H) {
    int gx = (tx - GRID_X) / (CELL_W + GAP);
    int gy = (ty - GRID_Y) / (CELL_H + GAP);
    if (gx >= 0 && gx < LAMP_WIDTH && gy >= 0 && gy < LAMP_HEIGHT) {
      drop_xy((float)gx, (float)gy, 0);
    }
  }
}

void setup() {
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(200);
  Serial.println("boot");
  identify();
  if (!psramFound()) {
    Serial.println("ERROR: PSRAM missing");
    return;
  }
  Serial.printf("PSRAM %u bytes\n", ESP.getPsramSize());
  expander_init();
  i2c_scan();
  delay(60);  // GT911 needs ~50 ms after reset before it answers.
  gt911_select_addr();
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

  uint32_t now = millis();
  static uint32_t last_touch_poll = 0;
  static uint32_t last_point_ms = 0;
  if (now - last_touch_poll >= 15) {
    last_touch_poll = now;
    uint16_t rawx = 0, rawy = 0;
    if (gt911_touch(&rawx, &rawy)) {
      last_point_ms = now;
      if (!touching) {
        touching = true;
        Serial.printf("TOUCH %u %u\n", rawx, rawy);
        handle_tap(rawx, rawy);
      }
    } else if (touching && now - last_point_ms >= TOUCH_RELEASE_MS) {
      touching = false;
    }
  }

  if (flash_index >= 0 && (int32_t)(now - flash_until) >= 0) {
    draw_button(buttons[flash_index], false);
    flash_index = -1;
  }

  if (now - last_frame < 100) {
    delay(2);
    return;
  }
  last_frame = now;
  lamp.step(0.10f);
  ripples.step(0.10f, &lamp, true);
  lamp.render_rgb(rgb);
  ripples.apply_rgb(rgb);
  send_rgb_frame();

  for (int gy = 0; gy < LAMP_HEIGHT; ++gy) {
    for (int gx = 0; gx < LAMP_WIDTH; ++gx) {
      int i = (gy * LAMP_WIDTH + gx) * 3;
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
