#pragma once

#include "lava_lamp.h"

// Expanding pond ripples. Origins are in lamp pixels (7x17).
class Ripples {
 public:
  void drop(float x, float y, float amp = 1.0f);
  void step(float dt, LavaLamp *lamp, bool move_blobs);
  void apply(uint8_t *luma) const;
  bool active() const;

 private:
  static const int MAX = 6;
  static const float SPEED;      // pixels / second
  static const float THICK;      // ring half-width
  static const float WAVE_K;     // trailing ripples
  static const float TRAIL;      // how far behind the front they last
  static const float DECAY;      // amplitude e-folding
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
