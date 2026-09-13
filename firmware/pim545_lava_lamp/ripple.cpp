#include "ripple.h"

#include <math.h>

#include "config.h"
#include "lava_lamp.h"

namespace {

const float kPi = 3.14159265f;
const float kC = 11.5f;                         // phase speed, pixels / s
const float kLambda = 2.7f;                     // wavelength, pixels
const float kK = 2.0f * kPi / kLambda;
const float kOmega = kK * kC;                   // non-dispersive: ω = c k
const float kSigma = 0.90f;                     // front softness
const float kWake = 11.0f;                      // trailing train length
const float kGamma = 0.48f;                     // time damping
const float kR0 = 0.70f;                        // cylindrical spreading floor

}  // namespace

const float Ripples::MAX_AGE = 3.4f;

WaveAmp circular_wave(float r, float t, float amp) {
  WaveAmp out = {0.0f, 0.0f};
  if (t < 0.0f || amp == 0.0f) {
    return out;
  }

  float front = kC * t;
  float ahead = r - front;

  float env;
  float d_env_dr;
  if (ahead >= 0.0f) {
    float s2 = kSigma * kSigma;
    env = expf(-(ahead * ahead) / (2.0f * s2));
    d_env_dr = env * (-ahead / s2);
  } else {
    env = expf(ahead / kWake);
    d_env_dr = env / kWake;
  }

  float spread = 1.0f / sqrtf(r + kR0);
  float tdamp = expf(-kGamma * t);
  float A = amp * tdamp * spread * env;
  float phase = kK * r - kOmega * t;
  float cosp = cosf(phase);
  float sinp = sinf(phase);

  out.h = A * cosp;

  // H = A(r) cos(kr − ωt). Keep the 1/√r and envelope derivatives so
  // the slope that shoves blobs matches the picture on the LEDs.
  float dA_dr = amp * tdamp * ((-0.5f) * spread / (r + kR0) * env + spread * d_env_dr);
  out.dhdr = dA_dr * cosp - A * kK * sinp;
  return out;
}

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
      float h = height_at((float)x, (float)y);
      if (h > 1.8f) {
        h = 1.8f;
      } else if (h < -1.8f) {
        h = -1.8f;
      }
      if (h == 0.0f) {
        continue;
      }
      int i = y * LAMP_WIDTH + x;
      int v = (int)luma[i] + (int)lroundf(h * 200.0f);
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
