#include "lava_lamp.h"

#include <math.h>
#include <algorithm>

namespace {

const int START_BLOBS = 6;
const int MIN_BLOBS = 5;
const int MAX_BLOBS = 7;
const float MIN_RADIUS = 1.6f;
const float MAX_RADIUS = 4.2f;
const float X_MIN = 0.3f;
const float X_MAX = (float)LAMP_WIDTH - 1.3f;
const float Y_MIN = 0.3f;
const float Y_MAX = (float)LAMP_HEIGHT - 1.3f;
const float MAX_SPEED = 1.8f;
const float RESTITUTION = 0.85f;
const float NEUTRAL_TEMP = 0.5f;
const float HEAT_RATE = 0.22f;
const float BUOYANCY = 3.4f;
const float DRAG = 0.55f;
const float WANDER = 0.9f;
const float WANDER_Y = 0.28f;
const float REPEL_REACH = 0.55f;
const float REPEL_STRENGTH = 1.1f;
const float MERGE_FACTOR = 0.32f;
const float SPLIT_RADIUS = 3.15f;
const float SPLIT_TEMP = 0.68f;
const float SPLIT_RATE = 0.35f;
const float HUE_SPREAD = 0.62f;
const float HUE_DRIFT = 0.033f;
const float HUE_SPEED_JITTER = 0.024f;
const float SAT_BASE = 0.82f;
const float SAT_TEMP = 0.18f;
const float VAL_BASE = 0.65f;
const float VAL_TEMP = 0.35f;
const float Y_ASPECT = 1.0f;
const float GAUSS_FALLOFF = 2.5f;
const float GLOW_GAMMA = 1.35f;
const float TAU = 6.283185307179586f;
const uint8_t BACKGROUND_R = 5;
const uint8_t BACKGROUND_G = 2;
const uint8_t BACKGROUND_B = 15;

float clampf(float v, float lo, float hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

}  // namespace

LavaLamp::LavaLamp(uint32_t seed) { reseed(seed); }

void LavaLamp::reseed(uint32_t seed) {
  seed_ = seed;
  rng_state_ = seed ? seed : 0xA341316Cu;
  t_ = 0.0f;
  blobs_.clear();
  spawn(START_BLOBS);
}

uint32_t LavaLamp::rng_u32() {
  // xorshift32
  uint32_t x = rng_state_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state_ = x ? x : 1;
  return rng_state_;
}

float LavaLamp::rng_float() { return (rng_u32() >> 8) * (1.0f / 16777216.0f); }

float LavaLamp::rng_uniform(float lo, float hi) {
  return lo + (hi - lo) * rng_float();
}

float LavaLamp::ambient_temp(float y) const {
  float span = Y_MAX - Y_MIN;
  float ny = span <= 0.0f ? 0.0f : (y - Y_MIN) / span;
  ny = clampf(ny, 0.0f, 1.0f);
  return 0.08f + 0.92f * ny;
}

void LavaLamp::spawn(int count) {
  float start_hue = rng_float();
  for (int i = 0; i < count; i++) {
    Blob b;
    b.y = rng_uniform(Y_MIN + 0.8f, Y_MAX - 0.8f);
    b.x = rng_uniform(X_MIN + 0.8f, X_MAX - 0.8f);
    b.vx = rng_uniform(-0.45f, 0.45f);
    b.vy = rng_uniform(-0.65f, 0.65f);
    b.radius = rng_uniform(2.0f, 3.4f);
    b.hue = fmodf(start_hue + (float)i / (float)std::max(count, 1) * HUE_SPREAD, 1.0f);
    if (b.hue < 0) {
      b.hue += 1.0f;
    }
    b.hue_speed = HUE_DRIFT + rng_uniform(-HUE_SPEED_JITTER, HUE_SPEED_JITTER);
    b.phase = rng_uniform(0.0f, TAU);
    b.temp = clampf(ambient_temp(b.y) + rng_uniform(-0.15f, 0.15f), 0.0f, 1.0f);
    blobs_.push_back(b);
  }
}

void LavaLamp::hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b) const {
  if (s <= 0.0f) {
    *r = *g = *b = v;
    return;
  }
  h = fmodf(h, 1.0f);
  if (h < 0) {
    h += 1.0f;
  }
  float i;
  float f = modff(h * 6.0f, &i);
  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));
  int hi = ((int)i) % 6;
  if (hi < 0) {
    hi += 6;
  }
  switch (hi) {
    case 0:
      *r = v;
      *g = t;
      *b = p;
      break;
    case 1:
      *r = q;
      *g = v;
      *b = p;
      break;
    case 2:
      *r = p;
      *g = v;
      *b = t;
      break;
    case 3:
      *r = p;
      *g = q;
      *b = v;
      break;
    case 4:
      *r = t;
      *g = p;
      *b = v;
      break;
    default:
      *r = v;
      *g = p;
      *b = q;
      break;
  }
}

float LavaLamp::mix_hue(float h1, float w1, float h2, float w2) const {
  float x = w1 * cosf(h1 * TAU) + w2 * cosf(h2 * TAU);
  float y = w1 * sinf(h1 * TAU) + w2 * sinf(h2 * TAU);
  float h = atan2f(y, x) / TAU;
  h = fmodf(h, 1.0f);
  if (h < 0) {
    h += 1.0f;
  }
  return h;
}

void LavaLamp::integrate(Blob &b, float t, float dt) {
  b.temp += (ambient_temp(b.y) - b.temp) * std::min(1.0f, HEAT_RATE * dt);
  b.temp = clampf(b.temp, 0.0f, 1.0f);
  b.hue = fmodf(b.hue + b.hue_speed * dt, 1.0f);
  if (b.hue < 0) {
    b.hue += 1.0f;
  }
  b.vx += sinf(t * 0.73f + b.phase) * WANDER * dt;
  b.vy += sinf(t * 0.41f + b.phase * 1.3f) * WANDER_Y * dt;
  b.vy += (NEUTRAL_TEMP - b.temp) * BUOYANCY * dt;
  b.vx -= b.vx * DRAG * dt;
  b.vy -= b.vy * DRAG * dt;
}

void LavaLamp::repel(float dt) {
  for (size_t i = 0; i < blobs_.size(); i++) {
    for (size_t j = i + 1; j < blobs_.size(); j++) {
      Blob &a = blobs_[i];
      Blob &b = blobs_[j];
      float dx = b.x - a.x;
      float dy = b.y - a.y;
      float dist = hypotf(dx, dy);
      float reach = (a.radius + b.radius) * REPEL_REACH;
      if (dist > 0.0f && dist < reach) {
        float force = (reach - dist) * REPEL_STRENGTH * dt;
        float ux = dx / dist;
        float uy = dy / dist;
        a.vx -= force * ux;
        a.vy -= force * uy;
        b.vx += force * ux;
        b.vy += force * uy;
      }
    }
  }
}

void LavaLamp::bounce(Blob &b, float dt) {
  b.vx = clampf(b.vx, -MAX_SPEED, MAX_SPEED);
  b.vy = clampf(b.vy, -MAX_SPEED, MAX_SPEED);
  b.x += b.vx * dt;
  b.y += b.vy * dt;
  if (b.x < X_MIN || b.x > X_MAX) {
    b.x = clampf(b.x, X_MIN, X_MAX);
    b.vx *= -RESTITUTION;
  }
  if (b.y < Y_MIN || b.y > Y_MAX) {
    b.y = clampf(b.y, Y_MIN, Y_MAX);
    b.vy *= -RESTITUTION;
  }
  b.radius = clampf(b.radius, MIN_RADIUS, MAX_RADIUS);
}

void LavaLamp::try_merges() {
  while ((int)blobs_.size() > MIN_BLOBS) {
    int best_i = -1;
    int best_j = -1;
    float best_dist = 0.0f;
    for (size_t i = 0; i < blobs_.size(); i++) {
      for (size_t j = i + 1; j < blobs_.size(); j++) {
        const Blob &a = blobs_[i];
        const Blob &b = blobs_[j];
        float dist = hypotf(b.x - a.x, b.y - a.y);
        if (dist < MERGE_FACTOR * (a.radius + b.radius)) {
          if (best_i < 0 || dist < best_dist) {
            best_i = (int)i;
            best_j = (int)j;
            best_dist = dist;
          }
        }
      }
    }
    if (best_i < 0) {
      break;
    }
    Blob a = blobs_[(size_t)best_i];
    Blob b = blobs_[(size_t)best_j];
    float m1 = mass(a);
    float m2 = mass(b);
    float total = m1 + m2;
    Blob merged;
    merged.x = (a.x * m1 + b.x * m2) / total;
    merged.y = (a.y * m1 + b.y * m2) / total;
    merged.vx = (a.vx * m1 + b.vx * m2) / total;
    merged.vy = (a.vy * m1 + b.vy * m2) / total;
    merged.radius = std::min(MAX_RADIUS, sqrtf(total));
    merged.hue = mix_hue(a.hue, m1, b.hue, m2);
    merged.hue_speed = (a.hue_speed * m1 + b.hue_speed * m2) / total;
    merged.phase = a.phase;
    merged.temp = (a.temp * m1 + b.temp * m2) / total;
    if (best_j > best_i) {
      blobs_.erase(blobs_.begin() + best_j);
      blobs_.erase(blobs_.begin() + best_i);
    } else {
      blobs_.erase(blobs_.begin() + best_i);
      blobs_.erase(blobs_.begin() + best_j);
    }
    blobs_.push_back(merged);
  }
}

void LavaLamp::try_splits(float dt) {
  std::vector<Blob> out;
  std::vector<Blob> remaining = blobs_;
  while (!remaining.empty()) {
    Blob blob = remaining.back();
    remaining.pop_back();
    int live = (int)out.size() + 1 + (int)remaining.size();
    if (live < MAX_BLOBS && blob.radius > SPLIT_RADIUS && blob.temp > SPLIT_TEMP &&
        rng_float() < SPLIT_RATE * dt) {
      float angle = rng_uniform(0.0f, TAU);
      float offset = blob.radius * 0.35f;
      float radius = std::max(MIN_RADIUS, blob.radius / sqrtf(2.0f));
      for (int sign : {-1, 1}) {
        Blob child = blob;
        child.x = blob.x + (float)sign * cosf(angle) * offset;
        child.y = blob.y + (float)sign * sinf(angle) * offset;
        child.vx = blob.vx + (float)sign * cosf(angle) * 0.3f;
        child.vy = blob.vy + (float)sign * sinf(angle) * 0.3f;
        child.radius = radius;
        child.hue = fmodf(blob.hue + (float)sign * rng_uniform(0.06f, 0.14f), 1.0f);
        if (child.hue < 0) {
          child.hue += 1.0f;
        }
        child.hue_speed = blob.hue_speed + (float)sign * rng_uniform(0.002f, 0.01f);
        child.phase = blob.phase + (float)sign * 0.7f;
        bounce(child, 0.0f);
        out.push_back(child);
      }
    } else {
      out.push_back(blob);
    }
    if ((int)out.size() + (int)remaining.size() >= MAX_BLOBS) {
      out.insert(out.end(), remaining.begin(), remaining.end());
      break;
    }
  }
  blobs_.swap(out);
}

void LavaLamp::step(float dt) {
  for (Blob &b : blobs_) {
    integrate(b, t_, dt);
  }
  repel(dt);
  for (Blob &b : blobs_) {
    bounce(b, dt);
  }
  try_merges();
  try_splits(dt);
  t_ += dt;
}

void LavaLamp::render_rgb(uint8_t *rgb) const {
  const int n = (int)blobs_.size();
  float cr[8], cg[8], cb[8];
  int used = n < 8 ? n : 8;
  for (int i = 0; i < used; i++) {
    const Blob &b = blobs_[(size_t)i];
    float s = clampf(SAT_BASE - SAT_TEMP * b.temp, 0.0f, 1.0f);
    float v = clampf(VAL_BASE + VAL_TEMP * b.temp, 0.0f, 1.0f);
    hsv_to_rgb(b.hue, s, v, &cr[i], &cg[i], &cb[i]);
  }

  int o = 0;
  for (int y = 0; y < LAMP_HEIGHT; y++) {
    for (int x = 0; x < LAMP_WIDTH; x++) {
      float weights[8];
      float total = 0.0f;
      for (int i = 0; i < used; i++) {
        const Blob &b = blobs_[(size_t)i];
        float dx = (float)x - b.x;
        float dy = ((float)y - b.y) * Y_ASPECT;
        float w = expf(-GAUSS_FALLOFF * (dx * dx + dy * dy) / (b.radius * b.radius));
        weights[i] = w;
        total += w;
      }
      float glow = total;
      if (glow < 0.0f) {
        glow = 0.0f;
      }
      glow = powf(glow, GLOW_GAMMA);
      if (glow > 1.0f) {
        glow = 1.0f;
      }
      float inv = 1.0f / (total > 1e-12f ? total : 1e-12f);
      float r = 0, g = 0, b = 0;
      for (int i = 0; i < used; i++) {
        r += weights[i] * cr[i];
        g += weights[i] * cg[i];
        b += weights[i] * cb[i];
      }
      r *= inv;
      g *= inv;
      b *= inv;
      rgb[o++] = (uint8_t)lroundf((float)BACKGROUND_R * (1.0f - glow) + 255.0f * r * glow);
      rgb[o++] = (uint8_t)lroundf((float)BACKGROUND_G * (1.0f - glow) + 255.0f * g * glow);
      rgb[o++] = (uint8_t)lroundf((float)BACKGROUND_B * (1.0f - glow) + 255.0f * b * glow);
    }
  }
}

void LavaLamp::render_luma(uint8_t *luma) const {
  uint8_t rgb[LAMP_PIXELS * 3];
  render_rgb(rgb);
  for (int i = 0; i < LAMP_PIXELS; i++) {
    float y = 0.2126f * rgb[i * 3] + 0.7152f * rgb[i * 3 + 1] + 0.0722f * rgb[i * 3 + 2];
    if (y < 0) {
      y = 0;
    }
    if (y > 255) {
      y = 255;
    }
    luma[i] = (uint8_t)lroundf(y);
  }
}
