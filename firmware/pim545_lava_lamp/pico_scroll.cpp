#include "pico_scroll.h"

#include <string.h>

// Gamma table from Pimoroni's Scroll pHAT HD library, tuned for these LEDs.
static const uint8_t kGamma[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,
    2,   2,   2,   3,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,
    6,   6,   6,   7,   7,   7,   8,   8,   8,   9,   9,   9,   10,  10,  11,  11,
    11,  12,  12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,
    19,  19,  20,  21,  21,  22,  22,  23,  23,  24,  25,  25,  26,  27,  27,  28,
    29,  29,  30,  31,  31,  32,  33,  34,  34,  35,  36,  37,  37,  38,  39,  40,
    40,  41,  42,  43,  44,  45,  46,  46,  47,  48,  49,  50,  51,  52,  53,  54,
    55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
    71,  72,  73,  74,  76,  77,  78,  79,  80,  81,  83,  84,  85,  86,  88,  89,
    90,  91,  93,  94,  95,  96,  98,  99,  100, 102, 103, 104, 106, 107, 109, 110,
    111, 113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 128, 129, 131, 132, 134,
    135, 137, 138, 140, 142, 143, 145, 146, 148, 150, 151, 153, 155, 157, 158, 160,
    162, 163, 165, 167, 169, 170, 172, 174, 176, 178, 179, 181, 183, 185, 187, 189,
    191, 193, 194, 196, 198, 200, 202, 204, 206, 208, 210, 212, 214, 216, 218, 220,
    222, 224, 227, 229, 231, 233, 235, 237, 239, 241, 244, 246, 248, 250, 252, 255};

bool PicoScroll::begin(TwoWire *wire) {
  wire_ = wire;
  found_ = false;
  draw_frame_ = 0;
  memset(pwm_, 0, sizeof(pwm_));

  wire_->begin(PIN_SDA, PIN_SCL);
  wire_->setClock(I2C_HZ);
  delay(20);

  wire_->beginTransmission(PIM545_I2C_ADDR);
  if (wire_->endTransmission() != 0) {
    return false;
  }

  // Shutdown, then wake. Picture mode, no audio sync, display frame 0.
  if (!write_reg(CONFIG_BANK, REG_SHUTDOWN, 0x00)) {
    return false;
  }
  delay(10);
  if (!write_reg(CONFIG_BANK, REG_SHUTDOWN, 0x01)) {
    return false;
  }
  if (!write_reg(CONFIG_BANK, REG_MODE, 0x00)) {
    return false;
  }
  if (!write_reg(CONFIG_BANK, REG_AUDIOSYNC, 0x00)) {
    return false;
  }
  if (!write_reg(CONFIG_BANK, REG_FRAME, 0x00)) {
    return false;
  }

  // Enable the 17x7 LEDs that exist on the pack (bitmask from Pimoroni).
  const uint8_t enable[18] = {
      0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
      0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x00};
  for (uint8_t frame = 0; frame < 8; frame++) {
    if (!select_bank(frame)) {
      return false;
    }
    if (!write_block(0x00, enable, 18)) {
      return false;
    }
  }

  clear();
  show();
  found_ = true;
  return true;
}

void PicoScroll::clear() { memset(pwm_, 0, sizeof(pwm_)); }

int PicoScroll::native_addr(int x, int y) const {
  if (x < 0 || x >= SCROLL_WIDTH || y < 0 || y >= SCROLL_HEIGHT) {
    return -1;
  }
  // Pimoroni Pico Scroll / Scroll pHAT HD mapping.
  y = (SCROLL_HEIGHT - 1) - y;
  if (x > 8) {
    x = x - 8;
    y = (SCROLL_HEIGHT - 1) - (y + 8);
  } else {
    x = 8 - x;
  }
  return x * 16 + y;
}

void PicoScroll::lamp_to_native(int lx, int ly, int *nx, int *ny) const {
  int x = lx;
  int y = ly;
#if SWAP_XY
  int tmp = x;
  x = y;
  y = tmp;
#endif
#if FLIP_X
  x = SCROLL_WIDTH - 1 - x;
#endif
#if FLIP_Y
  y = SCROLL_HEIGHT - 1 - y;
#endif
  *nx = x;
  *ny = y;
}

void PicoScroll::set_native(int x, int y, uint8_t pwm) {
  int addr = native_addr(x, y);
  if (addr >= 0 && addr < PWM_BYTES) {
    pwm_[addr] = pwm;
  }
}

uint8_t PicoScroll::apply_gamma(uint8_t luma, uint8_t brightness) const {
  uint16_t scaled = (uint16_t)luma * brightness / 255;
  if (scaled > 255) {
    scaled = 255;
  }
  return kGamma[scaled];
}

void PicoScroll::set_lamp(int x, int y, uint8_t luma) {
  int nx, ny;
  lamp_to_native(x, y, &nx, &ny);
  set_native(nx, ny, luma);
}

void PicoScroll::fill_lamp(const uint8_t *luma, int count, uint8_t brightness) {
  clear();
  int n = count < LAMP_PIXELS ? count : LAMP_PIXELS;
  for (int i = 0; i < n; i++) {
    int x = i % LAMP_WIDTH;
    int y = i / LAMP_WIDTH;
    set_lamp(x, y, apply_gamma(luma[i], brightness));
  }
}

void PicoScroll::show() {
  if (!select_bank(draw_frame_)) {
    return;
  }
  // Wire buffer on ESP32 is 128 bytes; send PWM in 24-byte chunks.
  for (int offset = 0; offset < PWM_BYTES; offset += 24) {
    int chunk = PWM_BYTES - offset;
    if (chunk > 24) {
      chunk = 24;
    }
    if (!write_block(COLOR_OFFSET + offset, pwm_ + offset, chunk)) {
      return;
    }
  }
  write_reg(CONFIG_BANK, REG_FRAME, draw_frame_);
  draw_frame_ = draw_frame_ ? 0 : 1;
}

void PicoScroll::test_pattern(uint32_t now_ms) {
  clear();
  uint32_t phase = (now_ms / 400) % 6;
  uint8_t on = 180;
  switch (phase) {
    case 0:
      set_lamp(0, 0, on);
      break;
    case 1:
      set_lamp(LAMP_WIDTH - 1, 0, on);
      break;
    case 2:
      set_lamp(LAMP_WIDTH - 1, LAMP_HEIGHT - 1, on);
      break;
    case 3:
      set_lamp(0, LAMP_HEIGHT - 1, on);
      break;
    case 4:
      for (int x = 0; x < LAMP_WIDTH; x++) {
        set_lamp(x, 0, on);
      }
      break;
    default:
      for (int y = 0; y < LAMP_HEIGHT; y++) {
        set_lamp(0, y, on);
      }
      break;
  }
  show();
}

bool PicoScroll::select_bank(uint8_t bank) {
  wire_->beginTransmission(PIM545_I2C_ADDR);
  wire_->write(BANK_CMD);
  wire_->write(bank);
  return wire_->endTransmission() == 0;
}

bool PicoScroll::write_reg(uint8_t bank, uint8_t reg, uint8_t value) {
  if (reg != BANK_CMD && !select_bank(bank)) {
    return false;
  }
  wire_->beginTransmission(PIM545_I2C_ADDR);
  wire_->write(reg);
  wire_->write(value);
  return wire_->endTransmission() == 0;
}

bool PicoScroll::write_block(uint8_t start_reg, const uint8_t *data, int len) {
  wire_->beginTransmission(PIM545_I2C_ADDR);
  wire_->write(start_reg);
  for (int i = 0; i < len; i++) {
    wire_->write(data[i]);
  }
  return wire_->endTransmission() == 0;
}
