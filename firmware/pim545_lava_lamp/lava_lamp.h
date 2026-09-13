#pragma once

#include <stdint.h>
#include <vector>

#include "config.h"

// Heat-driven metaball lava lamp, ported from the Green Building client
// (17x9 RGB windows) onto this 7x17 white LED canvas.
class LavaLamp {
 public:
  explicit LavaLamp(uint32_t seed = 0);

  void reseed(uint32_t seed);
  void step(float dt);
  void render_rgb(uint8_t *rgb) const;
  void render_luma(uint8_t *luma) const;

 private:
  struct Blob {
    float x, y, vx, vy;
    float radius;
    float hue, hue_speed, phase, temp;
  };

  float ambient_temp(float y) const;
  float mass(const Blob &b) const { return b.radius * b.radius; }
  void spawn(int count);
  void integrate(Blob &b, float t, float dt);
  void repel(float dt);
  void bounce(Blob &b, float dt);
  void try_merges();
  void try_splits(float dt);
  void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b) const;
  float mix_hue(float h1, float w1, float h2, float w2) const;

  uint32_t seed_;
  uint32_t rng_state_;
  float t_;
  std::vector<Blob> blobs_;

  uint32_t rng_u32();
  float rng_float();
  float rng_uniform(float lo, float hi);
};
