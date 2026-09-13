#pragma once

// Board selector. Override with -D PIM545_BOARD=N in platformio.ini,
// or edit the number below for the Arduino IDE.
//
//   1 = ESP32 DevKit / WROOM          SDA=21 SCL=22
//   2 = ESP32-S3 DevKitC and similar  SDA=8  SCL=9
//   3 = ESP32-C3 SuperMini            SDA=8  SCL=9
#ifndef PIM545_BOARD
#define PIM545_BOARD 1
#endif

#if PIM545_BOARD == 2
#ifndef PIN_SDA
#define PIN_SDA 8
#endif
#ifndef PIN_SCL
#define PIN_SCL 9
#endif
#ifndef PIN_BTN_A
#define PIN_BTN_A 4
#endif
#ifndef PIN_BTN_B
#define PIN_BTN_B 5
#endif
#ifndef PIN_BTN_X
#define PIN_BTN_X 6
#endif
#ifndef PIN_BTN_Y
#define PIN_BTN_Y 7
#endif

#elif PIM545_BOARD == 3
#ifndef PIN_SDA
#define PIN_SDA 8
#endif
#ifndef PIN_SCL
#define PIN_SCL 9
#endif
#ifndef PIN_BTN_A
#define PIN_BTN_A 2
#endif
#ifndef PIN_BTN_B
#define PIN_BTN_B 3
#endif
#ifndef PIN_BTN_X
#define PIN_BTN_X 4
#endif
#ifndef PIN_BTN_Y
#define PIN_BTN_Y 5
#endif

#else
#ifndef PIN_SDA
#define PIN_SDA 21
#endif
#ifndef PIN_SCL
#define PIN_SCL 22
#endif
#ifndef PIN_BTN_A
#define PIN_BTN_A 32
#endif
#ifndef PIN_BTN_B
#define PIN_BTN_B 33
#endif
#ifndef PIN_BTN_X
#define PIN_BTN_X 25
#endif
#ifndef PIN_BTN_Y
#define PIN_BTN_Y 26
#endif
#endif

// IS31FL3731 on the Pico Scroll Pack.
static const uint8_t PIM545_I2C_ADDR = 0x74;
static const uint32_t I2C_HZ = 400000;

// Lamp canvas: Green Building analog, 7 windows by 17 floors.
#ifdef LAVA_TOUCHSCREEN
static const int LAMP_WIDTH = 9;
#else
static const int LAMP_WIDTH = 7;
#endif
static const int LAMP_HEIGHT = 17;
static const int LAMP_PIXELS = LAMP_WIDTH * LAMP_HEIGHT;

// Native Pico Scroll Pack geometry (landscape).
static const int SCROLL_WIDTH = 17;
static const int SCROLL_HEIGHT = 7;

// Portrait mapping: hold the pack with buttons A/B at the roof (top)
// and X/Y at the ground (bottom). Flip these if the image is mirrored
// or if you mount the pack the other way up.
#ifndef SWAP_XY
#define SWAP_XY 1
#endif
#ifndef FLIP_X
#define FLIP_X 0
#endif
#ifndef FLIP_Y
#define FLIP_Y 0
#endif

static const int TARGET_FPS = 20;
static const uint8_t DEFAULT_BRIGHTNESS = 140;
static const uint8_t MIN_BRIGHTNESS = 16;
static const uint8_t MAX_BRIGHTNESS = 255;
static const uint32_t HOST_TIMEOUT_MS = 2500;
static const uint32_t SERIAL_BAUD = 115200;
