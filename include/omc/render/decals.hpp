#pragma once

#include <cstdint>
#include <vector>

namespace omc::render {

enum class DecalKind { Scorch, Corpse, Crater, Blood };

struct Decal {
    float x = 0, y = 0;
    DecalKind kind = DecalKind::Scorch;
    float age = 0;
    float lifetime = 1;

    float alpha() const {
        if (lifetime <= 0) return 0.0f;
        const float fade_start = lifetime * 0.75f;
        if (age <= fade_start) return 1.0f;
        if (age >= lifetime) return 0.0f;
        return 1.0f - (age - fade_start) / (lifetime - fade_start);
    }
    bool expired() const { return age >= lifetime; }
};

class DecalField {
public:
    explicit DecalField(std::size_t cap = 256) : cap_(cap) {}

    void spawn(float x, float y, DecalKind kind, float lifetime) {
        if (lifetime <= 0) return;
        if (decals_.size() >= cap_) decals_.erase(decals_.begin());
        Decal d;
        d.x = x;
        d.y = y;
        d.kind = kind;
        d.lifetime = lifetime;
        decals_.push_back(d);
    }

    void advance(float dt) {
        for (Decal& d : decals_) d.age += dt;
        std::size_t w = 0;
        for (std::size_t r = 0; r < decals_.size(); ++r)
            if (!decals_[r].expired()) decals_[w++] = decals_[r];
        decals_.resize(w);
    }

    const std::vector<Decal>& all() const { return decals_; }
    std::size_t size() const { return decals_.size(); }
    void clear() { decals_.clear(); }

private:
    std::size_t cap_;
    std::vector<Decal> decals_;
};

}
