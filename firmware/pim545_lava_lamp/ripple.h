#pragma once

#include <stdint.h>

class LavaLamp;

// Signed circular wave (peak +, trough -). Superpose by adding heights.
struct WaveAmp {
  float h;
  float dhdr;
};

WaveAmp circular_wave(float r, float t, float amp);

class Ripples {
 public:
  void drop(float x, float y, float amp = 1.0f);
  void step(float dt, LavaLamp *lamp, bool move_blobs);
  void apply(uint8_t *luma) const;
  void apply_rgb(uint8_t *rgb) const;
  bool active() const;

 private:
  static const int MAX = 16;
  static const float MAX_AGE;

  struct Drop {
    bool live;
    float x, y;
    float t;
    float amp;
  };

  Drop drops_[MAX] = {};
  float height_at(float px, float py) const;
};
