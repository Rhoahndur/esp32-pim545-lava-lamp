#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "lava_lamp.h"
#include "pico_scroll.h"
#include "ripple.h"

enum Mode { MODE_LAVA, MODE_TEST, MODE_HOST };

static PicoScroll scroll;
static LavaLamp lamp;
static Ripples ripples;
static Mode mode = MODE_LAVA;
static bool paused = false;
static uint8_t brightness = DEFAULT_BRIGHTNESS;
static uint8_t luma[LAMP_PIXELS];
static uint32_t last_frame_ms = 0;
static uint32_t last_host_ms = 0;

// Physical corners with A/B at the roof, X/Y at the ground.
struct CornerBtn {
  int pin;
  float x;
  float y;
  const char *name;
  bool was_pressed;
  uint32_t last_edge_ms;
};

static CornerBtn corners[] = {
    {PIN_BTN_B, (float)LAMP_WIDTH - 1.2f, 0.2f, "B", false, 0},
    {PIN_BTN_A, 0.2f, 0.2f, "A", false, 0},
    {PIN_BTN_Y, (float)LAMP_WIDTH - 1.2f, (float)LAMP_HEIGHT - 1.2f, "Y", false, 0},
    {PIN_BTN_X, 0.2f, (float)LAMP_HEIGHT - 1.2f, "X", false, 0},
};

static const uint8_t PKT_MAGIC0 = 0x50;  // 'P'
static const uint8_t PKT_MAGIC1 = 0x53;  // 'S'
static const int PKT_LEN = 4 + LAMP_PIXELS + 1;

static uint8_t pkt[PKT_LEN];
static int pkt_n = 0;

static void scan_i2c() {
  Serial.println(F("I2C scan:"));
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X%s\n", addr,
                    addr == PIM545_I2C_ADDR ? "  (PIM545 / IS31FL3731)" : "");
      found++;
    }
  }
  if (!found) {
    Serial.println(F("  nothing found"));
  }
}

static void print_help() {
  Serial.println();
  Serial.println(F("PIM545 lava lamp  7x17 white LEDs  IS31FL3731 @ 0x74"));
  Serial.printf("I2C  SDA=GPIO%d  SCL=GPIO%d  3.3V -> VSYS  GND -> GND\n", PIN_SDA, PIN_SCL);
  Serial.println(F("buttons (optional, active-low): tap a corner to drop a pebble"));
  Serial.printf("  A=GPIO%d top-left   B=GPIO%d top-right\n", PIN_BTN_A, PIN_BTN_B);
  Serial.printf("  X=GPIO%d bot-left   Y=GPIO%d bot-right\n", PIN_BTN_X, PIN_BTN_Y);
  Serial.println(F("serial: a/b/x/y pebble  t=test  l=lava  p=pause  s=reseed  +/-=brightness"));
  Serial.println(F("host frames: 0x50 0x53 0x07 0x11 + 119 luma bytes + xor"));
  Serial.println();
}

static void apply_luma() { scroll.fill_lamp(luma, LAMP_PIXELS, brightness); }

static void drop_pebble(float x, float y, const char *name) {
  mode = MODE_LAVA;
  ripples.drop(x, y, 1.0f);
  lamp.impulse(x, y, 2.2f);
  Serial.printf("PEBBLE %s\n", name);
  Serial.printf("pebble %s (%.1f, %.1f)\n", name, x, y);
}

static void render_lava() {
  lamp.render_luma(luma);
  ripples.apply(luma);
  apply_luma();
  scroll.show();
}

static bool checksum_ok(const uint8_t *p, int n) {
  uint8_t x = 0;
  for (int i = 0; i < n - 1; i++) {
    x ^= p[i];
  }
  return x == p[n - 1];
}

static void handle_packet() {
  if (pkt[0] != PKT_MAGIC0 || pkt[1] != PKT_MAGIC1) {
    return;
  }
  if (pkt[2] != LAMP_WIDTH || pkt[3] != LAMP_HEIGHT) {
    Serial.printf("bad size %ux%u, want %dx%d\n", pkt[2], pkt[3], LAMP_WIDTH, LAMP_HEIGHT);
    return;
  }
  if (!checksum_ok(pkt, PKT_LEN)) {
    Serial.println(F("bad checksum, dropping frame"));
    return;
  }
  memcpy(luma, pkt + 4, LAMP_PIXELS);
  mode = MODE_HOST;
  last_host_ms = millis();
  apply_luma();
  scroll.show();
}

static void feed_serial_byte(uint8_t b) {
  if (pkt_n == 0 && b != PKT_MAGIC0) {
    if (b == 'a' || b == 'A') {
      drop_pebble(corners[1].x, corners[1].y, corners[1].name);
    } else if (b == 'b' || b == 'B') {
      drop_pebble(corners[0].x, corners[0].y, corners[0].name);
    } else if (b == 'x' || b == 'X') {
      drop_pebble(corners[3].x, corners[3].y, corners[3].name);
    } else if (b == 'y' || b == 'Y') {
      drop_pebble(corners[2].x, corners[2].y, corners[2].name);
    } else if (b == 't' || b == 'T') {
      mode = MODE_TEST;
      Serial.println(F("test pattern"));
    } else if (b == 'l' || b == 'L') {
      mode = MODE_LAVA;
      paused = false;
      Serial.println(F("lava"));
    } else if (b == 'p' || b == 'P') {
      paused = !paused;
      Serial.println(paused ? F("paused") : F("running"));
    } else if (b == 's' || b == 'S') {
      lamp.reseed(millis());
      Serial.println(F("reseed"));
    } else if (b == '+' || b == '=') {
      if (brightness < MAX_BRIGHTNESS - 8) {
        brightness += 8;
      } else {
        brightness = MAX_BRIGHTNESS;
      }
      Serial.printf("brightness %u\n", brightness);
    } else if (b == '-' || b == '_') {
      if (brightness > MIN_BRIGHTNESS + 8) {
        brightness -= 8;
      } else {
        brightness = MIN_BRIGHTNESS;
      }
      Serial.printf("brightness %u\n", brightness);
    } else if (b == '?' || b == 'h') {
      print_help();
    }
    return;
  }

  if (pkt_n == 1 && b != PKT_MAGIC1) {
    pkt_n = 0;
    feed_serial_byte(b);
    return;
  }
  if (pkt_n < PKT_LEN) {
    pkt[pkt_n++] = b;
  }
  if (pkt_n == PKT_LEN) {
    handle_packet();
    pkt_n = 0;
  }
}

static bool pressed(int pin) { return digitalRead(pin) == LOW; }

static void handle_buttons() {
  uint32_t now = millis();
  for (CornerBtn &c : corners) {
    bool down = pressed(c.pin);
    if (down == c.was_pressed) {
      continue;
    }
    if (now - c.last_edge_ms < 70) {
      continue;
    }
    c.last_edge_ms = now;
    c.was_pressed = down;
    if (down) {
      drop_pebble(c.x, c.y, c.name);
    }
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  uint32_t wait_until = millis() + 2000;
  while (!Serial && millis() < wait_until) {
    delay(10);
  }

  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  pinMode(PIN_BTN_X, INPUT_PULLUP);
  pinMode(PIN_BTN_Y, INPUT_PULLUP);

  print_help();

  if (!scroll.begin(&Wire)) {
    Serial.println(F("IS31FL3731 not found at 0x74. Check wiring:"));
    Serial.println(F("  ESP32 3V3 -> PIM545 VSYS (pin 39)   NOT 5V"));
    Serial.println(F("  ESP32 GND -> PIM545 GND"));
    Serial.printf("  ESP32 GPIO%d -> PIM545 SDA (pin 6)\n", PIN_SDA);
    Serial.printf("  ESP32 GPIO%d -> PIM545 SCL (pin 7)\n", PIN_SCL);
    scan_i2c();
  } else {
    Serial.println(F("PIM545 ready."));
  }

  lamp.reseed(millis());
  if (pressed(PIN_BTN_A)) {
    mode = MODE_TEST;
    Serial.println(F("boot: test pattern (release A for lava, or send l)"));
  }
  last_frame_ms = millis();
}

void loop() {
  while (Serial.available()) {
    feed_serial_byte((uint8_t)Serial.read());
  }
  handle_buttons();

  uint32_t now = millis();
  if (mode == MODE_HOST && now - last_host_ms > HOST_TIMEOUT_MS) {
    mode = MODE_LAVA;
    Serial.println(F("host timed out; back to lava"));
  }

  uint32_t frame_ms = 1000 / TARGET_FPS;
  if (now - last_frame_ms < frame_ms) {
    delay(1);
    return;
  }
  last_frame_ms = now;

  if (!scroll.found()) {
    static uint32_t last_retry = 0;
    if (now - last_retry > 2000) {
      last_retry = now;
      if (scroll.begin(&Wire)) {
        Serial.println(F("PIM545 appeared."));
      }
    }
    return;
  }

  if (mode == MODE_TEST) {
    scroll.test_pattern(now);
    return;
  }
  if (mode == MODE_HOST) {
    return;
  }
  const float dt = 1.0f / TARGET_FPS;
  if (!paused) {
    lamp.step(dt);
  }
  ripples.step(dt, &lamp, !paused);
  render_lava();
}
