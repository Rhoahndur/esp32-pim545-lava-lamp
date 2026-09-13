#include <Arduino.h>
#include <lgfx_user/LGFX_Waveshare_ESP32S3_Touch_LCD_7.h>
#include "../pim545_lava_lamp/lava_lamp.h"
#include "../pim545_lava_lamp/ripple.h"

static LGFX display;
static LavaLamp lamp;
static Ripples ripples;
static uint8_t rgb[LAMP_PIXELS * 3];
static bool ready = false;
static bool touching = false;
static uint32_t last_frame = 0;
struct Button { int x, y; char name; float px, py; };
static const Button buttons[] = {
  {30, 65, 'A', 0.3f, 0.3f}, {610, 65, 'B', 7.8f, 0.3f},
  {30, 335, 'X', 0.3f, 15.7f}, {610, 335, 'Y', 7.8f, 15.7f}
};

static void identify() {
  Serial.println("LAVA_CONTROLLER 1 WAVESHARE_TOUCH_7 corners");
}

static void drop(const Button &b) {
  ripples.drop(b.px, b.py);
  lamp.impulse(b.px, b.py, 2.2f);
  Serial.printf("PEBBLE %c\n", b.name);
}

void setup() {
  // Use the board's USB-to-UART connector; no native-USB mux changes.
  Serial.begin(115200);
  delay(400);
  identify();
  if (!psramFound()) {
    Serial.println("ERROR: PSRAM missing; use waveshare-touch-7 build with OPI PSRAM.");
    return;
  }
  if (!display.init()) {
    Serial.println("ERROR: LCD initialization failed; check board model and power.");
    return;
  }
  display.setRotation(0);
  display.fillScreen(0x0821);
  display.setTextColor(0xFFFF);
  display.setTextSize(2);
  display.setCursor(260, 8);
  display.print("GREEN BUILDING LAVA");
  for (const Button &b : buttons) {
    display.fillRoundRect(b.x, b.y, 160, 80, 12, 0x2949);
    display.setCursor(b.x + 66, b.y + 30);
    display.printf("%c", b.name);
  }
  display.setTextSize(1);
  display.setCursor(235, 466);
  display.print("Tap a corner | USB bridge sends taps to simulator");
  lamp.reseed(micros());
  ready = true;
  Serial.println("Touchscreen ready. Send ? to identify; a/b/x/y to tap.");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '?') identify();
    for (const Button &b : buttons) {
      if (c == b.name || c == b.name + ('a' - 'A')) drop(b);
    }
  }
  if (!ready) { delay(20); return; }
  uint16_t x, y;
  bool down = display.getTouch(&x, &y);
  if (down && !touching) {
    for (const Button &b : buttons) {
      if (x >= b.x && x < b.x + 160 && y >= b.y && y < b.y + 80) drop(b);
    }
  }
  touching = down;
  uint32_t now = millis();
  if (now - last_frame >= 50) {
    last_frame = now;
    lamp.step(0.05f);
    ripples.step(0.05f, &lamp, true);
    lamp.render_rgb(rgb);
    // Apply the existing signed ripple modulation to each RGB channel.
    uint8_t channel[LAMP_PIXELS];
    for (int c = 0; c < 3; ++c) {
      for (int i = 0; i < LAMP_PIXELS; ++i) channel[i] = rgb[3*i+c];
      ripples.apply(channel);
      for (int i = 0; i < LAMP_PIXELS; ++i) rgb[3*i+c] = channel[i];
    }
    for (int y = 0; y < 17; ++y) {
      for (int x = 0; x < 9; ++x) {
        int i = (y * 9 + x) * 3;
        display.fillRect(292 + x * 24, 40 + y * 24, 20, 20,
                         display.color565(rgb[i], rgb[i+1], rgb[i+2]));
      }
    }
  }
  delay(2);
}
