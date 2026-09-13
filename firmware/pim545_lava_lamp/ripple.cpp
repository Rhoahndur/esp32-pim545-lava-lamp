#include "ripple.h"

#include <math.h>

#include "config.h"
#include "lava_lamp.h"

namespace {

const float kPi = 3.14159265f;
const float kC = 10.0f;          // ring speed, pixels / s
const float kLambda = 5.4f;      // bright ring, then a trough inside it
const float kK = 2.0f * kPi / kLambda;
const float kSigma = 1.15f;      // tight packet so a new drop does not sit on the last ring
const float kGamma = 0.32f;      // lives long enough to cross the board
const float kR0 = 1.35f;         // mild 1/√r so the ring stays visible

}  // namespace

const float Ripples::MAX_AGE = 2.4f;

WaveAmp circular_wave(float r, float t, float amp) {
  WaveAmp out = {0.0f, 0.0f};
  if (t < 0.0f || amp == 0.0f) {
    return out;
  }

  // Packet locked to the wavefront r = c t. At t = 0 this is a peak
  // at the pebble, not an immediate trough on the next pixel.
  float psi = r - kC * t;
  float s2 = kSigma * kSigma;
  float env = expf(-(psi * psi) / (2.0f * s2));
  float spread = 1.0f / sqrtf(0.35f * r + kR0);
  float tdamp = expf(-kGamma * t);
  float A = amp * tdamp * spread * env;
  float phase = kK * psi;
  float cosp = cosf(phase);
  float sinp = sinf(phase);
  out.h = A * cosp;

  float d_env_dr = env * (-psi / s2);
  float d_spread_dr = -0.5f * 0.35f * spread / (0.35f * r + kR0);
  float dA_dr = amp * tdamp * (d_spread_dr * env + spread * d_env_dr);
  out.dhdr = dA_dr * cosp - A * kK * sinp;
  return out;
}

void Ripples::drop(float x, float y, float amp) {
  // Always add a new ring. Never rewind an existing one.
  int slot = -1;
  int oldest_i = 0;
  float oldest_t = -1.0f;
  for (int i = 0; i < MAX; i++) {
    if (!drops_[i].live) {
      slot = i;
      break;
    }
    if (drops_[i].t > oldest_t) {
      oldest_t = drops_[i].t;
      oldest_i = i;
    }
  }
  if (slot < 0) {
    slot = oldest_i;
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
      lamp->wave_force(d.x, d.y, d.t, d.amp, dt);
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
    h += circular_wave(r, d.t, d.amp).h;
  }
  return h;
}

void Ripples::apply(uint8_t *luma) const {
  if (!active()) {
    return;
  }
  for (int y = 0; y < LAMP_HEIGHT; y++) {
    for (int x = 0; x < LAMP_WIDTH; x++) {
      int add = (int)lroundf(height_at((float)x, (float)y) * 230.0f);
      if (add > 368) {
        add = 368;
      } else if (add < -368) {
        add = -368;
      }
      if (add == 0) {
        continue;
      }
      int i = y * LAMP_WIDTH + x;
      int v = (int)luma[i] + add;
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

void Ripples::apply_rgb(uint8_t *rgb) const {
  if (!active()) {
    return;
  }
  for (int y = 0; y < LAMP_HEIGHT; y++) {
    for (int x = 0; x < LAMP_WIDTH; x++) {
      int add = (int)lroundf(height_at((float)x, (float)y) * 230.0f);
      if (add > 368) {
        add = 368;
      } else if (add < -368) {
        add = -368;
      }
      if (add == 0) {
        continue;
      }
      int i = (y * LAMP_WIDTH + x) * 3;
      for (int c = 0; c < 3; c++) {
        int v = (int)rgb[i + c] + add;
        if (v < 0) {
          v = 0;
        }
        if (v > 255) {
          v = 255;
        }
        rgb[i + c] = (uint8_t)v;
      }
    }
  }
}
