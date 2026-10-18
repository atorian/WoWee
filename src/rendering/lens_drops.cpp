#include "rendering/lens_drops.hpp"

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

namespace {

// How many arrive a second with everything at full: the weather full, the
// camera out under the sky and the lens facing into it.
constexpr float kRainPerSecond = 16.0f;
constexpr float kFlakesPerSecond = 9.0f;
constexpr float kGritPerSecond = 140.0f;

/// Under a roof the lens dries this many times as fast as in the open.
constexpr float kShelteredDrying = 4.0f;

/// A running drop speeds up to this, in screen heights a second.
constexpr float kRunAcceleration = 0.25f;
constexpr float kRunTopSpeed = 0.45f;
/// And thins as it goes, leaving its water behind.
constexpr float kRunLossPerSecond = 0.02f;

/// Half a minute of heavy snow in the open to frost it fully; it thaws in
/// twenty seconds out of it.
constexpr float kFrostPerSecond = 0.033f;
constexpr float kFrostThawPerSecond = 0.05f;
constexpr float kDustPerSecond = 0.1f;
constexpr float kDustClearPerSecond = 0.025f;

float smooth01(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

float LensDrop::opacity() const {
    const float t = life > 0.0f ? age / life : 1.0f;
    switch (kind) {
        case Kind::Water: return smooth01(0.0f, 0.15f, age) * (1.0f - smooth01(0.7f, 1.0f, t));
        // Whole until it has melted: what is left goes on as water.
        case Kind::Flake: return smooth01(0.0f, 0.1f, age);
        case Kind::Grit: return 1.0f - t;
    }
    return 0.0f;
}

float LensDrops::random() {
    // xorshift32: small, and the same sequence on every machine.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
}

int LensDrops::spawnCount(float perSecond, float dt, float& carry) {
    carry += std::max(perSecond, 0.0f) * dt;
    const int n = static_cast<int>(carry);
    carry -= static_cast<float>(n);
    return n;
}

void LensDrops::clear() {
    drops_.clear();
    frost_ = 0.0f;
    dust_ = 0.0f;
    rainCarry_ = snowCarry_ = gritCarry_ = 0.0f;
}

void LensDrops::spawnWater(glm::vec2 pos, float radius, float life) {
    if (drops_.size() >= kMaxDrops) return;
    LensDrop d;
    d.kind = LensDrop::Kind::Water;
    d.pos = pos;
    d.radius = radius;
    d.life = life;
    d.seed = random();
    d.trailTimer = random(0.08f, 0.25f);
    drops_.push_back(d);
}

void LensDrops::spawnFlake() {
    if (drops_.size() >= kMaxDrops) return;
    LensDrop d;
    d.kind = LensDrop::Kind::Flake;
    d.pos = {random(), random(-0.02f, 1.0f)};
    d.radius = random(0.008f, 0.024f);
    d.life = random(1.8f, 4.0f);
    d.seed = random();
    drops_.push_back(d);
}

void LensDrops::spawnGrit(const LensInputs& in) {
    if (drops_.size() >= kMaxDrops) return;
    LensDrop d;
    d.kind = LensDrop::Kind::Grit;
    d.pos = {random(), random()};
    d.radius = random(0.0015f, 0.004f);
    d.life = random(0.08f, 0.22f);
    d.velocity = in.windOnScreen * random(0.8f, 1.6f) + glm::vec2(random(-0.05f, 0.05f), random(-0.05f, 0.05f));
    d.seed = random();
    drops_.push_back(d);
}

void LensDrops::mergeWater(float aspect) {
    // Two drops that touch run together into one holding the water of both.
    for (size_t a = 0; a < drops_.size(); ++a) {
        LensDrop& da = drops_[a];
        if (da.kind != LensDrop::Kind::Water) continue;
        for (size_t b = a + 1; b < drops_.size();) {
            LensDrop& db = drops_[b];
            const glm::vec2 apart((db.pos.x - da.pos.x) * aspect, db.pos.y - da.pos.y);
            if (db.kind != LensDrop::Kind::Water || glm::length(apart) >= (da.radius + db.radius) * 0.8f) {
                ++b;
                continue;
            }
            const float wa = da.radius * da.radius;
            const float wb = db.radius * db.radius;
            da.pos = (da.pos * wa + db.pos * wb) / (wa + wb);
            da.radius = std::sqrt(wa + wb);
            da.velocity = glm::max(da.velocity, db.velocity);
            da.age = std::min(da.age, db.age);
            da.life = std::max(da.life, db.life);
            db = drops_.back();
            drops_.pop_back();
        }
    }
}

void LensDrops::update(float dt, const LensInputs& in, float aspect) {
    dt = std::max(dt, 0.0f);
    aspect = std::max(aspect, 0.1f);
    if (in.underwater) {
        // Under the surface the lens is all water; nothing on it shows.
        clear();
        wasUnderwater_ = true;
        return;
    }
    if (wasUnderwater_) {
        // Up out of the water: it runs off the lens in sheets.
        wasUnderwater_ = false;
        for (int i = 0; i < 36; ++i) {
            spawnWater({random(), random(-0.05f, 0.9f)}, random(0.018f, 0.035f), random(2.5f, 4.0f));
            drops_.back().velocity.y = random(0.25f, 0.6f);
        }
    }

    const float exposure = std::clamp(in.exposure, 0.0f, 1.0f);
    const float facingFall = std::clamp(in.facingFall, 0.0f, 1.0f);
    const float facingWind = std::clamp(in.facingWind, 0.0f, 1.0f);

    // What arrives.
    for (int n = spawnCount(kRainPerSecond * in.rain * exposure * facingFall, dt, rainCarry_); n > 0; --n) {
        const float r = random();
        spawnWater({random(), random(-0.02f, 1.0f)}, 0.004f + 0.02f * std::pow(r, 2.5f), random(5.0f, 10.0f));
    }
    for (int n = spawnCount(kFlakesPerSecond * in.snow * exposure * facingFall, dt, snowCarry_); n > 0; --n) {
        spawnFlake();
    }
    for (int n = spawnCount(kGritPerSecond * in.sand * exposure * facingWind, dt, gritCarry_); n > 0; --n) {
        spawnGrit(in);
    }

    // What is there.
    const float drying = 1.0f + (kShelteredDrying - 1.0f) * (1.0f - exposure);
    const size_t before = drops_.size();
    for (size_t i = 0; i < before; ++i) {
        LensDrop& d = drops_[i];
        switch (d.kind) {
            case LensDrop::Kind::Water: {
                d.age += dt * drying;
                if (d.radius >= kRunRadius) {
                    if (d.velocity.y <= 0.0f) d.velocity.y = random(0.05f, 0.12f);
                    d.velocity.y = std::min(d.velocity.y + kRunAcceleration * dt, kRunTopSpeed);
                    d.velocity.x = std::sin(d.age * 3.0f + d.seed * 20.0f) * 0.01f;
                    d.radius *= 1.0f - kRunLossPerSecond * dt;
                    d.trailTimer -= dt;
                    if (d.trailTimer <= 0.0f) {
                        d.trailTimer = random(0.08f, 0.25f);
                        const glm::vec2 at = d.pos;
                        const float bead = d.radius * random(0.25f, 0.4f);
                        spawnWater(at, bead, random(3.0f, 6.0f));  // may move d's storage
                    }
                }
                break;
            }
            case LensDrop::Kind::Flake:
                d.age += dt * drying;
                if (d.age >= d.life) {
                    // Melted: what is left is water, already there - past
                    // the moment a new drop takes to show.
                    d.kind = LensDrop::Kind::Water;
                    d.radius *= 0.7f;
                    d.age = 0.15f;
                    d.life = random(3.0f, 6.0f);
                }
                break;
            case LensDrop::Kind::Grit:
                d.age += dt;
                break;
        }
        LensDrop& moved = drops_[i];
        moved.pos += glm::vec2(moved.velocity.x / aspect, moved.velocity.y) * dt;
    }
    std::erase_if(drops_, [](const LensDrop& d) {
        return d.age >= d.life || d.pos.y > 1.15f || d.pos.x < -0.1f || d.pos.x > 1.1f || d.pos.y < -0.2f;
    });
    mergeWater(aspect);

    // The film over the whole lens.
    const float snowing = in.snow * exposure;
    frost_ += snowing > 0.05f ? kFrostPerSecond * snowing * dt : -kFrostThawPerSecond * dt;
    frost_ = std::clamp(frost_, 0.0f, 1.0f);
    const float sanding = in.sand * exposure;
    dust_ += sanding > 0.05f ? kDustPerSecond * sanding * (0.4f + 0.6f * facingWind) * dt
                             : -kDustClearPerSecond * dt;
    dust_ = std::clamp(dust_, 0.0f, 1.0f);
}

}  // namespace rendering
}  // namespace wowee
