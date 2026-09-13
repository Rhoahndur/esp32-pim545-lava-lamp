#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>

#include "config.h"

// Driver for the Pimoroni Pico Scroll Pack (PIM545):
// IS31FL3731 at 0x74, 17x7 white LEDs, Pico-header pinout.
class PicoScroll {
 public:
  bool begin(TwoWire *wire = &Wire);
  bool found() const { return found_; }

  void clear();
  void set_native(int x, int y, uint8_t pwm);
  void set_lamp(int x, int y, uint8_t luma);
  void fill_lamp(const uint8_t *luma, int count, uint8_t brightness);
  void show();

  void test_pattern(uint32_t now_ms);

 private:
  static const uint8_t BANK_CMD = 0xFD;
  static const uint8_t CONFIG_BANK = 0x0B;
  static const uint8_t REG_MODE = 0x00;
  static const uint8_t REG_FRAME = 0x01;
  static const uint8_t REG_AUDIOSYNC = 0x06;
  static const uint8_t REG_SHUTDOWN = 0x0A;
  static const uint8_t COLOR_OFFSET = 0x24;
  static const int PWM_BYTES = 144;

  bool select_bank(uint8_t bank);
  bool write_reg(uint8_t bank, uint8_t reg, uint8_t value);
  bool write_block(uint8_t start_reg, const uint8_t *data, int len);
  int native_addr(int x, int y) const;
  void lamp_to_native(int lx, int ly, int *nx, int *ny) const;
  uint8_t apply_gamma(uint8_t luma, uint8_t brightness) const;

  TwoWire *wire_ = &Wire;
  bool found_ = false;
  uint8_t draw_frame_ = 0;
  uint8_t pwm_[PWM_BYTES] = {};
};
