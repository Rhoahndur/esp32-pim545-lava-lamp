#include "ripple.h"

#include <math.h>
#include <algorithm>

const float Ripples::SPEED = 12.0f;
const float Ripples::THICK = 0.85f;
const float Ripples::WAVE_K = 2.6f;
const float Ripples::TRAIL = 3.4f;
const float Ripples::DECAY = 0.72f;
const float Ripples::MAX_AGE = 2.6f;

void Ripples::drop(float x, float y, float amp) {
  int slot = 0;
  float oldest = -1.0f;
  for (int i = 0; i < MAX; i++) {
    if (!drops_[i].live) {
      slot = i;
      oldest = 1e9f;
      break;
    }
    if (drops_[i].t > oldest) {
      oldest = drops_[i].t;
      slot = i;
    }
  }
  drops_[slot].live = true;
  drops_[slot].x = x;
  drops_[slot].y = y;
  drops_[slot].t = 0.0f;
  drops_[slot].amp = amp;
}

void Ripples::step(float dt, LavaLamp *lamp, bool move_blobs) {
  for (int i = 0; i < MAX; i++) {
    Drop &d = drops_[i];
    if (!d.live) {
      continue;
    }
    d.t += dt;
    if (d.t > MAX_AGE) {
      d.live = false;
      continue;
    }
    if (move_blobs && lamp != nullptr) {
      float front = SPEED * d.t;
      float damp = expf(-DECAY * d.t);
      lamp->wave_force(d.x, d.y, front, d.amp * damp, dt);
    }
  }
}

bool Ripples::active() const {
  for (int i = 0; i < MAX; i++) {
    if (drops_[i].live) {
      return true;
    }
  }
  return false;
}

float Ripples::height_at(float px, float py) const {
  float h = 0.0f;
  for (int i = 0; i < MAX; i++) {
    const Drop &d = drops_[i];
    if (!d.live) {
      continue;
    }
    float dx = px - d.x;
    float dy = py - d.y;
    float r = sqrtf(dx * dx + dy * dy);
    float t = d.t;
    float front = SPEED * t;
    float damp = expf(-DECAY * t);

    float dr = r - front;
    float ring = expf(-(dr * dr) / (2.0f * THICK * THICK));

    float trail = 0.0f;
    if (r < front) {
      float behind = front - r;
      trail = sinf(WAVE_K * behind) * expf(-behind / TRAIL);
    }

    float splash = expf(-t / 0.11f) * expf(-(r * r) / 0.50f);
    h += d.amp * damp * (1.20f * ring + 0.42f * trail + 0.90f * splash);
  }
  return h;
}

void Ripples::apply(uint8_t *luma) const {
  if (!active()) {
    return;
  }
  for (int y = 0; y < LAMP_HEIGHT; y++) {
    for (int x = 0; x < LAMP_WIDTH; x++) {
      float h = height_at((float)x, (float)y);
      if (h == 0.0f) {
        continue;
      }
      int i = y * LAMP_WIDTH + x;
      int v = (int)luma[i] + (int)lroundf(h * 220.0f);
      if (v < 0) {
        v = 0;
      }
      if (v > 255) {
        v = 255;
      }
      luma[i] = (uint8_t)v;
    }
  }
}
